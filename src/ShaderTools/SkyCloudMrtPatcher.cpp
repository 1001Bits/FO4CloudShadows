// SPDX-License-Identifier: GPL-3.0-only
#include "SkyCloudMrtPatcher.h"

#include "DXBCPatch.h"
#include "DXBCPatcher.h"

#include <array>
#include <cstring>
#include <limits>
#include <vector>

namespace SIE
{
	namespace
	{
		struct KnownShader
		{
			uint64_t hash;
			uint32_t size;
			SkyCloudPixelShaderVariant variant;
            uint32_t temporaryCount{ 2 };
		};

		// OG 1.10.163 and next-gen 1.11.240 contain byte-identical flat
		// cloud pixel shaders. VR 1.2.72 has the corresponding stereo forms.
		constexpr std::array kKnownShaders{
			KnownShader{ 0x8E88AA0367139E3DULL, 1008,
				SkyCloudPixelShaderVariant::kFlatClouds },
			KnownShader{ 0xD2C1A25885A8C5D8ULL, 1188,
				SkyCloudPixelShaderVariant::kFlatCloudsLerp },
			KnownShader{ 0xA4C32FA7398E1DDBULL, 1096,
				SkyCloudPixelShaderVariant::kFlatCloudsFade },
			KnownShader{ 0x0219856733164CB0ULL, 1200,
				SkyCloudPixelShaderVariant::kVRClouds },
			KnownShader{ 0x7C98FE0C571E1916ULL, 1380,
				SkyCloudPixelShaderVariant::kVRCloudsLerp },
			KnownShader{ 0x9672C2620203FD20ULL, 1288,
				SkyCloudPixelShaderVariant::kVRCloudsFade },
            // Locally authenticated FO4VR Deferred 0.5.0-dev recompilations.
            // Cloud color/alpha and signatures match stock; CB12 index math
            // is equivalent for both eyes. Private corpus fixtures compare
            // both render targets and verify these MRT payloads on the GPU.
            KnownShader{ 0x6D2B9D973AF2EC0DULL, 1944,
                SkyCloudPixelShaderVariant::kVRClouds, 3 },
            KnownShader{ 0x9D6DBBA74E764A0CULL, 2196,
                SkyCloudPixelShaderVariant::kVRCloudsLerp, 3 },
            KnownShader{ 0x585F89F70474752CULL, 2032,
                SkyCloudPixelShaderVariant::kVRCloudsFade, 3 }
		};

		constexpr bool IsPixelShader50(uint32_t versionToken) noexcept
		{
			return (versionToken >> 16) == 0 &&
				((versionToken >> 4) & 0xF) == 5 &&
				(versionToken & 0xF) == 0;
		}

		constexpr bool IsDeclaration(uint32_t opcode) noexcept
		{
			return opcode == DXBCOpcodes::DCL_IMMEDIATE_CB ||
				(opcode >= 0x58 && opcode <= 0x6A) ||
				(opcode >= DXBCOpcodes::DCL_STREAM &&
					opcode <= DXBCOpcodes::DCL_RESOURCE_STRUCTURED);
		}

		struct OutputWrites
		{
			uint32_t count = 0;
			uint32_t mask = 0;
			bool overlaps = false;
		};

