#include "NativeSkyCube.h"

#include "PCH.h"
#include "RuntimeAPI.h"

#include <atomic>
#include <bit>
#include <limits>
#include <mutex>
#include <TlHelp32.h>
#include <vector>

namespace
{
    using FO4CS::AddressKind;
    using FO4CS::F4SECompat::RuntimeTarget;
    using FO4CS::NativeSkyCube::FaceLifecycleCallback;
    using FO4CS::NativeSkyCube::FaceLifecycleEvent;
    using FO4CS::NativeSkyCube::FacePhase;
    using FO4CS::RuntimeAPI;
    using FO4CS::RuntimeRVA;

    struct ReviewedSymbol
    {
        std::uint64_t legacyID{};
        std::uint64_t aeID{};
        std::uintptr_t vrRva{};
    };

    // Flat IDs are exact Address Library entries. The VR RVAs come from the
    // 1.2.72 PDB/binary correspondence and are selected only after RuntimeAPI
    // has proved that the loaded image is Fallout4VR.exe 1.2.72.
    constexpr ReviewedSymbol kWaterReflectionUpdate{
        907876, 2213919, 0x07CA020
    };
    constexpr ReviewedSymbol kClickCubeMap{
        593615, 2318829, 0x2898FB0
    };
    // Passive capture never changes Fallout's scheduling, face cursor, render
    // flags, or reflection-layer settings. A generation is assembled from the
    // exact face masks supplied by naturally occurring ClickCubeMap calls.
    constexpr std::uint32_t kFullFaceMask = 0x3Fu;
    constexpr std::uint32_t kPostLoadSettleUpdateCalls = 120;

    using UpdateFn = std::uint8_t (*)(void* reflections);
    using ClickFn = void (*)(void* camera, std::uint32_t faces,
        char b3, char b4, char b5);

    UpdateFn g_originalUpdate = nullptr;
    ClickFn g_originalClick = nullptr;
    std::mutex g_installMutex;
    std::atomic<bool> g_installAttempted{ false };
    std::atomic<bool> g_installed{ false };
    std::atomic<bool> g_loadBlocked{ true };
    std::atomic<bool> g_captureConsumerReady{ false };
    std::atomic<FaceLifecycleCallback> g_lifecycleCallback{ nullptr };
    // Reset state packs a monotonically increasing 63-bit generation in bits
    // 63..1 and a pending post-load-settle request in bit 0. Keeping the reason
    // in the same atomic prevents a render-thread observer from seeing a new
    // generation before it can see whether that generation requires settling.
    // The settle bit remains sticky until the render thread acknowledges it,
    // so a later lightweight consumer reset cannot erase a pending world reset.
    constexpr std::uint64_t kResetSettlePending = 1u;
    constexpr std::uint64_t kMaxResetGeneration =
        (std::numeric_limits<std::uint64_t>::max)() >> 1u;
    std::atomic<std::uint64_t> g_requestedResetState{
        (std::uint64_t{ 1 } << 1u) | kResetSettlePending
    };
    std::atomic<bool> g_diagnosticCaptureActive{ false };
    std::atomic<std::uint32_t> g_diagnosticActiveFaceIndex{
        FO4CS::NativeSkyCube::kInvalidFaceIndex
    };
    std::atomic<std::uint32_t> g_diagnosticActiveFaceMask{ 0 };
    std::atomic<std::uint32_t> g_diagnosticStagingFaceMask{ 0 };
    std::atomic<std::uint64_t> g_diagnosticActiveSerial{ 0 };
    std::atomic<std::uint64_t> g_diagnosticStagingSerial{ 0 };
    std::atomic<std::uint64_t> g_completedCaptureSerial{ 0 };
    std::atomic<std::uint64_t> g_completedCubeCount{ 0 };
    std::atomic<std::uint64_t> g_naturalUpdateCount{ 0 };
    std::atomic<std::uint64_t> g_reflectionUpdateCount{ 0 };
    // Relaxed mirror of the render-thread-only settle counter. Without it a
    // post-load warmup that never drains is indistinguishable from a runtime
    // that never updates its water reflections at all.
    std::atomic<std::uint32_t> g_diagnosticSettleUpdateCalls{
        kPostLoadSettleUpdateCalls
    };
    std::atomic<std::uint64_t> g_skyBearingClickCount{ 0 };
    std::atomic<std::uint64_t> g_skylessClickCount{ 0 };
    std::atomic<std::uint64_t> g_authenticatedFaceCount{ 0 };
    std::atomic<std::uint64_t> g_cloudDrawFaceCount{ 0 };
    std::atomic<std::uint64_t> g_consumerAcknowledgedFaceCount{ 0 };
    std::atomic<std::uint64_t> g_consumerRejectedFaceCount{ 0 };

