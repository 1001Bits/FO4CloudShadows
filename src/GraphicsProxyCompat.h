// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Windows.h>
#include <d3d11_1.h>
#include <atomic>
#include <cstdint>
#include <cwchar>
#include <type_traits>
#include <wrl/client.h>

namespace FO4CS::GraphicsProxyCompat
{
    // A module loaded from the system directory, even when a game-folder proxy
    // owns the same module name.
    [[nodiscard]] inline HMODULE SystemDirectoryModule(const wchar_t* name) noexcept
    {
        wchar_t path[MAX_PATH]{};
        const UINT length = GetSystemDirectoryW(path, MAX_PATH);
        if (!length || length >= MAX_PATH || wcscat_s(path, L"\\") != 0 ||
            wcscat_s(path, name) != 0)
            return nullptr;
        return GetModuleHandleW(path);
    }

    // d3d11.dll stays loaded once found.
    [[nodiscard]] inline HMODULE NativeSystemD3D11Module() noexcept
    {
        static std::atomic<HMODULE> cached{ nullptr };
        if (const HMODULE module = cached.load(std::memory_order_acquire))
            return module;
        const HMODULE module = SystemDirectoryModule(L"d3d11.dll");
        if (module)
            cached.store(module, std::memory_order_release);
        return module;
    }

    // True when the COM object's QueryInterface belongs to the runtime itself
    // (System32 d3d11.dll, or Microsoft's debug layer wrapping it) rather than
    // to a graphics wrapper. The runtime allocates a context's vtable on the
    // heap, so the vtable address does not identify the owner; its entries do.
    [[nodiscard]] inline bool IsNativeD3D11Object(const void* object) noexcept
    {
        const HMODULE native = NativeSystemD3D11Module();
        if (!object || !native)
            return false;
        const auto* vtable = *static_cast<const std::uintptr_t* const*>(object);
        // One entry, vtable | native bit: the immediate context is queried
        // several times per frame and its vtable never changes.
        static std::atomic<std::uintptr_t> cache{ 0 };
        const auto key = reinterpret_cast<std::uintptr_t>(vtable);
        const auto cached = cache.load(std::memory_order_relaxed);
        if (key && (cached & ~std::uintptr_t{ 1 }) == key)
            return (cached & 1) != 0;
        HMODULE owner = nullptr;
        const bool isNative = vtable && GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(vtable[0]), &owner) &&
            (owner == native ||
                owner == SystemDirectoryModule(L"d3d11_3SDKLayers.dll") ||
                owner == SystemDirectoryModule(L"d3d11sdklayers.dll"));
        cache.store(key | (isNative ? 1u : 0u), std::memory_order_relaxed);
        return isNative;
    }

    // D3D11.1 constant-buffer range calls are made only on the runtime's own
    // context. ENB's context wrapper forwards QueryInterface to the object it
    // wraps, so the query reaches whatever layer sits below it. In a tester's
    // OG setup (2 Oct 2026) that returned an ID3D11DeviceContext1 whose vtable
    // was already freed, and the first prepass after loading a save crashed in
    // CSGetConstantBuffers1. Fallout binds whole buffers, so callers fall back
    // to the D3D11.0 methods the game itself calls through the wrapper, and
    // the wrapper is never queried at all.
    [[nodiscard]] inline Microsoft::WRL::ComPtr<ID3D11DeviceContext1> NativeContext1(
        ID3D11DeviceContext* context) noexcept
    {
        Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context1;
        if (IsNativeD3D11Object(context) &&
            FAILED(context->QueryInterface(IID_PPV_ARGS(&context1))))
            context1.Reset();
        return context1;
    }

    // ENB forwards QueryInterface to its native device but returns a distinct
    // device pointer from D3D11CreateDevice. Compare the canonical IUnknown,
    // not the ID3D11Device interface address, before sharing output resources.
    // Wrappers with their own identity (e.g. RenderDoc) remain distinct.
    [[nodiscard]] inline bool SameComIdentity(IUnknown* first, IUnknown* second) noexcept
    {
        if (!first || !second)
            return false;
        if (first == second)
            return true;
        Microsoft::WRL::ComPtr<IUnknown> firstIdentity, secondIdentity;
        return SUCCEEDED(first->QueryInterface(IID_PPV_ARGS(&firstIdentity))) &&
            SUCCEEDED(second->QueryInterface(IID_PPV_ARGS(&secondIdentity))) &&
            firstIdentity && firstIdentity.Get() == secondIdentity.Get();
    }

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

    // A game-folder d3d11.dll that is neither ENB nor ReShade may only observe
    // the runtime (e.g. hook Present) and hand the game the native device. The
    // game then creates shaders through System32 d3d11.dll, which a name-based
    // "d3d11.dll" check rejects. Accept that module only when the game-facing
    // device itself is native: a wrapper's inner native device (ENB creates one
    // through its own D3D11CreateDevice export) must never qualify, because
    // hooking the shared native vtable under a wrapper recursed (10 Sep 2026).
    struct NativeDeviceEvidence
    {
        bool foreignD3D11Module{};  // "d3d11.dll" is not System32 d3d11.dll
        bool enbModule{};
        bool reShadeModule{};
        bool gameFacingDevice{};    // outermost create with swap chain, or RendererData
        bool nativeDeviceObject{};  // the device's vtable belongs to System32 d3d11.dll
    };

    [[nodiscard]] constexpr bool AcceptNativeD3D11BehindProxy(
        const NativeDeviceEvidence& evidence) noexcept
    {
        return evidence.foreignD3D11Module && !evidence.enbModule &&
            !evidence.reShadeModule && evidence.gameFacingDevice &&
            evidence.nativeDeviceObject;
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