		bool DecodeOutputWrites(
			const DXBCPatcher::ParsedSHEX& shex,
			std::array<OutputWrites, 2>& writes) noexcept
		{
			const uint32_t* stream = shex.instrStream;
			uint32_t position = 0;
			while (position < shex.instrStreamDwords) {
				const uint32_t opcodeToken = stream[position];
				const uint32_t opcode = opcodeToken & 0x7FF;
				uint32_t length = (opcodeToken >> 24) & 0x7F;
				if (opcode == DXBCOpcodes::DCL_IMMEDIATE_CB && length == 0 &&
					position + 1 < shex.instrStreamDwords) {
					length = stream[position + 1];
				}
				if (length == 0 || position + length > shex.instrStreamDwords)
					return false;

				if (!IsDeclaration(opcode) &&
					opcode != DXBCOpcodes::RET && length >= 2) {
					uint32_t operandPosition = position + 1;
					uint32_t extendedOpcodeToken = opcodeToken;
					while ((extendedOpcodeToken >> 31) != 0) {
						if (operandPosition >= position + length)
							return false;
						extendedOpcodeToken = stream[operandPosition++];
					}
					if (operandPosition < position + length) {
						const uint32_t operand = stream[operandPosition++];
						const uint32_t operandType = (operand >> 12) & 0xFF;
						const uint32_t indexDimensions = (operand >> 20) & 0x3;
						uint32_t extendedOperand = operand;
						while ((extendedOperand >> 31) != 0) {
							if (operandPosition >= position + length)
								return false;
							extendedOperand = stream[operandPosition++];
						}
						if (operandType == 2) {
							if (indexDimensions != 1 ||
								operandPosition >= position + length ||
								((operand >> 22) & 0x7) != 0) {
								return false;
							}
							const uint32_t outputRegister = stream[operandPosition];
							const uint32_t writeMask = (operand >> 4) & 0xF;
							if (outputRegister >= writes.size() || writeMask == 0)
								return false;
							auto& summary = writes[outputRegister];
							summary.count++;
							summary.overlaps = summary.overlaps ||
								((summary.mask & writeMask) != 0);
							summary.mask |= writeMask;
						}
					}
				}

				position += length;
			}
			return position == shex.instrStreamDwords;
		}

		bool HasStockOutputStructure(
			const DXBCPatcher::ParsedDXBC& parsed, uint64_t sourceHash) noexcept
		{
            uint32_t expectedTemporaries = 0;
            for (const auto& known : kKnownShaders)
                if (known.hash == sourceHash) expectedTemporaries = known.temporaryCount;
			const auto& shex = parsed.shex;
			if (!IsPixelShader50(shex.versionToken) ||
				shex.usesDynamicLinkage || shex.retOffsets.size() != 1 ||
				expectedTemporaries == 0 || shex.tempCount != expectedTemporaries ||
                shex.outputRegisterMasks.size() != 2) {
				return false;
			}

			const auto output0 = shex.outputRegisterMasks.find(0);
			const auto output1 = shex.outputRegisterMasks.find(1);
			if (output0 == shex.outputRegisterMasks.end() ||
				output1 == shex.outputRegisterMasks.end() ||
				output0->second != 0xF || output1->second != 0xF) {
				return false;
			}

			std::array<OutputWrites, 2> writes{};
			if (!DecodeOutputWrites(shex, writes))
				return false;
			return writes[0].count == 2 && writes[0].mask == 0xF &&
				!writes[0].overlaps &&
				writes[1].count == 2 && writes[1].mask == 0xF &&
				!writes[1].overlaps;
		}

