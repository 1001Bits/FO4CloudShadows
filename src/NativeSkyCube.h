#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace FO4CS::NativeSkyCube
{
    inline constexpr std::uint32_t kInvalidFaceIndex = 0xFFFFFFFFu;

    enum class FacePhase : std::uint8_t
    {
        kBegin,
        kComplete
    };

    // Render-thread notification for the opacity-cube owner. Each naturally
    // occurring water-reflection Click contributes its unchanged face mask;
    // the producer publishes only after a complete, non-overlapping 0x3F
    // generation. Callbacks are delivered per face in D3D TextureCube order
    // so the consumer can prepare, complete, and acknowledge each private
    // opacity slice independently.
    struct FaceLifecycleEvent
    {
        FacePhase phase{ FacePhase::kBegin };
        std::uint32_t faceIndex{ kInvalidFaceIndex };
        std::uint32_t faceMask{};
        std::uint64_t captureSerial{};
        std::uint64_t resetGeneration{};
        bool producerAuthenticated{};
        bool cloudDrawObserved{};
    };

    using FaceLifecycleCallback = void (*)(
        const FaceLifecycleEvent& event) noexcept;

    struct Diagnostics
    {
        bool installAttempted{};
        bool installed{};
        bool captureConsumerReady{};
        bool loadBlocked{};
        bool captureActive{};
        std::uint32_t activeFaceIndex{ kInvalidFaceIndex };
        std::uint32_t activeFaceMask{};
        std::uint32_t stagingFaceMask{};
        std::uint64_t resetGeneration{};
        std::uint64_t activeCaptureSerial{};
        std::uint64_t stagingCaptureSerial{};
        std::uint64_t completedCaptureSerial{};
        std::uint64_t completedCubeCount{};
        std::uint64_t naturalUpdateCount{};
        // Every outermost natural TESWaterReflections::Update, whether or not
        // this feature was eligible to observe it. Zero means the runtime is
        // not updating water reflections at all, so no natural cubemap
        // producer exists to capture from.
        std::uint64_t reflectionUpdateCount{};
        // Remaining post-load warmup updates. A value that never reaches zero
        // while reflectionUpdateCount climbs means the warmup, not the runtime,
        // is withholding capture.
        std::uint32_t settleUpdatesRemaining{};
        // Natural cube clicks classified by the engine's own sky contract.
        // TESWaterReflections::Update adds the Sky root to the reflection cube
        // only for a mirror-flagged reflection or when bReflectSky:Water is
        // non-zero, and it passes that decision to BSCubeMapCamera::ClickCubeMap
        // as its final argument. A click without the sky cannot contain the
        // BSSkyShader cloud draw this feature captures.
        std::uint64_t skyBearingClickCount{};
        std::uint64_t skylessClickCount{};
        std::uint64_t authenticatedFaceCount{};
        std::uint64_t cloudDrawFaceCount{};
        std::uint64_t consumerAcknowledgedFaceCount{};
        std::uint64_t consumerRejectedFaceCount{};
    };

    // Installs the exact TESWaterReflections::Update and
    // BSCubeMapCamera::ClickCubeMap detours for the already-proved runtime.
    // The call is idempotent and fails closed if either entry is unavailable,
    // non-executable, or already patched by an incompatible producer.
    [[nodiscard]] bool Install() noexcept;
    [[nodiscard]] inline bool InstallHooks() noexcept
    {
        return Install();
    }

    // F4SE lifecycle gates.  Pre-load must block before world teardown;
    // post-load/new-game requests a fresh generation before unblocking.
    void SetLoadBlocked(bool blocked) noexcept;
    void RequestWorldReset() noexcept;

    // The opacity-cube owner registers one render-thread callback, then marks
    // itself ready only after its shader and RTV contracts are usable. Natural
    // reflection rendering is never altered when either condition is absent.
    void SetFaceLifecycleCallback(FaceLifecycleCallback callback) noexcept;
    void SetCaptureConsumerReady(bool ready) noexcept;

    // These queries are intentionally TLS-scoped. ActiveFaceMask identifies
    // the unchanged face set of the active natural Click. Because ClickCubeMap
    // provides no reviewed per-face callback, ResolveActiveFace(context) is
    // the authoritative face query at draw time; ActiveFaceIndex is only its
    // most recent diagnostic result and must not be used to select an RTV.
    [[nodiscard]] bool IsCaptureActive() noexcept;
    // Covers the complete TESWaterReflections::Update call, including natural
    // updates and the post-Click camera-restoration tail. Main-camera state
    // must never be sampled while this broader reflection envelope is active.
    [[nodiscard]] bool IsReflectionUpdateActive() noexcept;
    [[nodiscard]] std::uint32_t ActiveFaceMask() noexcept;
    [[nodiscard]] std::uint32_t ActiveFaceIndex() noexcept;
    [[nodiscard]] std::uint32_t ResolveActiveFace(
        ID3D11DeviceContext* context) noexcept;
    [[nodiscard]] std::uint64_t ActiveCaptureSerial() noexcept;
    [[nodiscard]] std::uint64_t ActiveResetGeneration() noexcept;

    // Called by the authenticated cloud draw path after the one vanilla cloud
    // draw has executed with its stock RT0 plus the private opacity RT1. A
    // blue-sky sample is a real zero-alpha draw. A face with no cloud-system
    // draw is rejected so a reflection path that omitted Sky cannot publish a
    // manufactured all-clear field.
    void MarkCloudDrawCaptured(ID3D11DeviceContext* context) noexcept;

    // Must be called synchronously by the registered lifecycle callback for
    // every kComplete event, after the consumer has committed or rejected that
    // exact one-bit face. Missing, duplicate, wrong-serial, or rejected
    // acknowledgements prevent NativeSkyCube from completing the cube.
    [[nodiscard]] bool AcknowledgeFaceCompletion(
        std::uint32_t faceMask,
        std::uint64_t captureSerial,
        bool accepted) noexcept;

    [[nodiscard]] Diagnostics GetDiagnostics() noexcept;
}
