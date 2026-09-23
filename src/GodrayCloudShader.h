// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <DirectXMath.h>
#include <string>
#include "SunMaskProjection.h"

namespace FO4CS::GodrayCloudShader
{
    struct alignas(16) Constants
    {
        // Shell height, planet radius, opacity, enabled.
        DirectX::XMFLOAT4 geometryAndStrength{};
        // Committed origin and active captured blend.
        DirectX::XMFLOAT4 captureOriginAndBlend{};
        // Current CameraPosAdjust and accepted eye error.
        DirectX::XMFLOAT4 expectedEyeAndTolerance{};
        // Same visible-sun direction / validity as the ground shader.
        DirectX::XMFLOAT4 visibleSunDirectionAndValidity{};
        SunMaskProjection sunProjection{};
    };
    static_assert(sizeof(Constants) == 128);

    // Shared by both native payloads and numerical GPU fixtures.
    [[nodiscard]] const char* CommonSource() noexcept;
    [[nodiscard]] std::string PayloadSource(bool screenIntegral, bool sunMask = false);
}
