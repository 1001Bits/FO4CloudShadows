// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <dxgi.h>
namespace FO4CS::PrivateProfile
{
    bool Active() noexcept;
    // Exact, authenticated ENB 0.501 route for the private benchmark only.
    // Normal launches retain the native-DXGI ownership requirement.
    bool IsPinnedEnbPresent(std::uintptr_t address, HMODULE owner) noexcept;
    // Called only for the authoritative game's non-TEST Present, after it
    // returns. This is private-desktop application cadence, never display FPS.
    void AfterPresent(IDXGISwapChain* chain, HRESULT result,
        bool maskValid, bool lightingApplied) noexcept;
}