		bool HasMappingInputContract(
			const DXBCPatcher::ParsedDXBC& parsed) noexcept
		{
			// All authenticated stock/equivalent shaders expose the animated cloud UV in
			// v1.xy and the interpolated native cloud/vertex alpha in v2.w. Parse
			// the declarations as well as authenticating the complete bytecode so
			// the injected operands can never read an undeclared component.
			std::array<uint32_t, 3> inputMasks{};
			const auto& shex = parsed.shex;
			const uint32_t* stream = shex.instrStream;
			uint32_t position = 0;
			while (position < shex.instrStreamDwords) {
				const uint32_t opcodeToken = stream[position];
				const uint32_t opcode = opcodeToken & 0x7FF;
				uint32_t length = (opcodeToken >> 24) & 0x7F;
				if (opcode == DXBCOpcodes::DCL_IMMEDIATE_CB && length == 0 &&
					position + 1 < shex.instrStreamDwords) {
					length = stream[position + 1];
				}
				if (length == 0 || position + length > shex.instrStreamDwords)
					return false;

				if ((opcode == DXBCOpcodes::DCL_INPUT_PS ||
						opcode == DXBCOpcodes::DCL_INPUT_PS_SIV) &&
					length >= 3) {
					uint32_t operandPosition = position + 1;
					uint32_t extendedOpcodeToken = opcodeToken;
					while ((extendedOpcodeToken >> 31) != 0) {
						if (operandPosition >= position + length)
							return false;
						extendedOpcodeToken = stream[operandPosition++];
					}
					if (operandPosition >= position + length)
						return false;

					const uint32_t operand = stream[operandPosition++];
					uint32_t extendedOperand = operand;
					while ((extendedOperand >> 31) != 0) {
						if (operandPosition >= position + length)
							return false;
						extendedOperand = stream[operandPosition++];
					}
					const uint32_t operandType = (operand >> 12) & 0xFF;
					const uint32_t indexDimensions = (operand >> 20) & 0x3;
					const uint32_t indexRepresentation = (operand >> 22) & 0x7;
					const uint32_t componentCount = operand & 0x3;
					const uint32_t selectionMode = (operand >> 2) & 0x3;
					if (operandType != 1 || indexDimensions != 1 ||
						indexRepresentation != 0 || componentCount != 2 ||
						selectionMode != 0 ||
						operandPosition >= position + length) {
						return false;
					}
					const uint32_t inputRegister = stream[operandPosition];
					if (inputRegister < inputMasks.size())
						inputMasks[inputRegister] |= (operand >> 4) & 0xF;
				}

				position += length;
			}

			return position == shex.instrStreamDwords &&
				(inputMasks[1] & 0x3) == 0x3 &&
				(inputMasks[2] & 0x8) == 0x8;
		}

		DXBCPatch BuildOpacityPatch()
		{
			DXBCPatch patch;
			patch.additionalTempRegisters = 2;
			patch.redirectedOutputs.emplace(0, 0);
			patch.redirectedOutputs.emplace(1, 1);

			// r0/r1 are relative to the two newly allocated registers. Existing
			// stock writes to o0 and o1 are redirected there by DXBCPatcher.
			patch.preRetInstructions = {
				{
					0x05000036,        // mov, length 5
					0x001020F2, 0,     // o0.xyzw
					0x00100E46, 0      // r0.xyzw (stock colour)
				},
				{
					0x05000036,        // mov, length 5
					0x001020F2, 1,     // o1.xyzw
					0x00100FF6, 0      // r0.wwww (stock cloud alpha)
				}
			};
			patch.tempRemapPositions = { { 4 }, { 4 } };
			patch.svPositionFixupPositions.resize(2);
			return patch;
		}

		DXBCPatch BuildUvMappingPatch()
		{
			DXBCPatch patch;
			patch.additionalTempRegisters = 2;
			patch.redirectedOutputs.emplace(0, 0);
			patch.redirectedOutputs.emplace(1, 1);

			// v1.xy is the exact texture coordinate consumed by stock t0. v2.w is
			// the exact interpolated alpha stock multiplies into cloud opacity.
			// Validity is separate because a geometrically covered pixel may have
			// a legitimate alpha of zero. Stock Target0 is recovered unchanged
			// from redirected r0; redirected stock motion in r1 is intentionally
			// replaced only for this private cubemap attachment.
			patch.preRetInstructions = {
				{
					0x05000036,        // mov, length 5
					0x001020F2, 0,     // o0.xyzw
					0x00100E46, 0      // r0.xyzw (stock colour)
				},
				{
					0x05000036,        // mov, length 5
					0x00102032, 1,     // o1.xy
					0x00101046, 1      // v1.xyxx (animated cloud UV)
				},
				{
					0x05000036,        // mov, length 5
					0x00102042, 1,     // o1.z
					0x0010103A, 2      // v2.w (native vertex alpha)
				},
				{
					0x05000036,        // mov, length 5
					0x00102082, 1,     // o1.w
					0x00004001, 0x3F800000 // l(1.0) (geometry valid)
				}
			};
			patch.tempRemapPositions = { { 4 }, {}, {}, {} };
			patch.svPositionFixupPositions.resize(4);
			return patch;
		}

