// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Derived in part from Dynamic Reflections Fallout 4 commit
// 3c35b6927d26f1d819f4fa7e827d7b3c9b9f2cb7. Licensed under GPLv3 with
// that project's exceptions; see /THIRD_PARTY_NOTICES.md and /LICENSE.

#include "DXBCPatch.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace SIE
{
	// SM5 DXBC opcodes used by the patcher
	namespace DXBCOpcodes
	{
		constexpr uint32_t ADD = 0x00;
		constexpr uint32_t DIV = 0x0E;
		constexpr uint32_t DP3 = 0x10;
		constexpr uint32_t DP4 = 0x11;
		constexpr uint32_t EXP = 0x19;
		constexpr uint32_t LOG = 0x2F;
		constexpr uint32_t MAD = 0x32;
		constexpr uint32_t MOV = 0x36;
		constexpr uint32_t MUL = 0x38;
		constexpr uint32_t RET = 0x3E;
		constexpr uint32_t SAMPLE_L = 0x48;
		constexpr uint32_t DCL_IMMEDIATE_CB = 0x35;
		constexpr uint32_t DCL_RESOURCE = 0x58;
		constexpr uint32_t DCL_CONSTANT_BUFFER = 0x59;
		constexpr uint32_t DCL_SAMPLER = 0x5A;
		constexpr uint32_t DCL_INPUT_PS = 0x62;
		constexpr uint32_t DCL_INPUT_PS_SIV = 0x64;
		constexpr uint32_t DCL_OUTPUT = 0x65;
		constexpr uint32_t DCL_TEMPS = 0x68;
		constexpr uint32_t DCL_GLOBAL_FLAGS = 0x6A;
		// SM5 fcall (D3D11_SB_OPCODE_INTERFACE_CALL = 120). 0x77 is
		// emit_then_cut_stream, which is not dynamic linkage.
		constexpr uint32_t INTERFACE_CALL = 0x78;
		constexpr uint32_t RETC = 0x3F;
		constexpr uint32_t DCL_STREAM = 0x8F;
		constexpr uint32_t DCL_FUNCTION_BODY = 0x90;
		constexpr uint32_t DCL_FUNCTION_TABLE = 0x91;
		constexpr uint32_t DCL_INTERFACE = 0x92;
		constexpr uint32_t DCL_RESOURCE_RAW = 0xA1;
		constexpr uint32_t DCL_RESOURCE_STRUCTURED = 0xA2;
	}

	/// Parses and patches DXBC shader bytecode blobs.
	///
	/// Used to inject feature code (cloud shadows, etc.) into vanilla FXP-extracted
	/// shaders that cannot be recompiled from HLSL due to instruction scheduling divergence.
	class DXBCPatcher
	{
	public:
		/// Diagnostics for the fail-closed DFLight sunlight splice.
		struct SunShadowPatchInfo
		{
			bool signatureMatched = false;
			uint32_t insertionOffset = 0xFFFFFFFF;
			uint32_t cascadeTempRegister = 0xFFFFFFFF;
			uint32_t scaledTargetCount = 0;
			// 1/2 = flat shadowed direct+environment, 3 = flat shadowed
			// direct-only, 4/5 = flat-compatible unshadowed direct+environment;
			// 6/7 = VR shadowed direct+environment, 8/9 = VR shadowed
			// direct-only, 10 = VR unshadowed direct+environment.
			uint32_t signatureVariant = 0;
		};

		/// Apply a patch to a vanilla DXBC blob. Returns a new blob with the patch applied,
		/// or nullptr on failure. Caller owns the returned blob.
		static ID3DBlob* PatchShader(ID3DBlob* vanillaBlob, const DXBCPatch& patch);

		/// Apply the DFLight cloud factor only at the audited terminal sunlight
		/// composition site. The target must end in one of the exact vanilla
		/// directional-light layouts audited against stock FO4 1.10.163,
		/// FO4 1.11.240, and FO4 VR 1.2.72 corpora, including the equivalent
		/// compiler-normalized output suffix used by FO4VR Deferred. Light and
		/// ambient operands remain exact; other layouts are rejected.
		///
		/// `patch.preRetInstructions` contains the cloud-load payload, but is
		/// inserted at the matched body site rather than at ret. `factorTempRegister`
		/// is relative to the patch's additional-temp base and identifies the temp
		/// component containing the sampled cloud factor.
		static ID3DBlob* PatchSunShadowShader(
			ID3DBlob* vanillaBlob,
			const DXBCPatch& patch,
			uint32_t factorTempRegister,
			SunShadowPatchInfo* outInfo = nullptr);

		/// True when `data` has one of the audited terminal sunlight layouts
		/// PatchSunShadowShader accepts. Parses only; no blob or copy is made,
		/// so every shader the game creates can be classified cheaply.
		static bool IsSunShadowCandidate(const void* data, size_t size) noexcept;

		/// Compile a minimal HLSL snippet and extract its SHEX instruction bytes
		/// (excluding declarations and ret). Useful for generating injection payloads.
		/// Returns the raw DWORD sequences of the body instructions.
		static std::vector<std::vector<uint32_t>> CompileSnippetInstructions(
			const char* hlslSource,
			const char* entryPoint,
			const char* profile);

		/// Extract declaration instructions from a compiled DXBC blob's SHEX chunk.
		/// Only returns declarations matching the given opcode filter (e.g., DCL_RESOURCE).
		static std::vector<std::vector<uint32_t>> ExtractDeclarations(
			ID3DBlob* blob,
			const std::vector<uint32_t>& opcodeFilter);

		struct ParsedSHEX
		{
			const uint8_t* shexChunkStart = nullptr;  // Points to "SHEX" fourcc
			uint32_t shexChunkSize = 0;                // Size field from chunk header
			uint32_t versionToken = 0;
			const uint32_t* instrStream = nullptr;     // First instruction DWORD (after version+length)
			uint32_t instrStreamDwords = 0;            // Number of DWORDs in instruction stream

			// Key positions (DWORD offsets from instrStream start)
			uint32_t declEndOffset = 0xFFFFFFFF;          // First non-dcl instruction
			uint32_t resourceDeclEndOffset = 0xFFFFFFFF;  // First late declaration
			std::vector<uint32_t> retOffsets; // All ret instruction positions
			uint32_t tempsDeclOffset = 0xFFFFFFFF;        // dcl_temps instruction position
			uint32_t tempCount = 0;                        // Current dcl_temps value
			uint32_t svPositionRegister = 0xFFFFFFFF;     // Input register index for SV_Position
			uint32_t svPositionMask = 0;                  // Declared components of that register
			uint32_t maxInputRegister = 0xFFFFFFFF;       // Highest input register index used
			std::unordered_set<uint32_t> declaredInputRegisters;  // All registers with dcl_input_ps or dcl_input_ps_siv
			std::unordered_map<uint32_t, uint32_t> inputRegisterMasks;  // regIdx -> declared component mask
			bool hasConditionalReturn = false;            // retc present (legacy pre-ret insertion unsafe)
			std::unordered_set<uint32_t> declaredResourceRegisters;
			std::unordered_map<uint32_t, uint32_t> outputRegisterMasks;  // regIdx → component mask (x=1,y=2,z=4,w=8)
			bool usesDynamicLinkage = false;
		};

		struct ParsedDXBC
		{
			const uint8_t* data = nullptr;
			uint32_t totalSize = 0;
			uint32_t chunkCount = 0;
			// Offsets of each chunk from file start
			std::vector<uint32_t> chunkOffsets;
			// Index of SHEX/SHDR chunk (-1 if not found)
			int shexChunkIndex = -1;
			ParsedSHEX shex;

			// SV_Position register from ISGN chunk (0xFFFFFFFF if not found).
			// ISGN is the authoritative source for input register assignments.
			uint32_t isgnSvPositionRegister = 0xFFFFFFFF;
		};

		static bool Parse(const uint8_t* data, uint32_t size, ParsedDXBC& out);

	private:
		static ID3DBlob* PatchShaderInternal(
			ID3DBlob* vanillaBlob,
			const DXBCPatch& patch,
			bool strictSunShadow,
			uint32_t factorTempRegister,
			SunShadowPatchInfo* outInfo);

		static bool ParseSHEX(const uint8_t* chunkStart, uint32_t availableBytes, ParsedSHEX& out);
		static void ComputeChecksum(uint8_t* data, uint32_t size);

		static inline uint32_t GetOpcodeFromToken(uint32_t token) { return token & 0x7FF; }
		static inline uint32_t GetInstrLength(uint32_t token)
		{
			uint32_t len = (token >> 24) & 0x7F;
			// Length 0 means extended — not handled for most opcodes
			return len;
		}
		static inline bool IsDeclaration(uint32_t opcode)
		{
			// SM4 declarations plus the SM5 declaration block.
			return opcode == DXBCOpcodes::DCL_IMMEDIATE_CB ||
			       (opcode >= 0x58 && opcode <= 0x6A) ||
			       (opcode >= DXBCOpcodes::DCL_STREAM &&
			        opcode <= DXBCOpcodes::DCL_RESOURCE_STRUCTURED);
		}
	};
}
