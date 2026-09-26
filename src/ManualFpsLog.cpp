#include "PCH.h"
#include "ManualFpsLog.h"
#include "FrameRateWindow.h"
#include "CloudShadows.h"
#include "Overlay.h"
#include "CloudComparison.h"
#include "AcceptanceRunner.h"
#include "GodraysIntegration.h"

#include <atomic>

#if !FO4CS_ENABLE_DEVELOPER_TOOLS
// Release builds: F10 is a quick toggle with brief on-screen feedback. Frame
// measurement is a developer tool and is compiled out entirely.
namespace FO4CS::ManualFpsLog
{
    void OnToggle(bool enabled) noexcept
    {
        CloudComparison::FlashStatus(enabled ? "Cloud shadows ON (F10)" : "Cloud shadows OFF (F10)", 3.0);
    }
    void Stop() noexcept {}
    void AfterPresent(IDXGISwapChain*, bool, bool, bool, bool, bool) noexcept {}
}
#else
namespace FO4CS::ManualFpsLog
{
    namespace
    {
        std::atomic<std::uint64_t> s_request{ 0 };
        std::atomic<bool> s_armed{ false };
        std::atomic<DWORD> s_presentThread{ 0 };

        bool IsExterior() noexcept
        {
            // Loading may retire these engine objects between frames. This is
            // observation only: do not call the renderer's mutating prepass.
            __try {
                auto* player = EngineAPI::GetPlayerCharacter();
                auto* sky = EngineAPI::GetSky();
                return player && EngineAPI::IsExteriorCell(player->parentCell) &&
                    sky && EngineAPI::ReadSkyMode(sky) == EngineAPI::SkyMode::kFull;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        void Flush() noexcept
        {
            if (auto* logger = spdlog::default_logger_raw())
                logger->flush();
        }
    }

    void OnToggle(bool enabled) noexcept
    {
        if (RuntimeAPI::GetSingleton().Target() == F4SECompat::RuntimeTarget::kVR)
            return; // Desktop Present cadence is not headset FPS.
        s_request.fetch_add(1, std::memory_order_release);
        s_armed.store(true, std::memory_order_release);
        CloudComparison::ArmMeasurements();
        CloudComparison::SetStatus(fmt::format("Shadows {} | settling 3s, then measuring 10s",
            enabled ? "ON" : "OFF"));
        SPDLOG_INFO("[CloudShadows][FPS] shadows {}; settling 3s, then logging "
                    "10s averages. Keep camera fixed and menus closed. "
                    "Application FPS includes frame caps and stalls; no GPU-cost inference.",
            enabled ? "ON" : "OFF");
        Flush();
    }

    void Stop() noexcept { s_armed.store(false, std::memory_order_release); }

    void AfterPresentUnchecked(IDXGISwapChain* source, bool frameEnabled,
        bool succeeded, bool maskValid, bool lightingApplied, bool vsync);

    // Formatting allocates; a failure must never terminate the game from
    // this noexcept Present callback.
    void AfterPresent(IDXGISwapChain* source, bool frameEnabled,
        bool succeeded, bool maskValid, bool lightingApplied, bool vsync) noexcept
    {
        try {
            AfterPresentUnchecked(source, frameEnabled, succeeded, maskValid,
                lightingApplied, vsync);
        } catch (...) {
            s_armed.store(false, std::memory_order_release);
        }
    }

    void AfterPresentUnchecked(IDXGISwapChain* source, bool frameEnabled,
        bool succeeded, bool maskValid, bool lightingApplied, bool vsync)
    {
        if (!s_armed.load(std::memory_order_acquire)) return;
        const auto request = s_request.load(std::memory_order_acquire);
        if (request == 0)
            return; // No clocks, engine reads or extra logging until F10.

        // The engine can move Present between loading and gameplay threads.
        // Each thread starts a fresh window; samples never race or merge.
        thread_local FrameRateWindow window;
        const auto currentThread = GetCurrentThreadId();
        if (s_presentThread.exchange(currentThread, std::memory_order_relaxed) != currentThread)
            window = {};
        DWORD foregroundProcess = 0;
        const auto foreground = GetForegroundWindow();
        if (foreground)
            GetWindowThreadProcessId(foreground, &foregroundProcess);
        const bool eligible = succeeded &&
            foregroundProcess == GetCurrentProcessId() &&
            !Overlay::IsVisible() && CloudShadows::g_settings.DebugMode == 0.0f &&
            CloudComparison::GetPreview() == CloudComparison::Preview::Off &&
            !CloudShadows::AcceptanceRunner::IsRunning() &&
            !CloudShadows::g_singleCloudIsolationEnabled.load(std::memory_order_relaxed) &&
            frameEnabled == CloudShadows::g_shadowsEnabled.load(std::memory_order_acquire) &&
            IsExterior();
        const double now = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        FrameRateWindow::Result result;
        UINT presentCount{};
        const bool hasPresentCount = source && SUCCEEDED(source->GetLastPresentCount(&presentCount));
        const bool wasMeasuring = window.IsMeasuring();
        const auto event = window.Push({ now, request, reinterpret_cast<std::uintptr_t>(source), frameEnabled,
            eligible, maskValid, lightingApplied, vsync, presentCount, hasPresentCount,
            static_cast<double>(GetTickCount64()) * 0.001 }, result);
        if (!wasMeasuring && window.IsMeasuring()) CloudComparison::ResetTimings();
        if (event == FrameRateWindow::Event::kResult) {
            const bool working = !result.enabled ||
                (result.maskFrames == result.frames && result.lightingFrames == result.frames);
            const bool checked = result.counterMatches && result.clockMatches;
            const auto rays = GodraysIntegration::GetDiagnostics();
            const auto rayVariants = rays.authenticatedDirectionalVariants;
            const bool rayPathReady = !result.enabled || !rays.cloudOcclusionEnabled ||
                (rays.renderVolumeHookInstalled && rays.nativeConsumerSupported && rayVariants == 3);
            const auto gpu = CloudComparison::GetTimings();
            SPDLOG_INFO("[CloudShadows][FPS] shadows={} avg={:.2f} FPS mean={:.3f} ms "
                        "frames={} duration={:.3f}s max={:.3f} ms over50ms={} "
                        "maskFrames={} lightingFrames={} vsyncFrames={} request={} "
                        "dxgiPresents={} counterMatches={} clockMatches={} comparisonValid={} "
                        "gpuFrames={} gpuCaptureMs={:.4f} gpuProjectionMs={:.4f} "
                        "cpuCaptureAndPrepassMs={:.4f} captureDraws={:.2f} gpuDropped={} "
                        "godrayCloud={} godrayVariants={} godrayPathReady={}",
                result.enabled ? "ON" : "OFF", result.fps, result.meanFrameMs,
                result.frames, result.seconds, result.maxFrameMs, result.framesOver50Ms,
                result.maskFrames, result.lightingFrames, result.vsyncFrames, request,
                result.dxgiPresents, result.counterMatches, result.clockMatches, checked && working && rayPathReady,
                gpu.frames, gpu.captureMs, gpu.projectionMs, gpu.cpuMs, gpu.draws, gpu.dropped,
                rays.cloudOcclusionEnabled, rayVariants, rayPathReady);
            CloudComparison::SetStatus(fmt::format("Shadows {} | {:.1f} application FPS | {}{}{}",
                result.enabled ? "ON" : "OFF", result.fps,
                checked ? "DXGI/clock checked" : "UNVERIFIED COUNTERS",
                working ? "" : " | INVALID: cloud lighting missing",
                rayPathReady ? "" : " | INVALID: enabled godray lookup unavailable"));
            CloudComparison::ResetTimings();
            Flush();
        } else if (event == FrameRateWindow::Event::kDiscarded) {
            CloudComparison::SetStatus("Measurement paused: close menus/preview and return to the exterior.");
            CloudComparison::ResetTimings();
            SPDLOG_INFO("[CloudShadows][FPS] Window discarded: foreground, exterior, "
                        "menu/diagnostics, successful Present or stable state required. "
                        "Logging resumes after 3s settling.");
            Flush();
        } else if (eligible) {
            // Keep the last complete result readable for two seconds while the
            // next window begins. UI updates are bounded to four per second.
            static thread_local double lastStatus{};
            if (now - lastStatus >= 0.25 &&
                (!window.IsMeasuring() || window.RemainingSeconds(now) < 8.0)) {
                lastStatus = now;
                CloudComparison::SetStatus(fmt::format("Shadows {} | {} {:.1f}s remaining",
                    frameEnabled ? "ON" : "OFF",
                    window.IsMeasuring() ? "measuring" : "settling", window.RemainingSeconds(now)));
            }
        }
    }
}
#endif
