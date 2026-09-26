// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <array>
#include <cstdint>
#include <d3d11.h>

namespace FO4CS::CloudGeometryCapture
{
    // Exact BSSky cloud technique IDs shared by the flat and VR runtimes.
    enum class CloudTechnique : std::uint32_t
    {
        kClouds = 5,
        kCloudsLerp = 6,
        kCloudsFade = 7
    };

    enum class SkyConstantLayout : std::uint32_t
    {
        // Fallout 4 1.10.163 and 1.11.240:
        // BlendColor0.w = VS b2 byte 124; TexCoordOff.xy = bytes 160..167.
        kFlat = 0,
        // Fallout 4 VR:
        // BlendColor0.w = VS b2 byte 188; TexCoordOff.xy = bytes 224..231.
        kVr = 1
    };

    // Why a replay was refused; the visible draw is never affected.
    enum class RejectReason : std::uint32_t
    {
        kNone,
        kInvalidRequest,
        kTopology,
        kExtraShaderStage,
        kClassLinkage,
        kStreamOutput,
        kPixelUav,
        kResources,
        kRasterizer,
        kDeviceRemoved,
        kException
    };

    // D3D TextureCube faces +X, -X, +Y, -Y, +Z are replayed. The -Z face lies
    // entirely below the horizon: the shell lookup and godray consumers never
    // sample it for an above-horizon sun, so it stays cleared.
    inline constexpr std::uint32_t kCapturedCubeFaceCount = 5;

    using SubmitDraw = void (*)(void* user);

    // Rasterize live opacity, including every overlapping primitive. A UV
    // mapping cannot represent multiple cloud surfaces in one native draw.
    // Targets contain the preceding layers; this source-over blends the
    // captured faces and restores the caller's complete modified state.
    [[nodiscard]] bool AccumulateOpacity(
        ID3D11DeviceContext* context,
        SkyConstantLayout layout,
        CloudTechnique technique,
        std::uint32_t faceSize,
        const std::array<ID3D11RenderTargetView*, 6>& targets,
        SubmitDraw submit, void* user,
        RejectReason* reason = nullptr) noexcept;

    void ReleaseDeviceResources() noexcept;
}