    // Render-thread-only producer state. Atomics above are diagnostic mirrors.
    std::uint64_t g_observedResetGeneration = 0;
    std::uint64_t g_nextCaptureSerial = 0;
    std::uint64_t g_stagingCaptureSerial = 0;
    std::uint32_t g_stagingFaceMask = 0;
    void* g_selectedCubeCamera = nullptr;
    std::uint32_t g_settleUpdateCalls = kPostLoadSettleUpdateCalls;

    thread_local std::uint32_t g_updateDepth = 0;
    thread_local std::uint32_t g_reflectionUpdateDepth = 0;
    thread_local bool g_passiveCaptureEligible = false;
    thread_local bool g_captureActive = false;
    thread_local std::uint32_t g_cloudDrawObservedMask = 0;
    thread_local std::uint32_t g_activeFaceIndex =
        FO4CS::NativeSkyCube::kInvalidFaceIndex;
    thread_local std::uint32_t g_activeFaceMask = 0;
    thread_local std::uint64_t g_activeCaptureSerial = 0;
    thread_local std::uint64_t g_activeResetGeneration = 0;
    thread_local bool g_completionAcknowledgementOpen = false;
    thread_local std::uint32_t g_completionCallbackFaceMask = 0;
    thread_local std::uint32_t g_completionAcknowledgedMask = 0;
    thread_local std::uint32_t g_completionAcceptedMask = 0;

    [[nodiscard]] std::uintptr_t ResolveReviewedSymbol(
        const ReviewedSymbol& symbol, AddressKind kind) noexcept
    {
        auto& runtime = RuntimeAPI::GetSingleton();
        switch (runtime.Target()) {
        case RuntimeTarget::kLegacy:
            return symbol.legacyID != 0 ?
                runtime.ResolveID(symbol.legacyID, kind) : 0;
        case RuntimeTarget::kAE:
            return symbol.aeID != 0 ?
                runtime.ResolveID(symbol.aeID, kind) : 0;
        case RuntimeTarget::kVR:
            return symbol.vrRva != 0 ?
                runtime.ResolveRVA(
                    RuntimeRVA{ 0, 0, symbol.vrRva }, kind) : 0;
        default:
            return 0;
        }
    }

    [[nodiscard]] bool IsUnpatchedFunctionEntry(
        std::uintptr_t address) noexcept
    {
        auto& runtime = RuntimeAPI::GetSingleton();
        if (!runtime.ValidateMemory(address, 8, AddressKind::kExecutable))
            return false;

        const auto* code = reinterpret_cast<const std::uint8_t*>(address);
        // Fail closed on common near/absolute jump encodings. A second owner
        // on an already-detoured prologue has unknowable ordering/teardown.
        return code[0] != 0xE9 && code[0] != 0xEB &&
            !(code[0] == 0xFF && code[1] == 0x25) &&
            code[0] != 0xCC && code[0] != 0x00;
    }

    class DetourThreadEnlistment
    {
    public:
        ~DetourThreadEnlistment()
        {
            for (HANDLE thread : threads_)
                CloseHandle(thread);
        }

        DetourThreadEnlistment() = default;
        DetourThreadEnlistment(const DetourThreadEnlistment&) = delete;
        DetourThreadEnlistment& operator=(
            const DetourThreadEnlistment&) = delete;

        [[nodiscard]] LONG EnlistProcessThreads() noexcept
        {
            LONG error = DetourUpdateThread(GetCurrentThread());
            if (error != NO_ERROR)
                return error;

            HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snapshot == INVALID_HANDLE_VALUE)
                return static_cast<LONG>(GetLastError());

            THREADENTRY32 entry{};
            entry.dwSize = sizeof(entry);
            if (!Thread32First(snapshot, &entry)) {
                error = static_cast<LONG>(GetLastError());
                CloseHandle(snapshot);
                return error;
            }

            const DWORD processID = GetCurrentProcessId();
            const DWORD currentThreadID = GetCurrentThreadId();
            do {
                if (entry.th32OwnerProcessID != processID ||
                    entry.th32ThreadID == currentThreadID) {
                    continue;
                }
                HANDLE thread = OpenThread(
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                        THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION,
                    FALSE, entry.th32ThreadID);
                if (!thread) {
                    const DWORD openError = GetLastError();
                    if (openError == ERROR_INVALID_PARAMETER)
                        continue;
                    error = static_cast<LONG>(openError);
                    break;
                }
                threads_.push_back(thread);
            } while (Thread32Next(snapshot, &entry));

