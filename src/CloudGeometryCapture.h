// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "CloudMotionResolver.h"
#include "SunMaskProjection.h"
#include <array>

namespace FO4CS::CloudGeometryCapture
{
    using SubmitDraw = void (*)(void* user);

    // Bootstrap a layer's six directional mappings from an authenticated main
    // Sky draw. The caller owns the generation and has opened all six faces.
    // Uses the native input geometry and VS b2, never engine scene traversal.
    // Restores all modified D3D state before returning. No CPU/GPU readback.
    [[nodiscard]] bool CaptureLayer(
        ID3D11DeviceContext* context,
        CloudMotionResolver::SkyConstantLayout layout,
        std::uint64_t stableLayerId,
        CloudMotionResolver::CloudTechnique technique,
        std::uint32_t faceSize,
        SubmitDraw submit, void* user) noexcept;

    // Rasterize live opacity, including every overlapping primitive. A UV
    // mapping cannot represent multiple cloud surfaces in one native draw.
    // Targets contain the preceding layers; this source-over blends all six
    // faces and restores the caller's complete modified graphics state.
    [[nodiscard]] bool AccumulateOpacity(
        ID3D11DeviceContext* context,
        CloudMotionResolver::SkyConstantLayout layout,
        CloudMotionResolver::CloudTechnique technique,
        std::uint32_t faceSize,
        const std::array<ID3D11RenderTargetView*, 6>& targets,
        SubmitDraw submit, void* user) noexcept;

    void ReleaseDeviceResources() noexcept;
    // False once the sun-oriented (SUN_MASK) capture vertex shader failed to
    // compile on the current device; the cubemap path is unaffected.
    [[nodiscard]] bool SunCaptureAvailable() noexcept;

    // Direct sun-oriented 2D capture, with the identical native alpha shader
    // and graphics-state restoration used by AccumulateOpacity. One reissue.
    [[nodiscard]] bool AccumulateSunOpacity(
        ID3D11DeviceContext* context,
        CloudMotionResolver::SkyConstantLayout layout,
        CloudMotionResolver::CloudTechnique technique,
        ID3D11RenderTargetView* target, const SunMaskProjection& projection,
        SubmitDraw submit, void* user) noexcept;
}
