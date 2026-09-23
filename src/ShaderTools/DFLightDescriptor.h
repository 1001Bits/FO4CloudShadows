// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>

namespace SIE
{
	// The reviewed flat and VR DFLight corpora use low kind 1/2 for direct sun;
	// bit 8 identifies the clear pass. This is only a cheap coarse gate. The
	// authoritative decision remains DFLightPatcher's validated full-DXBC
	// hash/signature match.
	[[nodiscard]] constexpr bool IsPotentialDirectionalSunDescriptor(
		std::uint32_t descriptor) noexcept
	{
		constexpr std::uint32_t kClearPass = 0x00000100u;
		const std::uint32_t lightKind = descriptor & 0x7Fu;
		return (descriptor & kClearPass) == 0u &&
		       (lightKind == 1u || lightKind == 2u);
	}
}
