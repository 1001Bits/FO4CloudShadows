#pragma once

namespace FO4CS::WaterReflectionGuard
{
    // Installs the exact TESWaterReflections::Update observer for the
    // already-proved runtime. Idempotent and fail-closed when the entry is
    // unavailable, non-executable, or already patched by another producer.
    // Must be called from the F4SE load/lifecycle thread before rendering.
    [[nodiscard]] bool Install() noexcept;

    // TLS-scoped: true while this thread is inside a vanilla water-reflection
    // update, including its post-Click camera-restoration tail. Fallout
    // temporarily replaces CameraPosAdjust during that window, so main-camera
    // state and main-view lighting must never be sampled inside it.
    [[nodiscard]] bool IsReflectionUpdateActive() noexcept;
}
