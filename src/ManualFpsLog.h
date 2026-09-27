#pragma once
#include <dxgi.h>

#include <cstdint>
#include "FpsTestSession.h"

namespace FO4CS::ManualFpsLog
{
    // Opt-in desktop FPS meter available in the release Development menu.
    void OnToggle(bool enabled) noexcept;
    void Stop() noexcept;
    void Start(bool automatic) noexcept;
    void HideResults() noexcept;
    bool IsActive() noexcept;
    bool IsAutomatic() noexcept;
    bool Showing() noexcept;
    bool EnabledForSave(bool live) noexcept;
    FpsTestSession::Snapshot Snapshot() noexcept;
    const char* Message() noexcept;
    // A read-only world-pass observer also runs in OFF mode during a test.
    bool NeedsWorldObservation() noexcept;
    void ObserveWorldPass() noexcept;

    // Called only after a real authoritative Present (never TEST/auxiliary).
    // frameEnabled is captured BEFORE the Present-boundary F10/settings poll,
    // so a toggle cannot label the just-rendered frame with its new state.
    void AfterPresent(IDXGISwapChain* source, bool frameEnabled,
        bool succeeded, bool maskValid, bool lightingApplied, bool vsync) noexcept;
}