		bool HasVerifiedPatchedTail(
			const DXBCPatcher::ParsedDXBC& parsed,
			uint32_t stockTempCount,
			SkyCloudMrtPayload payload) noexcept
		{
			const auto& shex = parsed.shex;
			if (shex.tempCount != stockTempCount + 2 ||
				shex.retOffsets.size() != 1 ||
				shex.retOffsets.front() < 10) {
				return false;
			}

			if (payload == SkyCloudMrtPayload::kOpacity) {
				const uint32_t tail = shex.retOffsets.front() - 10;
				const std::array<uint32_t, 11> expected{
					0x05000036, 0x001020F2, 0, 0x00100E46, stockTempCount,
					0x05000036, 0x001020F2, 1, 0x00100FF6, stockTempCount,
					0x0100003E
				};
				if (tail + expected.size() > shex.instrStreamDwords)
					return false;
				for (size_t index = 0; index < expected.size(); ++index) {
					if (shex.instrStream[tail + index] != expected[index])
						return false;
				}
			} else if (payload == SkyCloudMrtPayload::kUvMapping) {
				if (shex.retOffsets.front() < 20)
					return false;
				const uint32_t tail = shex.retOffsets.front() - 20;
				const std::array<uint32_t, 21> expected{
					0x05000036, 0x001020F2, 0, 0x00100E46, stockTempCount,
					0x05000036, 0x00102032, 1, 0x00101046, 1,
					0x05000036, 0x00102042, 1, 0x0010103A, 2,
					0x05000036, 0x00102082, 1, 0x00004001, 0x3F800000,
					0x0100003E
				};
				if (tail + expected.size() > shex.instrStreamDwords)
					return false;
				for (size_t index = 0; index < expected.size(); ++index) {
					if (shex.instrStream[tail + index] != expected[index])
						return false;
				}
			} else {
				return false;
			}
			const auto output0 = shex.outputRegisterMasks.find(0);
			const auto output1 = shex.outputRegisterMasks.find(1);
			return shex.outputRegisterMasks.size() == 2 &&
				output0 != shex.outputRegisterMasks.end() &&
				output1 != shex.outputRegisterMasks.end() &&
				output0->second == 0xF && output1->second == 0xF;
		}
	}

	uint64_t SkyCloudMrtPatcher::HashDXBC(
		const void* data, size_t size) noexcept
	{
		if (!data && size != 0)
			return 0;
		uint64_t hash = 0xCBF29CE484222325ULL;
		const auto* bytes = static_cast<const uint8_t*>(data);
		for (size_t index = 0; index < size; ++index) {
			hash ^= bytes[index];
			hash *= 0x100000001B3ULL;
		}
		return hash;
	}

	SkyCloudPixelShaderVariant SkyCloudMrtPatcher::Identify(
		const void* data, size_t size) noexcept
	{
		if (!data || size > std::numeric_limits<uint32_t>::max())
			return SkyCloudPixelShaderVariant::kUnsupported;
		const uint64_t hash = HashDXBC(data, size);
		for (const auto& shader : kKnownShaders) {
			if (shader.hash == hash && shader.size == size)
				return shader.variant;
		}
		return SkyCloudPixelShaderVariant::kUnsupported;
	}

	ID3DBlob* SkyCloudMrtPatcher::Patch(
		ID3DBlob* stockCloudShader,
		SkyCloudMrtPatchInfo* outInfo)
	{
		return Patch(
			stockCloudShader, SkyCloudMrtPayload::kOpacity, outInfo);
	}

