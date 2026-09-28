// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>

namespace FO4CS::CommunityShadersCompatibility
{
    // Official FO4 CS v0.1.1 cloud variants (1a0439a). Executable and signature
    // chunks were compared to builds of its BSSky source for techniques 5/6/7.
    // They preserve the flat World/BlendColor/TexCoordOff and cloud-alpha ABI.
    // Do not accept arbitrary Sky replacements merely because CS is loaded.
    [[nodiscard]] constexpr bool IsCloudPair(std::uint32_t technique,
        std::uint64_t vertex, std::uint64_t pixel) noexcept
    {
        switch (technique) {
        case 5: return vertex == 0x014133C37CD182F2ULL && pixel == 0x100A85E2A1187A02ULL;
        case 6: return vertex == 0x86C033D850129571ULL && pixel == 0x62E7E2321F6E2B63ULL;
        case 7: return vertex == 0x014133C37CD182F2ULL && pixel == 0x78B02568DA919EF7ULL;
        default: return false;
        }
    }
}