            if (error == NO_ERROR) {
                const DWORD enumerationError = GetLastError();
                if (enumerationError != ERROR_NO_MORE_FILES)
                    error = static_cast<LONG>(enumerationError);
            }
            CloseHandle(snapshot);
            if (error != NO_ERROR)
                return error;

            for (HANDLE thread : threads_) {
                error = DetourUpdateThread(thread);
                if (error != NO_ERROR)
                    return error;
            }
            return error;
        }

    private:
        std::vector<HANDLE> threads_;
    };

    void ResetRenderThreadState(
        std::uint64_t generation, bool settleAfterLoad) noexcept
    {
        g_observedResetGeneration = generation;
        g_stagingCaptureSerial = 0;
        g_stagingFaceMask = 0;
        g_selectedCubeCamera = nullptr;
        g_settleUpdateCalls =
            settleAfterLoad ? kPostLoadSettleUpdateCalls : 0;
        g_diagnosticSettleUpdateCalls.store(
            g_settleUpdateCalls, std::memory_order_relaxed);
        g_diagnosticStagingFaceMask.store(0, std::memory_order_release);
        g_diagnosticStagingSerial.store(0, std::memory_order_release);
        g_completedCaptureSerial.store(0, std::memory_order_release);
    }

    void ResetStagingCapture() noexcept
    {
        g_stagingCaptureSerial = 0;
        g_stagingFaceMask = 0;
        g_diagnosticStagingFaceMask.store(0, std::memory_order_release);
        g_diagnosticStagingSerial.store(0, std::memory_order_release);
    }

    void ObserveRequestedReset() noexcept
    {
        const auto state = g_requestedResetState.load(
            std::memory_order_acquire);
        const auto generation = state >> 1u;
        if (generation == g_observedResetGeneration)
            return;

        const bool settleAfterLoad =
            (state & kResetSettlePending) != 0;
        ResetRenderThreadState(generation, settleAfterLoad);

        // Clear only the settle request that was observed above. If another
        // thread requested a reset meanwhile the compare/exchange fails and
        // the new request remains intact for the next Update call.
        if (settleAfterLoad) {
            auto expected = state;
            (void)g_requestedResetState.compare_exchange_strong(
                expected, state & ~kResetSettlePending,
                std::memory_order_acq_rel, std::memory_order_acquire);
        }
    }

    void RequestReset(bool settleAfterLoad) noexcept
    {
        auto state = g_requestedResetState.load(std::memory_order_acquire);
        for (;;) {
            const auto generation = state >> 1u;
            const auto nextGeneration = generation >= kMaxResetGeneration ?
                std::uint64_t{ 1 } : generation + 1u;
            const bool settlePending = settleAfterLoad ||
                (state & kResetSettlePending) != 0;
            const auto replacement = (nextGeneration << 1u) |
                (settlePending ? kResetSettlePending : 0u);
            if (g_requestedResetState.compare_exchange_weak(
                    state, replacement, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return;
            }
        }
    }

    [[nodiscard]] std::uint64_t RequestedResetGeneration() noexcept
    {
        return g_requestedResetState.load(std::memory_order_acquire) >> 1u;
    }

    [[nodiscard]] bool CaptureConsumerAvailable() noexcept
    {
        return g_captureConsumerReady.load(std::memory_order_acquire) &&
            g_lifecycleCallback.load(std::memory_order_acquire) != nullptr;
    }

    class NaturalUpdateScope
    {
    public:
        explicit NaturalUpdateScope(bool captureEligible) noexcept :
            priorCaptureEligible_(g_passiveCaptureEligible)
        {
            ++g_updateDepth;
            g_passiveCaptureEligible = captureEligible;
        }

        ~NaturalUpdateScope()
        {
            g_passiveCaptureEligible = priorCaptureEligible_;
            --g_updateDepth;
        }

        NaturalUpdateScope(const NaturalUpdateScope&) = delete;
        NaturalUpdateScope& operator=(const NaturalUpdateScope&) = delete;

    private:
        bool priorCaptureEligible_{};
    };

    class CaptureScope
    {
    public:
        CaptureScope(std::uint32_t faceMask, std::uint64_t serial,
            std::uint64_t generation) noexcept :
            callback_(g_lifecycleCallback.load(std::memory_order_acquire)),
            readyAtBegin_(
                callback_ != nullptr &&
                g_captureConsumerReady.load(std::memory_order_acquire) &&
                !g_loadBlocked.load(std::memory_order_acquire)),
            priorCaptureActive_(g_captureActive),
            priorObservedMask_(g_cloudDrawObservedMask),
            priorFaceIndex_(g_activeFaceIndex),
            priorFaceMask_(g_activeFaceMask),
            priorCaptureSerial_(g_activeCaptureSerial),
            priorResetGeneration_(g_activeResetGeneration),
            priorAcknowledgementOpen_(g_completionAcknowledgementOpen),
            priorCallbackFaceMask_(g_completionCallbackFaceMask),
            priorAcknowledgedMask_(g_completionAcknowledgedMask),
            priorAcceptedMask_(g_completionAcceptedMask)
        {
            g_captureActive = true;
            g_cloudDrawObservedMask = 0;
            g_activeFaceIndex = FO4CS::NativeSkyCube::kInvalidFaceIndex;
            g_activeFaceMask = faceMask;
            g_activeCaptureSerial = serial;
            g_activeResetGeneration = generation;

            g_diagnosticCaptureActive.store(true, std::memory_order_release);
            g_diagnosticActiveFaceIndex.store(
                FO4CS::NativeSkyCube::kInvalidFaceIndex,
                std::memory_order_release);
            g_diagnosticActiveFaceMask.store(
                faceMask, std::memory_order_release);
            g_diagnosticActiveSerial.store(
                serial, std::memory_order_release);

            NotifyFaces(FacePhase::kBegin, readyAtBegin_);
        }

        ~CaptureScope()
        {
            g_diagnosticCaptureActive.store(
                priorCaptureActive_, std::memory_order_release);
            g_diagnosticActiveFaceIndex.store(
                priorFaceIndex_, std::memory_order_release);
            g_diagnosticActiveFaceMask.store(
                priorFaceMask_, std::memory_order_release);
            g_diagnosticActiveSerial.store(
                priorCaptureSerial_, std::memory_order_release);

            g_activeResetGeneration = priorResetGeneration_;
            g_activeCaptureSerial = priorCaptureSerial_;
            g_activeFaceMask = priorFaceMask_;
            g_activeFaceIndex = priorFaceIndex_;
            g_cloudDrawObservedMask = priorObservedMask_;
            g_captureActive = priorCaptureActive_;
            g_completionAcceptedMask = priorAcceptedMask_;
            g_completionAcknowledgedMask = priorAcknowledgedMask_;
            g_completionCallbackFaceMask = priorCallbackFaceMask_;
            g_completionAcknowledgementOpen = priorAcknowledgementOpen_;
        }

        [[nodiscard]] bool Complete() noexcept
        {
            // A zero-alpha cloud draw is valid blue sky. No cloud draw at all
            // is different: it means this natural reflection path did not
            // render the game's cloud system, so accepting its cleared RT1
            // would manufacture an unauthenticated all-clear field.
            const bool cloudProducerObserved =
                (g_cloudDrawObservedMask & g_activeFaceMask) ==
                    g_activeFaceMask;
            const bool producerAuthenticated = readyAtBegin_ &&
                callback_ != nullptr &&
                cloudProducerObserved &&
                g_captureConsumerReady.load(std::memory_order_acquire) &&
                !g_loadBlocked.load(std::memory_order_acquire) &&
                RequestedResetGeneration() == g_activeResetGeneration;

            g_completionAcknowledgedMask = 0;
            g_completionAcceptedMask = 0;
            g_completionCallbackFaceMask = 0;
            g_completionAcknowledgementOpen = true;
            NotifyFaces(FacePhase::kComplete, producerAuthenticated);
            g_completionAcknowledgementOpen = false;
            g_completionCallbackFaceMask = 0;

            const auto acknowledgedMask =
                g_completionAcknowledgedMask & g_activeFaceMask;
            const auto acceptedMask =
                g_completionAcceptedMask & g_activeFaceMask;
            g_diagnosticStagingFaceMask.store(
                acknowledgedMask, std::memory_order_release);
            g_consumerAcknowledgedFaceCount.fetch_add(
                std::popcount(acknowledgedMask),
                std::memory_order_relaxed);
            g_consumerRejectedFaceCount.fetch_add(
                std::popcount(g_activeFaceMask & ~acceptedMask),
                std::memory_order_relaxed);

            const bool producerStillAuthenticated =
                producerAuthenticated &&
                g_captureConsumerReady.load(std::memory_order_acquire) &&
                !g_loadBlocked.load(std::memory_order_acquire) &&
                RequestedResetGeneration() == g_activeResetGeneration;
            const bool completed = producerStillAuthenticated &&
                acknowledgedMask == g_activeFaceMask &&
                acceptedMask == g_activeFaceMask;
            if (completed) {
                g_authenticatedFaceCount.fetch_add(
                    std::popcount(g_activeFaceMask),
                    std::memory_order_relaxed);
                g_cloudDrawFaceCount.fetch_add(
                    std::popcount(
                        g_cloudDrawObservedMask & g_activeFaceMask),
                    std::memory_order_relaxed);
            }
            return completed;
        }

        CaptureScope(const CaptureScope&) = delete;
        CaptureScope& operator=(const CaptureScope&) = delete;

    private:
        void NotifyFaces(FacePhase phase, bool authenticated) const noexcept
        {
            if (!callback_)
                return;
            for (std::uint32_t face = 0; face < 6; ++face) {
                const std::uint32_t faceBit = 1u << face;
                if ((g_activeFaceMask & faceBit) == 0)
                    continue;
                const FaceLifecycleEvent event{
                    .phase = phase,
                    .faceIndex = face,
                    .faceMask = faceBit,
                    .captureSerial = g_activeCaptureSerial,
                    .resetGeneration = g_activeResetGeneration,
                    .producerAuthenticated = authenticated,
                    .cloudDrawObserved =
                        (g_cloudDrawObservedMask & faceBit) != 0,
                };
                if (phase == FacePhase::kComplete)
                    g_completionCallbackFaceMask = faceBit;
                callback_(event);
                if (phase == FacePhase::kComplete)
                    g_completionCallbackFaceMask = 0;
            }
        }

        FaceLifecycleCallback callback_{};
        bool readyAtBegin_{};
        bool priorCaptureActive_{};
        std::uint32_t priorObservedMask_{};
        std::uint32_t priorFaceIndex_{};
        std::uint32_t priorFaceMask_{};
        std::uint64_t priorCaptureSerial_{};
        std::uint64_t priorResetGeneration_{};
        bool priorAcknowledgementOpen_{};
        std::uint32_t priorCallbackFaceMask_{};
        std::uint32_t priorAcknowledgedMask_{};
        std::uint32_t priorAcceptedMask_{};
    };

    class ReflectionUpdateScope
    {
    public:
        ReflectionUpdateScope() noexcept
        {
            ++g_reflectionUpdateDepth;
        }

        ~ReflectionUpdateScope()
        {
            --g_reflectionUpdateDepth;
        }

        ReflectionUpdateScope(const ReflectionUpdateScope&) = delete;
        ReflectionUpdateScope& operator=(const ReflectionUpdateScope&) = delete;
    };

    void RecordFullCubeCompletion(
        std::uint64_t captureSerial, bool accepted) noexcept
    {
        g_diagnosticStagingFaceMask.store(0, std::memory_order_release);
        if (!accepted)
            return;

        g_completedCaptureSerial.store(
            captureSerial, std::memory_order_release);
        g_completedCubeCount.fetch_add(1, std::memory_order_relaxed);
    }

    // BSCubeMapCamera::ClickCubeMap receives, as its final argument, the exact
    // decision TESWaterReflections::Update already made about this cube:
    // the engine passes 1 when the reflection is not mirror-flagged and
    // bReflectSky:Water is zero, which is precisely the case in which it did
    // not add the Sky root to the cube's scene list. ClickCubeMap renders
    // nothing at all when that list is empty, so such a cube can never contain
    // the BSSkyShader cloud draw that is this feature's only opacity producer.
    // Reviewed against Fallout4 1.10.163, 1.11.240 and Fallout4VR 1.2.72, whose
    // reflection updates read that setting at the same two sites.
    // Observation only: the argument is forwarded unchanged.
    void ObserveNaturalClickSkyContract(char skyExcludedByRuntime) noexcept
    {
        if (skyExcludedByRuntime == 0) {
            g_skyBearingClickCount.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        const auto count = g_skylessClickCount.fetch_add(
            1, std::memory_order_relaxed) + 1;
        if (count != 1)
            return;

        SPDLOG_WARN(
            "[CloudShadows][NativeSkyCube] the runtime renders its water "
            "reflection cubemaps without the sky, because bReflectSky:Water "
            "is 0 (the stock default). No cloud draw can occur inside the "
            "natural cubemap, so no opacity field is produced and cloud "
            "shadows stay neutral. Set bReflectSky=1 under [Water] in "
            "Fallout4.ini or Fallout4Custom.ini and restart the game.");
    }

    std::uint8_t HookWaterReflectionUpdate(void* reflections)
    {
        if (!g_originalUpdate)
            return 0;

        // Enclose every vanilla water-reflection update. Camera restoration
        // and natural update paths can otherwise expose reflection
        // CameraPosAdjust as main state.
        ReflectionUpdateScope reflectionUpdateScope;

        const bool outermost = g_updateDepth == 0;
        bool captureEligible = g_passiveCaptureEligible;
        if (outermost) {
            // Counted before every eligibility gate: a zero here means the
            // runtime never updates water reflections, which is a different
            // and earlier failure than an update this feature declined.
            g_reflectionUpdateCount.fetch_add(1, std::memory_order_relaxed);
            ObserveRequestedReset();
            captureEligible = false;
            if (reflections &&
                !g_loadBlocked.load(std::memory_order_acquire) &&
                CaptureConsumerAvailable()) {
                if (g_settleUpdateCalls != 0) {
                    --g_settleUpdateCalls;
                    g_diagnosticSettleUpdateCalls.store(
                        g_settleUpdateCalls, std::memory_order_relaxed);
                } else {
                    captureEligible = true;
                    g_naturalUpdateCount.fetch_add(
                        1, std::memory_order_relaxed);
                }
            }
        }

        // This scope changes only TLS bookkeeping. Fallout receives the exact
        // object and runs its normal scheduler/state without any mutation.
        NaturalUpdateScope updateScope(captureEligible);
        return g_originalUpdate(reflections);
    }

    void HookClickCubeMap(void* camera, std::uint32_t faces,
        char b3, char b4, char b5)
    {
        if (!g_originalClick)
            return;

        // Classify only clicks the engine itself issued from inside a natural
        // water-reflection update. A click from any other owner carries that
        // owner's arguments and says nothing about the runtime's own contract.
        if (g_reflectionUpdateDepth != 0)
            ObserveNaturalClickSkyContract(b5);

        const std::uint32_t naturalFaceMask = faces & kFullFaceMask;
        if (!g_passiveCaptureEligible || !camera || naturalFaceMask == 0 ||
            g_loadBlocked.load(std::memory_order_acquire) ||
            !CaptureConsumerAvailable()) {
            g_originalClick(camera, faces, b3, b4, b5);
            return;
        }

        // A world can own several water-reflection objects. Never assemble
        // faces from different BSCubeMapCamera instances into one opacity
        // generation; select the first natural producer until a lifecycle
        // reset proves the old world/camera is gone.
        if (!g_selectedCubeCamera)
            g_selectedCubeCamera = camera;
        if (camera != g_selectedCubeCamera) {
            g_originalClick(camera, faces, b3, b4, b5);
            return;
        }

        // A repeated face means Fallout has started a new natural cube cycle
        // before the prior one reached 0x3F. Never publish a mixed-age cube:
        // give the repeated face a fresh serial and let the consumer discard
        // the incomplete generation when it observes that new serial.
        if (g_stagingCaptureSerial != 0 &&
            (g_stagingFaceMask & naturalFaceMask) != 0) {
            ResetStagingCapture();
        }

        if (g_stagingCaptureSerial == 0) {
            if (++g_nextCaptureSerial == 0)
                ++g_nextCaptureSerial;
            g_stagingCaptureSerial = g_nextCaptureSerial;
            g_stagingFaceMask = 0;
            g_diagnosticStagingSerial.store(
                g_stagingCaptureSerial, std::memory_order_release);
        }

        const auto captureSerial = g_stagingCaptureSerial;
        CaptureScope captureScope(
            naturalFaceMask, captureSerial, g_observedResetGeneration);
        // Passive contract: preserve every argument and invoke Fallout's
        // original Click exactly once. Draw interception only appends RT1 to
        // authenticated cloud submissions made within this call.
        g_originalClick(camera, faces, b3, b4, b5);
        if (!captureScope.Complete()) {
            RecordFullCubeCompletion(captureSerial, false);
            ResetStagingCapture();
            // Do not permanently pin an unauthenticated first producer. A
            // world may expose several water-reflection cameras, and another
            // natural camera may be the one whose path actually renders Sky.
            g_selectedCubeCamera = nullptr;
            return;
        }

        g_stagingFaceMask |= naturalFaceMask;
        g_diagnosticStagingFaceMask.store(
            g_stagingFaceMask, std::memory_order_release);
        if (g_stagingFaceMask == kFullFaceMask) {
            RecordFullCubeCompletion(captureSerial, true);
            ResetStagingCapture();
        }
    }
}

namespace FO4CS::NativeSkyCube
{
    bool Install() noexcept
    {
        std::scoped_lock lock(g_installMutex);
        if (g_installed.load(std::memory_order_acquire))
            return true;
        if (g_installAttempted.exchange(true, std::memory_order_acq_rel))
            return false;

        const auto updateAddress = ResolveReviewedSymbol(
            kWaterReflectionUpdate, AddressKind::kExecutable);
        const auto clickAddress = ResolveReviewedSymbol(
            kClickCubeMap, AddressKind::kExecutable);
        if (updateAddress == 0 || clickAddress == 0 ||
            !IsUnpatchedFunctionEntry(updateAddress) ||
            !IsUnpatchedFunctionEntry(clickAddress)) {
            SPDLOG_ERROR(
                "[CloudShadows][NativeSkyCube] reviewed runtime contract "
                "unavailable or already owned (update={:#x}, click={:#x}); "
                "passive producer disabled",
                updateAddress, clickAddress);
            return false;
        }

        g_originalUpdate = reinterpret_cast<UpdateFn>(updateAddress);
        g_originalClick = reinterpret_cast<ClickFn>(clickAddress);

        DetourThreadEnlistment threads;
        LONG error = DetourTransactionBegin();
        if (error == NO_ERROR)
            error = threads.EnlistProcessThreads();
        if (error == NO_ERROR) {
            error = DetourAttach(
                reinterpret_cast<PVOID*>(&g_originalUpdate),
                reinterpret_cast<PVOID>(&HookWaterReflectionUpdate));
        }
        if (error == NO_ERROR) {
            error = DetourAttach(
                reinterpret_cast<PVOID*>(&g_originalClick),
                reinterpret_cast<PVOID>(&HookClickCubeMap));
        }
        if (error != NO_ERROR) {
            DetourTransactionAbort();
            SPDLOG_ERROR(
                "[CloudShadows][NativeSkyCube] detour transaction failed: {}",
                error);
            return false;
        }
        error = DetourTransactionCommit();
        if (error != NO_ERROR) {
            SPDLOG_ERROR(
                "[CloudShadows][NativeSkyCube] detour commit failed: {}",
                error);
            return false;
        }

        g_installed.store(true, std::memory_order_release);
        SPDLOG_INFO(
            "[CloudShadows][NativeSkyCube] passive natural-cubemap producer "
            "installed (update={:#x}, click={:#x}, zero scheduled draws)",
            updateAddress, clickAddress);
        return true;
    }

    void SetLoadBlocked(bool blocked) noexcept
    {
        const bool prior = g_loadBlocked.exchange(
            blocked, std::memory_order_acq_rel);
        if (prior != blocked)
            RequestWorldReset();
    }

    void RequestWorldReset() noexcept
    {
        RequestReset(true);
    }

    void SetFaceLifecycleCallback(
        FaceLifecycleCallback callback) noexcept
    {
        const auto prior = g_lifecycleCallback.exchange(
            callback, std::memory_order_acq_rel);
        if (prior != callback)
            RequestReset(false);
    }

    void SetCaptureConsumerReady(bool ready) noexcept
    {
        const bool prior = g_captureConsumerReady.exchange(
            ready, std::memory_order_acq_rel);
        if (prior != ready)
            // F10 and the menu master switch use this edge. They must discard
            // a partial cube, but they are not world loads and must not impose
            // the 120-update post-load settle delay when shadows are re-enabled.
            RequestReset(false);
    }

    bool IsCaptureActive() noexcept
    {
        return g_captureActive;
    }

    bool IsReflectionUpdateActive() noexcept
    {
        return g_reflectionUpdateDepth != 0;
    }

    std::uint32_t ActiveFaceMask() noexcept
    {
        return g_captureActive ? g_activeFaceMask : 0u;
    }

    std::uint32_t ActiveFaceIndex() noexcept
    {
        return g_captureActive ? g_activeFaceIndex : kInvalidFaceIndex;
    }

    std::uint32_t ResolveActiveFace(
        ID3D11DeviceContext* context) noexcept
    {
        if (!g_captureActive || !context)
            return kInvalidFaceIndex;

        ComPtr<ID3D11RenderTargetView> renderTarget;
        context->OMGetRenderTargets(
            1, renderTarget.GetAddressOf(), nullptr);
        if (!renderTarget)
            return kInvalidFaceIndex;

        D3D11_RENDER_TARGET_VIEW_DESC description{};
        renderTarget->GetDesc(&description);
        std::uint32_t face = kInvalidFaceIndex;
        switch (description.ViewDimension) {
        case D3D11_RTV_DIMENSION_TEXTURE2DARRAY:
            if (description.Texture2DArray.ArraySize == 1)
                face = description.Texture2DArray.FirstArraySlice;
            break;
        case D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY:
            if (description.Texture2DMSArray.ArraySize == 1)
                face = description.Texture2DMSArray.FirstArraySlice;
            break;
        default:
            break;
        }
        if (face >= 6 || (g_activeFaceMask & (1u << face)) == 0)
            return kInvalidFaceIndex;

        g_activeFaceIndex = face;
        g_diagnosticActiveFaceIndex.store(face, std::memory_order_release);
        return face;
    }

    std::uint64_t ActiveCaptureSerial() noexcept
    {
        return g_captureActive ? g_activeCaptureSerial : 0u;
    }

    std::uint64_t ActiveResetGeneration() noexcept
    {
        return g_captureActive ? g_activeResetGeneration : 0u;
    }

    void MarkCloudDrawCaptured(ID3D11DeviceContext* context) noexcept
    {
        const auto face = ResolveActiveFace(context);
        if (face != kInvalidFaceIndex)
            g_cloudDrawObservedMask |= 1u << face;
    }

    bool AcknowledgeFaceCompletion(
        std::uint32_t faceMask,
        std::uint64_t captureSerial,
        bool accepted) noexcept
    {
        if (!g_captureActive || !g_completionAcknowledgementOpen ||
            captureSerial == 0 || captureSerial != g_activeCaptureSerial ||
            faceMask == 0 || faceMask != g_completionCallbackFaceMask ||
            (faceMask & g_activeFaceMask) != faceMask ||
            (g_completionAcknowledgedMask & faceMask) != 0) {
            return false;
        }

        g_completionAcknowledgedMask |= faceMask;
        if (accepted)
            g_completionAcceptedMask |= faceMask;
        g_diagnosticStagingFaceMask.store(
            g_completionAcknowledgedMask, std::memory_order_release);
        return true;
    }

    Diagnostics GetDiagnostics() noexcept
    {
        return Diagnostics{
            .installAttempted = g_installAttempted.load(
                std::memory_order_acquire),
            .installed = g_installed.load(std::memory_order_acquire),
            .captureConsumerReady = g_captureConsumerReady.load(
                std::memory_order_acquire),
            .loadBlocked = g_loadBlocked.load(std::memory_order_acquire),
            .captureActive = g_diagnosticCaptureActive.load(
                std::memory_order_acquire),
            .activeFaceIndex = g_diagnosticActiveFaceIndex.load(
                std::memory_order_acquire),
            .activeFaceMask = g_diagnosticActiveFaceMask.load(
                std::memory_order_acquire),
            .stagingFaceMask = g_diagnosticStagingFaceMask.load(
                std::memory_order_acquire),
            .resetGeneration = RequestedResetGeneration(),
            .activeCaptureSerial = g_diagnosticActiveSerial.load(
                std::memory_order_acquire),
            .stagingCaptureSerial = g_diagnosticStagingSerial.load(
                std::memory_order_acquire),
            .completedCaptureSerial = g_completedCaptureSerial.load(
                std::memory_order_acquire),
            .completedCubeCount = g_completedCubeCount.load(
                std::memory_order_relaxed),
            .naturalUpdateCount = g_naturalUpdateCount.load(
                std::memory_order_relaxed),
            .reflectionUpdateCount = g_reflectionUpdateCount.load(
                std::memory_order_relaxed),
            .settleUpdatesRemaining = g_diagnosticSettleUpdateCalls.load(
                std::memory_order_relaxed),
            .skyBearingClickCount = g_skyBearingClickCount.load(
                std::memory_order_relaxed),
            .skylessClickCount = g_skylessClickCount.load(
                std::memory_order_relaxed),
            .authenticatedFaceCount = g_authenticatedFaceCount.load(
                std::memory_order_relaxed),
            .cloudDrawFaceCount = g_cloudDrawFaceCount.load(
                std::memory_order_relaxed),
            .consumerAcknowledgedFaceCount =
                g_consumerAcknowledgedFaceCount.load(
                    std::memory_order_relaxed),
            .consumerRejectedFaceCount =
                g_consumerRejectedFaceCount.load(
                    std::memory_order_relaxed),
        };
    }
}
