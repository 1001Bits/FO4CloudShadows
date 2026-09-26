// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <d3d11.h>
#include <algorithm>
#include <cmath>

namespace FO4CS
{
    // Call only after the bound resource is identified as the engine's main
    // Sky target. Upscalers retain that allocation while reducing its active
    // viewport. Reflections are rejected separately by target/view identity.
    [[nodiscard]] inline bool IsMainSkyViewport(const D3D11_VIEWPORT& viewport,
        UINT count, UINT width, UINT height, bool stereo) noexcept
    {
        if (count != 1 || !width || !height ||
            viewport.TopLeftX != 0.0f || viewport.TopLeftY != 0.0f ||
            !std::isfinite(viewport.Width) || !std::isfinite(viewport.Height) ||
            viewport.Width <= 0.0f || viewport.Height <= 0.0f ||
            viewport.Width > static_cast<float>(width) ||
            viewport.Height > static_cast<float>(height))
            return false;
        if (stereo)
            return (width & 1u) == 0u &&
                viewport.Width == static_cast<float>(width) &&
                viewport.Height == static_cast<float>(height);
        // Allow a pixel of rounding when the render size is scaled. Offset or
        // cropped views must not establish the player's cloud-field origin.
        return std::abs(viewport.Width * height - viewport.Height * width) <=
            static_cast<float>((std::max)(width, height));
    }
}
