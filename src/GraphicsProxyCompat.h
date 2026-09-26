// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Windows.h>
#include <cstdint>
#include <type_traits>
#include <wrl/client.h>

namespace FO4CS::GraphicsProxyCompat
{
    // ReShade 6.8 exposes this QueryInterface contract in source/com_utils.hpp.
    // Obtain its real D3D11 device so capture sees the bytecode *after* add-ons
    // have modified it. The game's device/context and its proxy stay intact.
    inline constexpr GUID kReShadeUnwrappedObject{
        0x7f2c9a11, 0x3b4e, 0x4d6a,
        { 0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42 }
    };

    template <class TDevice>
    [[nodiscard]] Microsoft::WRL::ComPtr<TDevice> ShaderCaptureDevice(
        TDevice* device, bool reShadeProxy) noexcept
    {
        Microsoft::WRL::ComPtr<TDevice> native;
        if (!device)
            return native;
        if (!reShadeProxy) {
            native = device;
            return native;
        }
        if (FAILED(device->QueryInterface(kReShadeUnwrappedObject,
                reinterpret_cast<void**>(native.GetAddressOf()))) ||
            native.Get() == device) {
            native.Reset();
        }
        return native;
    }

    struct ComRouteData
    {
        static constexpr uint32_t kMagic = 0x46534353u;
        static constexpr uint32_t kVersion = 2;
        uint32_t magic{ kMagic };
        uint32_t version{ kVersion };
        std::uintptr_t downstream{};
        std::uintptr_t receiver{};

        [[nodiscard]] bool Matches(std::uintptr_t object,
            std::uintptr_t thunk) const noexcept
        {
            // Wrappers can forward private data to their underlying COM object.
            // A saved method from one receiver must never be called on another.
            return magic == kMagic && version == kVersion && object &&
                receiver == object && downstream && downstream != thunk;
        }
    };
    static_assert(std::is_trivially_copyable_v<ComRouteData>);
}
