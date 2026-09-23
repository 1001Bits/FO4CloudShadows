// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Derived in part from Dynamic Reflections Fallout 4 commit
// 3c35b6927d26f1d819f4fa7e827d7b3c9b9f2cb7. Licensed under GPLv3 with
// that project's exceptions; see /THIRD_PARTY_NOTICES.md and /LICENSE.

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace SIE
{
	/// Specification for a DXBC bytecode patch.
	struct DXBCPatch
	{
		/// New resource/sampler/CB declarations to add (each is a sequence of DWORDs).
		std::vector<std::vector<uint32_t>> newDeclarations;

		/// Instructions to insert before EACH ret instruction (each is a sequence of DWORDs).
		std::vector<std::vector<uint32_t>> preRetInstructions;

		/// Minimum dcl_temps value. If the shader has fewer, it will be bumped to this.
		uint32_t minTempRegisters = 0;

		/// Number of additional temp registers the patch needs beyond what the target shader has.
		/// The patcher sets dcl_temps = max(minTempRegisters, currentTemps + additionalTempRegisters).
		uint32_t additionalTempRegisters = 0;

		/// For each preRetInstruction, the DWORD positions that contain temp register indices.
		/// The patcher adds the target shader's current dcl_temps count to these positions,
		/// remapping the patch's r0, r1, etc. to unused registers in the target shader.
		std::vector<std::vector<uint32_t>> tempRemapPositions;

		/// For each preRetInstruction, DWORD positions that contain an SV_Position input
		/// register operand. The patcher scans for dcl_input_ps_siv with system_value=position,
		/// finds the actual register index, and writes the correct operand encoding at these
		/// positions. The placeholder value at these positions should encode v0 (the patcher
		/// will adjust the register index to match the target shader).
		std::vector<std::vector<uint32_t>> svPositionFixupPositions;

		/// Output registers to redirect to temp registers.
		/// Key: output register index (0 for o0, 1 for o1)
		/// Value: additional temp register index (relative to additionalTempRegisters base)
		///
		/// The patcher scans ALL body instructions and changes destination operands
		/// targeting oN to the corresponding temp register (tempBase + value).
		/// Pre-ret instructions can then safely READ from the redirect temps
		/// and WRITE the final results to the actual output registers.
		/// This avoids reading from write-only output registers (undefined in SM5).
		std::unordered_map<uint32_t, uint32_t> redirectedOutputs;
	};
}
