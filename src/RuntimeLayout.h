#pragma once

#include "F4SECompat.h"

namespace FO4CS
{
    // Layouts are independent of addresses: NG 984 and AE 221/240 share the
    // extended BSShader and target-manager layouts, but not their RVAs.
    // Verified against the versioned executables in the Combined Ghidra project.
    struct RuntimeLayout
    {
        std::size_t shaderFilenameOffset{};
        std::size_t renderTargetMapOffset{};
        std::size_t depthTargetMapOffset{};
        std::uint32_t perCallRegisterCount{};
        bool legacyDepthProjection{};
    };

    [[nodiscard]] constexpr RuntimeLayout LayoutFor(
        F4SECompat::RuntimeTarget target) noexcept
    {
        using F4SECompat::RuntimeTarget;
        switch (target) {
        case RuntimeTarget::kLegacy:
            return { 0x110, 0xDC4, 0xF54, 28, true };
        case RuntimeTarget::kNG:
        case RuntimeTarget::kAE221:
        case RuntimeTarget::kAE:
            return { 0x188, 0xDF4, 0xF84, 25, false };
        case RuntimeTarget::kVR:
            return { 0x110, 0x13BC, 0x15FC, 46, false };
        default:
            return {};
        }
    }
}
