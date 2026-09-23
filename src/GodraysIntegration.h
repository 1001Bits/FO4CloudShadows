// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "F4SECompat.h"

#include <d3d11_1.h>

#include <cstddef>
#include <cstdint>

namespace FO4CS::GodraysIntegration
{
    // Matches ID3D11Device::CreatePixelShader. The permanent device hook passes
    // its authenticated downstream entry so creating the replacement cannot
    // recurse through the vtable interceptor.
    using CreatePixelShaderFn = HRESULT(WINAPI*)(
        ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*,
        ID3D11PixelShader**);

    struct Diagnostics
    {
        bool cloudOcclusionEnabled{ false };
        bool nativeConsumerSupported{ false };
        bool nativeSettingsEnabled{ false };
        bool nativeSettingsError{ false };
        bool nativeSettingsRestartRequired{ false };
        bool renderVolumeExportResolved{ false };
        bool renderVolumeHookInstalled{ false };
        std::uint32_t authenticatedShaderObjects{ 0 };
        // Bit 0 = directional volume-geometry PS, bit 1 = directional
        // screen-integral PS. Full flat-runtime proof requires both bits.
        std::uint32_t authenticatedDirectionalVariants{ 0 };
        std::uint32_t authenticatedSunMaskVariants{ 0 };
        std::uint32_t patchFailures{ 0 };
        std::uint64_t renderVolumeCalls{ 0 };
        std::uint64_t submittedCloudOcclusionDraws{ 0 };
        std::uint64_t submittedDrawsSinceEnable{ 0 };
        std::uint64_t lastSubmittedCloudEpoch{ 0 };
    };

    // Optional cloud attenuation of native godrays, off by default. Change on
    // the render thread, like the other menu/settings mutations. The hook and
    // authenticated shader objects stay available for immediate toggling.
    [[nodiscard]] bool IsCloudOcclusionEnabled() noexcept;
    void SetCloudOcclusionEnabled(bool enabled) noexcept;
    // Called only for an explicit enable/save or initial adoption of an enabled
    // setting. Native graphics changes take effect after restarting Fallout.
    void EnsureNativeGodraysEnabled() noexcept;

    // Resolve and detour GFSDK_GodraysLib_RenderVolume by exported name. This
    // never loads the GameWorks DLL into a runtime which did not load it, and
    // never uses an executable RVA. VR deliberately reports no native consumer.
    bool TryInstall(F4SECompat::RuntimeTarget target) noexcept;

    // Called only after the stock shader object was created successfully with
    // no class linkage. Exact bytecode identity and structure are authenticated
    // before a patched child object is attached to the stock shader lifetime.
    void ObservePixelShaderCreated(
        ID3D11Device* device,
        CreatePixelShaderFn createPixelShader,
        const void* stockBytecode,
        std::size_t stockBytecodeLength,
        ID3D11PixelShader* stockShader) noexcept;

    // POD state used by the permanent driver-entry draw detours. BeginDraw is a
    // no-op unless this thread is inside the named RenderVolume export and the
    // currently bound PS is one of the two exact directional GameWorks shaders.
    struct DrawSwapState
    {
        ID3D11PixelShader* vanillaPS{ nullptr };
        ID3D11ShaderResourceView* previousT47{ nullptr };
        ID3D11SamplerState* previousS15{ nullptr };
        ID3D11Buffer* previousB13{ nullptr };
        ID3D11DeviceContext1* context1{ nullptr };
        UINT previousB13FirstConstant{ 0 };
        UINT previousB13ConstantCount{ 0 };
        bool previousB13UsedRange{ false };
        bool swapped{ false };
        std::uint64_t cloudEpoch{ 0 };
    };

    [[nodiscard]] DrawSwapState BeginDraw(
        ID3D11DeviceContext* context) noexcept;
    void EndDraw(
        ID3D11DeviceContext* context,
        DrawSwapState& state,
        bool submitted) noexcept;

    // Releases the dynamic b13 buffer and device ownership. Stock shader
    // private-data attachments own their replacement objects independently.
    void ReleaseDeviceResources() noexcept;

    [[nodiscard]] Diagnostics GetDiagnostics() noexcept;
}
