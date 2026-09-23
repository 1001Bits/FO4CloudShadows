#pragma once
#include <dxgi.h>

#include <cstdint>

namespace FO4CS::ManualFpsLog
{
    // Arms read-only logging on the existing F10 edge. Never changes settings
    // or toggles the renderer itself. Results go to FO4CloudShadows.log.
    void OnToggle(bool enabled) noexcept;
    void Stop() noexcept;

    // Called only after a real authoritative Present (never TEST/auxiliary).
    // frameEnabled is captured BEFORE the Present-boundary F10/settings poll,
    // so a toggle cannot label the just-rendered frame with its new state.
    void AfterPresent(IDXGISwapChain* source, bool frameEnabled,
        bool succeeded, bool maskValid, bool lightingApplied, bool vsync) noexcept;
}