	ID3DBlob* SkyCloudMrtPatcher::Patch(
		ID3DBlob* stockCloudShader,
		SkyCloudMrtPayload payload,
		SkyCloudMrtPatchInfo* outInfo)
	{
		SkyCloudMrtPatchInfo info{};
		info.payload = payload;
		if (!stockCloudShader) {
			if (outInfo)
				*outInfo = info;
			return nullptr;
		}

		const void* source = stockCloudShader->GetBufferPointer();
		const size_t sourceSize = stockCloudShader->GetBufferSize();
		if (sourceSize <= std::numeric_limits<uint32_t>::max())
			info.sourceSize = static_cast<uint32_t>(sourceSize);
		info.sourceHash = HashDXBC(source, sourceSize);
		info.variant = Identify(source, sourceSize);
		info.identityMatched =
			info.variant != SkyCloudPixelShaderVariant::kUnsupported;
		if (!info.identityMatched) {
			if (outInfo)
				*outInfo = info;
			return nullptr;
		}

		DXBCPatcher::ParsedDXBC parsed{};
		const bool parsedSuccessfully = DXBCPatcher::Parse(
			static_cast<const uint8_t*>(source), info.sourceSize, parsed);
		info.mappingInputsMatched = parsedSuccessfully &&
			HasMappingInputContract(parsed);
		info.structureMatched = parsedSuccessfully &&
			HasStockOutputStructure(parsed, info.sourceHash) &&
			(payload == SkyCloudMrtPayload::kOpacity ||
				(payload == SkyCloudMrtPayload::kUvMapping &&
					info.mappingInputsMatched));
		if (!info.structureMatched) {
			if (outInfo)
				*outInfo = info;
			return nullptr;
		}

		DXBCPatch patch;
		switch (payload) {
		case SkyCloudMrtPayload::kOpacity:
			patch = BuildOpacityPatch();
			break;
		case SkyCloudMrtPayload::kUvMapping:
			patch = BuildUvMappingPatch();
			break;
		default:
			if (outInfo)
				*outInfo = info;
			return nullptr;
		}

		ID3DBlob* output = DXBCPatcher::PatchShader(
			stockCloudShader, patch);
		if (output) {
			DXBCPatcher::ParsedDXBC patched{};
			info.patchVerified = DXBCPatcher::Parse(
				static_cast<const uint8_t*>(output->GetBufferPointer()),
				static_cast<uint32_t>(output->GetBufferSize()), patched) &&
				HasVerifiedPatchedTail(
					patched, parsed.shex.tempCount, payload);
			if (!info.patchVerified) {
				output->Release();
				output = nullptr;
			}
		}

		if (outInfo)
			*outInfo = info;
		return output;
	}

	ID3DBlob* SkyCloudMrtPatcher::Patch(
		const void* stockBytecode,
		size_t bytecodeSize,
		SkyCloudMrtPatchInfo* outInfo)
	{
		return Patch(
			stockBytecode, bytecodeSize,
			SkyCloudMrtPayload::kOpacity, outInfo);
	}

	ID3DBlob* SkyCloudMrtPatcher::Patch(
		const void* stockBytecode,
		size_t bytecodeSize,
		SkyCloudMrtPayload payload,
		SkyCloudMrtPatchInfo* outInfo)
	{
		if (!stockBytecode || bytecodeSize == 0 ||
			bytecodeSize > std::numeric_limits<uint32_t>::max()) {
			if (outInfo)
				*outInfo = {};
			return nullptr;
		}

		ID3DBlob* source = nullptr;
		if (FAILED(D3DCreateBlob(bytecodeSize, &source)) || !source) {
			if (outInfo)
				*outInfo = {};
			return nullptr;
		}
		std::memcpy(source->GetBufferPointer(), stockBytecode, bytecodeSize);
		ID3DBlob* output = Patch(source, payload, outInfo);
		source->Release();
		return output;
	}
}
