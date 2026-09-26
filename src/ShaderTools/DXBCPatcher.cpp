// SPDX-License-Identifier: GPL-3.0-only
#include "DXBCPatcher.h"

// Derived in part from Dynamic Reflections Fallout 4 commit
// 3c35b6927d26f1d819f4fa7e827d7b3c9b9f2cb7. Licensed under GPLv3 with
// that project's exceptions; see /THIRD_PARTY_NOTICES.md and /LICENSE.

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <unordered_set>

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

namespace SIE
{
	// ---- DXBC Checksum (custom MD5 variant from Wine's d3dcompiler) ----
	// The DXBC checksum at offset 0x04 is computed over the data EXCLUDING
	// the first 0x14 bytes (magic + checksum). It's a modified MD5.

	namespace
	{
		// Advances past one SM4/SM5 operand (token, extension tokens, index
		// representations). False when the operand is malformed or truncated.
		bool SkipOperand(const uint32_t* instruction, uint32_t length,
			uint32_t& position, uint32_t depth = 0)
		{
			if (position >= length || depth > 8)
				return false;
			const uint32_t token = instruction[position++];
			uint32_t extension = token;
			while ((extension >> 31) != 0) {
				if (position >= length)
					return false;
				extension = instruction[position++];
			}
			const uint32_t type = (token >> 12) & 0xFF;
			if (type == 4 || type == 5) { // immediate32 / immediate64
				const uint32_t components = token & 0x3;
				uint32_t values = components == 1 ? 1u : components == 2 ? 4u : 0u;
				if (values == 0)
					return false;
				if (type == 5)
					values *= 2;
				if (values > length - position)
					return false;
				position += values;
				return true;
			}
			const uint32_t dimensions = (token >> 20) & 0x3;
			for (uint32_t dimension = 0; dimension < dimensions; ++dimension) {
				switch ((token >> (22 + 3 * dimension)) & 0x7) {
				case 0: // immediate32
					position += 1;
					break;
				case 1: // immediate64
					position += 2;
					break;
				case 2: // relative
					if (!SkipOperand(instruction, length, position, depth + 1))
						return false;
					break;
				case 3: // immediate32 + relative
					position += 1;
					if (!SkipOperand(instruction, length, position, depth + 1))
						return false;
					break;
				case 4: // immediate64 + relative
					position += 2;
					if (!SkipOperand(instruction, length, position, depth + 1))
						return false;
					break;
				default:
					return false;
				}
				if (position > length)
					return false;
			}
			return true;
		}

		// imul, sincos, udiv, umul, uaddc, usubb and swapc write two
		// destinations. True when the second one is an output register (or
		// the instruction cannot be parsed, which is treated the same).
		bool WritesOutputInSecondDestination(
			const uint32_t* instruction, uint32_t length, uint32_t opcode)
		{
			switch (opcode) {
			case 0x26: case 0x4D: case 0x4E: case 0x51:
			case 0x84: case 0x85: case 0x8E:
				break;
			default:
				return false;
			}
			// Extended opcode tokens chain through bit 31 after the opcode token.
			uint32_t position = 1;
			uint32_t opcodeToken = instruction[0];
			while ((opcodeToken >> 31) != 0) {
				if (position >= length)
					return true;
				opcodeToken = instruction[position++];
			}
			if (!SkipOperand(instruction, length, position) || position >= length)
				return true;
			return ((instruction[position] >> 12) & 0xFF) == 2;
		}

		struct MD5State
		{
			uint32_t a, b, c, d;
			uint32_t count[2];
			uint8_t buffer[64];
		};

		inline uint32_t RotL(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

		void MD5Transform(uint32_t state[4], const uint8_t block[64])
		{
			uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
			uint32_t x[16];
			for (int i = 0; i < 16; i++)
				x[i] = static_cast<uint32_t>(block[i * 4]) |
				       (static_cast<uint32_t>(block[i * 4 + 1]) << 8) |
				       (static_cast<uint32_t>(block[i * 4 + 2]) << 16) |
				       (static_cast<uint32_t>(block[i * 4 + 3]) << 24);

#define F(b, c, d) (((b) & (c)) | (~(b) & (d)))
#define G(b, c, d) (((b) & (d)) | ((c) & ~(d)))
#define H(b, c, d) ((b) ^ (c) ^ (d))
#define I(b, c, d) ((c) ^ ((b) | ~(d)))
#define STEP(f, a, b, c, d, x, t, s) (a) += f((b), (c), (d)) + (x) + (t); (a) = RotL((a), (s)); (a) += (b)

			STEP(F, a, b, c, d, x[0], 0xd76aa478, 7);
			STEP(F, d, a, b, c, x[1], 0xe8c7b756, 12);
			STEP(F, c, d, a, b, x[2], 0x242070db, 17);
			STEP(F, b, c, d, a, x[3], 0xc1bdceee, 22);
			STEP(F, a, b, c, d, x[4], 0xf57c0faf, 7);
			STEP(F, d, a, b, c, x[5], 0x4787c62a, 12);
			STEP(F, c, d, a, b, x[6], 0xa8304613, 17);
			STEP(F, b, c, d, a, x[7], 0xfd469501, 22);
			STEP(F, a, b, c, d, x[8], 0x698098d8, 7);
			STEP(F, d, a, b, c, x[9], 0x8b44f7af, 12);
			STEP(F, c, d, a, b, x[10], 0xffff5bb1, 17);
			STEP(F, b, c, d, a, x[11], 0x895cd7be, 22);
			STEP(F, a, b, c, d, x[12], 0x6b901122, 7);
			STEP(F, d, a, b, c, x[13], 0xfd987193, 12);
			STEP(F, c, d, a, b, x[14], 0xa679438e, 17);
			STEP(F, b, c, d, a, x[15], 0x49b40821, 22);

			STEP(G, a, b, c, d, x[1], 0xf61e2562, 5);
			STEP(G, d, a, b, c, x[6], 0xc040b340, 9);
			STEP(G, c, d, a, b, x[11], 0x265e5a51, 14);
			STEP(G, b, c, d, a, x[0], 0xe9b6c7aa, 20);
			STEP(G, a, b, c, d, x[5], 0xd62f105d, 5);
			STEP(G, d, a, b, c, x[10], 0x02441453, 9);
			STEP(G, c, d, a, b, x[15], 0xd8a1e681, 14);
			STEP(G, b, c, d, a, x[4], 0xe7d3fbc8, 20);
			STEP(G, a, b, c, d, x[9], 0x21e1cde6, 5);
			STEP(G, d, a, b, c, x[14], 0xc33707d6, 9);
			STEP(G, c, d, a, b, x[3], 0xf4d50d87, 14);
			STEP(G, b, c, d, a, x[8], 0x455a14ed, 20);
			STEP(G, a, b, c, d, x[13], 0xa9e3e905, 5);
			STEP(G, d, a, b, c, x[2], 0xfcefa3f8, 9);
			STEP(G, c, d, a, b, x[7], 0x676f02d9, 14);
			STEP(G, b, c, d, a, x[12], 0x8d2a4c8a, 20);

			STEP(H, a, b, c, d, x[5], 0xfffa3942, 4);
			STEP(H, d, a, b, c, x[8], 0x8771f681, 11);
			STEP(H, c, d, a, b, x[11], 0x6d9d6122, 16);
			STEP(H, b, c, d, a, x[14], 0xfde5380c, 23);
			STEP(H, a, b, c, d, x[1], 0xa4beea44, 4);
			STEP(H, d, a, b, c, x[4], 0x4bdecfa9, 11);
			STEP(H, c, d, a, b, x[7], 0xf6bb4b60, 16);
			STEP(H, b, c, d, a, x[10], 0xbebfbc70, 23);
			STEP(H, a, b, c, d, x[13], 0x289b7ec6, 4);
			STEP(H, d, a, b, c, x[0], 0xeaa127fa, 11);
			STEP(H, c, d, a, b, x[3], 0xd4ef3085, 16);
			STEP(H, b, c, d, a, x[6], 0x04881d05, 23);
			STEP(H, a, b, c, d, x[9], 0xd9d4d039, 4);
			STEP(H, d, a, b, c, x[12], 0xe6db99e5, 11);
			STEP(H, c, d, a, b, x[15], 0x1fa27cf8, 16);
			STEP(H, b, c, d, a, x[2], 0xc4ac5665, 23);

			STEP(I, a, b, c, d, x[0], 0xf4292244, 6);
			STEP(I, d, a, b, c, x[7], 0x432aff97, 10);
			STEP(I, c, d, a, b, x[14], 0xab9423a7, 15);
			STEP(I, b, c, d, a, x[5], 0xfc93a039, 21);
			STEP(I, a, b, c, d, x[12], 0x655b59c3, 6);
			STEP(I, d, a, b, c, x[3], 0x8f0ccc92, 10);
			STEP(I, c, d, a, b, x[10], 0xffeff47d, 15);
			STEP(I, b, c, d, a, x[1], 0x85845dd1, 21);
			STEP(I, a, b, c, d, x[8], 0x6fa87e4f, 6);
			STEP(I, d, a, b, c, x[15], 0xfe2ce6e0, 10);
			STEP(I, c, d, a, b, x[6], 0xa3014314, 15);
			STEP(I, b, c, d, a, x[13], 0x4e0811a1, 21);
			STEP(I, a, b, c, d, x[4], 0xf7537e82, 6);
			STEP(I, d, a, b, c, x[11], 0xbd3af235, 10);
			STEP(I, c, d, a, b, x[2], 0x2ad7d2bb, 15);
			STEP(I, b, c, d, a, x[9], 0xeb86d391, 21);

#undef F
#undef G
#undef H
#undef I
#undef STEP

			state[0] += a;
			state[1] += b;
			state[2] += c;
			state[3] += d;
		}

		struct InstructionSpan
		{
			uint32_t offset;
			uint32_t length;
		};

		template <size_t N>
		bool InstructionEquals(
			const uint32_t* stream,
			const InstructionSpan& span,
			const std::array<uint32_t, N>& expected)
		{
			return span.length == N &&
			       std::equal(expected.begin(), expected.end(), stream + span.offset);
		}

		bool IsResourceDeclarationOpcode(uint32_t opcode)
		{
			return opcode == DXBCOpcodes::DCL_RESOURCE ||
			       opcode == DXBCOpcodes::DCL_RESOURCE_RAW ||
			       opcode == DXBCOpcodes::DCL_RESOURCE_STRUCTURED;
		}

		// Decode a declaration's first register operand only when its sole index
		// is an immediate DWORD. Anything more exotic is rejected by the strict
		// parser instead of being mistaken for a free resource slot.
		bool GetImmediateRegisterIndex(
			const uint32_t* words,
			uint32_t length,
			uint32_t operandOffset,
			uint32_t expectedOperandType,
			uint32_t& outRegister)
		{
			if (!words || operandOffset >= length)
				return false;

			const uint32_t operandToken = words[operandOffset++];
			if (((operandToken >> 12) & 0xFF) != expectedOperandType ||
				((operandToken >> 20) & 0x3) != 1 ||
				((operandToken >> 22) & 0x7) != 0) {
				return false;
			}

			uint32_t extendedToken = operandToken;
			while ((extendedToken >> 31) != 0) {
				if (operandOffset >= length)
					return false;
				extendedToken = words[operandOffset++];
			}
			if (operandOffset >= length)
				return false;
			outRegister = words[operandOffset];
			return true;
		}

		struct SunShadowSite
		{
			enum class Components : uint32_t
			{
				kXYZ,
				kY,
				kW
			};

			struct ScaleTarget
			{
				uint32_t tempRegister = 0xFFFFFFFF;
				Components components = Components::kW;
			};

			uint32_t insertionOffset = 0xFFFFFFFF;
			uint32_t cascadeTempRegister = 0xFFFFFFFF;
			uint32_t signatureVariant = 0;
			std::array<ScaleTarget, 2> scaleTargets{};
			uint32_t scaleTargetCount = 0;
		};

		bool IsPixelShader50(uint32_t versionToken)
		{
			const uint32_t programType = versionToken >> 16;
			const uint32_t major = (versionToken >> 4) & 0xF;
			const uint32_t minor = versionToken & 0xF;
			return programType == 0 && major == 5 && minor == 0;
		}

		// Exact terminal layouts audited across the stock DFLight PS families from
		// FO4 1.10.163, FO4 1.11.240, and FO4 VR 1.2.72. Flat/AE each resolve 138
		// directional descriptors (66 unique bytecodes); VR resolves 136 (64 unique).
		// Shadow-only/diagnostic shaders deliberately do not match. Raw-token
		// signatures are authoritative; descriptors are only a runtime coarse gate.
		bool FindSunShadowSite(const DXBCPatcher::ParsedSHEX& shex, SunShadowSite& out)
		{
			out = {};
			if (!IsPixelShader50(shex.versionToken) || shex.retOffsets.size() != 1 ||
				shex.usesDynamicLinkage || shex.declaredResourceRegisters.contains(47)) {
				return false;
			}

			const auto output0 = shex.outputRegisterMasks.find(0);
			const auto output1 = shex.outputRegisterMasks.find(1);
			if (output0 == shex.outputRegisterMasks.end() ||
				output1 == shex.outputRegisterMasks.end() ||
				(output0->second & 0x7) != 0x7 || (output1->second & 0x7) != 0x7) {
				return false;
			}

			std::vector<InstructionSpan> instructions;
			instructions.reserve(384);

			uint32_t pos = 0;
			while (pos < shex.instrStreamDwords) {
				uint32_t token = shex.instrStream[pos];
				uint32_t opcode = token & 0x7FF;
				uint32_t len = (token >> 24) & 0x7F;
				if (opcode == DXBCOpcodes::DCL_IMMEDIATE_CB && len == 0 &&
					(pos + 1) < shex.instrStreamDwords) {
					len = shex.instrStream[pos + 1];
				}
				if (len == 0 || pos + len > shex.instrStreamDwords)
					return false;
				instructions.push_back({ pos, len });
				pos += len;
			}

			if (pos != shex.instrStreamDwords || instructions.size() < 6)
				return false;

			static constexpr std::array<uint32_t, 5> kSpecAlpha = {
				0x05000036, 0x00102082, 0x00000001, 0x00004001, 0x3F800000
			};
			static constexpr std::array<uint32_t, 5> kDiffuseAlpha = {
				0x05000036, 0x00100082, 0x00000000, 0x00004001, 0x00000000
			};
			static constexpr std::array<uint32_t, 10> kOutputDiv = {
				0x0A00000E, 0x001020F2, 0x00000000, 0x00100E46, 0x00000000,
				0x00004002, 0x40400000, 0x40400000, 0x40400000, 0x40400000
			};
			static constexpr std::array<uint32_t, 1> kRet = { 0x0100003E };

			// FO4VR Deferred's HLSL recompilation preserves the audited sunlight
			// MAD/MUL/ADD sites but folds the final /3 into *1/3 and writes output
			// alpha separately. Recognize that exact equivalent suffix. All light
			// and environment operands below still have to match byte for byte;
			// neither arbitrary output darkening nor a descriptor-only match is
			// accepted. The actual shader is never normalized or reordered.
			static constexpr std::array<uint32_t, 10> kRecompiledOutputMul = {
				0x0A000038, 0x00102072, 0x00000000, 0x00100246, 0x00000000,
				0x00004002, 0x3EAAAAAB, 0x3EAAAAAB, 0x3EAAAAAB, 0x00000000
			};
			static constexpr std::array<uint32_t, 5> kRecompiledDiffuseAlpha = {
				0x05000036, 0x00102082, 0x00000000, 0x00004001, 0x00000000
			};
			const bool recompiledTail = instructions.size() >= 6 &&
				InstructionEquals(shex.instrStream, instructions[instructions.size() - 4], kRecompiledOutputMul) &&
				InstructionEquals(shex.instrStream, instructions[instructions.size() - 3], kRecompiledDiffuseAlpha) &&
				InstructionEquals(shex.instrStream, instructions[instructions.size() - 2], kSpecAlpha);

			const auto& ret = instructions.back();
			if (!InstructionEquals(shex.instrStream, ret, kRet) ||
				ret.offset != shex.retOffsets.front()) {
				return false;
			}

			static constexpr std::array<uint32_t, 9> kShadowedSpecVariant1 = {
				0x09000032, 0x00102072, 0x00000001, 0x00100246, 0x00000001,
				0x00100FF6, 0x00000001, 0x00100246, 0x00000009
			};
			static constexpr std::array<uint32_t, 9> kShadowedDiffuseVariant1 = {
				0x09000032, 0x00100072, 0x00000000, 0x00100246, 0x00000000,
				0x00100FF6, 0x00000001, 0x00100246, 0x00000006
			};
			static constexpr std::array<uint32_t, 9> kShadowedSpecVariant2 = {
				0x09000032, 0x00102072, 0x00000001, 0x00100246, 0x00000001,
				0x00100FF6, 0x00000001, 0x00100246, 0x00000008
			};
			static constexpr std::array<uint32_t, 9> kShadowedDiffuseVariant2 = {
				0x09000032, 0x00100072, 0x00000000, 0x00100246, 0x00000000,
					0x00100FF6, 0x00000001, 0x00100346, 0x00000002
			};
			static constexpr std::array<uint32_t, 9> kVRShadowedSpecVariant1 = {
				0x09000032, 0x00102072, 0x00000001, 0x00100246, 0x00000001,
					0x00100FF6, 0x00000001, 0x00100246, 0x0000000A
			};
			static constexpr std::array<uint32_t, 9> kVRShadowedDiffuseVariant1 = {
				0x09000032, 0x00100072, 0x00000000, 0x00100246, 0x00000000,
					0x00100FF6, 0x00000001, 0x00100246, 0x00000006
			};
			static constexpr std::array<uint32_t, 9> kVRShadowedSpecVariant2 = {
				0x09000032, 0x00102072, 0x00000001, 0x00100246, 0x00000001,
					0x00100FF6, 0x00000000, 0x00100246, 0x00000009
			};
			static constexpr std::array<uint32_t, 9> kVRShadowedDiffuseVariant2 = {
				0x09000032, 0x00100072, 0x00000000, 0x00100246, 0x00000000,
					0x00100FF6, 0x00000000, 0x00100246, 0x00000006
			};

			// Shadowed direct+environment: scale the single visibility scalar used
			// by both terminal MADs. Environment addends remain byte-for-byte vanilla.
			if (instructions.size() >= 6) {
				const auto& specMad = instructions[instructions.size() - 6];
				const auto& specAlpha = instructions[instructions.size() - 5];
				const auto& diffuseMad = instructions[instructions.size() - (recompiledTail ? 5 : 4)];
				const auto& diffuseAlpha = instructions[instructions.size() - 3];
				const auto& outputDiv = instructions[instructions.size() - 2];
				const bool commonTail = recompiledTail || (
					InstructionEquals(shex.instrStream, specAlpha, kSpecAlpha) &&
					InstructionEquals(shex.instrStream, diffuseAlpha, kDiffuseAlpha) &&
					InstructionEquals(shex.instrStream, outputDiv, kOutputDiv));
				if (commonTail &&
					InstructionEquals(shex.instrStream, specMad, kShadowedSpecVariant1) &&
					InstructionEquals(shex.instrStream, diffuseMad, kShadowedDiffuseVariant1)) {
					out.signatureVariant = 1;
				} else if (commonTail &&
					InstructionEquals(shex.instrStream, specMad, kShadowedSpecVariant2) &&
					InstructionEquals(shex.instrStream, diffuseMad, kShadowedDiffuseVariant2)) {
					out.signatureVariant = 2;
				} else if (commonTail &&
					InstructionEquals(shex.instrStream, specMad, kVRShadowedSpecVariant1) &&
					InstructionEquals(shex.instrStream, diffuseMad, kVRShadowedDiffuseVariant1)) {
					out.signatureVariant = 6;
				} else if (commonTail &&
					InstructionEquals(shex.instrStream, specMad, kVRShadowedSpecVariant2) &&
					InstructionEquals(shex.instrStream, diffuseMad, kVRShadowedDiffuseVariant2)) {
					out.signatureVariant = 7;
				}
				if (out.signatureVariant != 0) {
					out.insertionOffset = specMad.offset;
					const bool vrSecondLayout = out.signatureVariant == 7;
					out.cascadeTempRegister = vrSecondLayout ? 0 : 1;
					out.scaleTargets[0] = {
						out.cascadeTempRegister, SunShadowSite::Components::kW
					};
					out.scaleTargetCount = 1;
					return true;
				}
			}

			// Shadowed direct-only: scale the exact scalar shared by both terminal
			// direct outputs (flat r1.w; VR r0.w or r0.y). Environment is absent.
			if (instructions.size() >= 7) {
				static constexpr std::array<uint32_t, 7> kDirectSpecBuild = {
					0x07000038, 0x00100072, 0x00000001, 0x00100FF6,
					0x00000000, 0x00100246, 0x00000007
				};
				static constexpr std::array<uint32_t, 7> kDirectSpecOutput = {
					0x07000038, 0x00102072, 0x00000001, 0x00100FF6,
					0x00000001, 0x00100246, 0x00000001
				};
				static constexpr std::array<uint32_t, 7> kDirectDiffuseOutput = {
					0x07000038, 0x00100072, 0x00000000, 0x00100FF6,
					0x00000001, 0x00100246, 0x00000000
				};
				static constexpr std::array<uint32_t, 7> kVRDirectSpecBuild = {
					0x07000038, 0x00100072, 0x00000001, 0x00100006,
					0x00000001, 0x00100246, 0x00000007
				};
				static constexpr std::array<uint32_t, 7> kVRDirectSpecOutputW = {
					0x07000038, 0x00102072, 0x00000001, 0x00100FF6,
					0x00000000, 0x00100246, 0x00000001
				};
				static constexpr std::array<uint32_t, 7> kVRDirectDiffuseOutputW = {
					0x07000038, 0x00100072, 0x00000000, 0x00100FF6,
					0x00000000, 0x00100246, 0x00000000
				};
				static constexpr std::array<uint32_t, 7> kVRDirectSpecOutputY = {
					0x07000038, 0x00102072, 0x00000001, 0x00100556,
					0x00000000, 0x00100246, 0x00000001
				};
				static constexpr std::array<uint32_t, 7> kVRDirectDiffuseOutputY = {
					0x07000038, 0x00100072, 0x00000000, 0x00100556,
					0x00000000, 0x00100386, 0x00000000
				};
				const auto& specBuild = instructions[instructions.size() - 7];
				const auto& specOutput = instructions[instructions.size() - 6];
				const auto& diffuseOutput = instructions[instructions.size() - 5];
				const auto& diffuseAlpha = instructions[instructions.size() - 4];
				const auto& outputDiv = instructions[instructions.size() - 3];
				const auto& specAlpha = instructions[instructions.size() - 2];
				if (InstructionEquals(shex.instrStream, specBuild, kDirectSpecBuild) &&
					InstructionEquals(shex.instrStream, specOutput, kDirectSpecOutput) &&
					InstructionEquals(shex.instrStream, diffuseOutput, kDirectDiffuseOutput) &&
					(recompiledTail || (InstructionEquals(shex.instrStream, diffuseAlpha, kDiffuseAlpha) &&
					InstructionEquals(shex.instrStream, outputDiv, kOutputDiv) &&
					InstructionEquals(shex.instrStream, specAlpha, kSpecAlpha)))) {
					out.insertionOffset = specOutput.offset;
					out.cascadeTempRegister = 1;
					out.signatureVariant = 3;
					out.scaleTargets[0] = { 1, SunShadowSite::Components::kW };
					out.scaleTargetCount = 1;
					return true;
				}
				const bool vrCommonTail =
					InstructionEquals(shex.instrStream, specBuild, kVRDirectSpecBuild) &&
					(recompiledTail || (InstructionEquals(shex.instrStream, diffuseAlpha, kDiffuseAlpha) &&
					InstructionEquals(shex.instrStream, outputDiv, kOutputDiv) &&
					InstructionEquals(shex.instrStream, specAlpha, kSpecAlpha)));
				if (vrCommonTail &&
					InstructionEquals(shex.instrStream, specOutput, kVRDirectSpecOutputW) &&
					InstructionEquals(shex.instrStream, diffuseOutput, kVRDirectDiffuseOutputW)) {
					out.insertionOffset = specOutput.offset;
					out.cascadeTempRegister = 0;
					out.signatureVariant = 8;
					out.scaleTargets[0] = { 0, SunShadowSite::Components::kW };
					out.scaleTargetCount = 1;
					return true;
				}
				if (vrCommonTail &&
					InstructionEquals(shex.instrStream, specOutput, kVRDirectSpecOutputY) &&
					InstructionEquals(shex.instrStream, diffuseOutput, kVRDirectDiffuseOutputY)) {
					out.insertionOffset = specOutput.offset;
					out.cascadeTempRegister = 0;
					out.signatureVariant = 9;
					out.scaleTargets[0] = { 0, SunShadowSite::Components::kY };
					out.scaleTargetCount = 1;
					return true;
				}
			}

			// Unshadowed direct+environment: there is no shared cascade scalar, so
			// scale the direct specular vector and direct diffuse accumulator while
			// leaving the two environment addends untouched.
			if (instructions.size() >= 6) {
				static constexpr std::array<uint32_t, 9> kUnshadowedSpecVariant1 = {
					0x09000032, 0x00102072, 0x00000001, 0x00100246, 0x00000008,
					0x00100FF6, 0x00000000, 0x00100246, 0x00000009
				};
				static constexpr std::array<uint32_t, 7> kUnshadowedDiffuseVariant1 = {
					0x07000000, 0x00100072, 0x00000000, 0x00100246,
					0x00000000, 0x00100246, 0x00000006
				};
				static constexpr std::array<uint32_t, 9> kUnshadowedSpecVariant2 = {
					0x09000032, 0x00102072, 0x00000001, 0x00100246, 0x00000007,
					0x00100FF6, 0x00000000, 0x00100246, 0x00000008
				};
				static constexpr std::array<uint32_t, 7> kUnshadowedDiffuseVariant2 = {
					0x07000000, 0x00100072, 0x00000000, 0x00100246,
					0x00000000, 0x00100246, 0x00000005
				};
				static constexpr std::array<uint32_t, 9> kVRUnshadowedSpec = {
					0x09000032, 0x00102072, 0x00000001, 0x00100796, 0x00000008,
					0x00100FF6, 0x00000000, 0x00100246, 0x00000009
				};
				static constexpr std::array<uint32_t, 7> kVRUnshadowedDiffuse = {
					0x07000000, 0x00100072, 0x00000000, 0x00100246,
					0x00000000, 0x00100246, 0x00000006
				};
				const auto& specMad = instructions[instructions.size() - 6];
				const auto& specAlpha = instructions[instructions.size() - 5];
				const auto& diffuseAdd = instructions[instructions.size() - (recompiledTail ? 5 : 4)];
				const auto& diffuseAlpha = instructions[instructions.size() - 3];
				const auto& outputDiv = instructions[instructions.size() - 2];
				const bool commonTail = recompiledTail || (
					InstructionEquals(shex.instrStream, specAlpha, kSpecAlpha) &&
					InstructionEquals(shex.instrStream, diffuseAlpha, kDiffuseAlpha) &&
					InstructionEquals(shex.instrStream, outputDiv, kOutputDiv));
				uint32_t specRegister = 0xFFFFFFFF;
				if (commonTail &&
					InstructionEquals(shex.instrStream, specMad, kUnshadowedSpecVariant1) &&
					InstructionEquals(shex.instrStream, diffuseAdd, kUnshadowedDiffuseVariant1)) {
					out.signatureVariant = 4;
					specRegister = 8;
				} else if (commonTail &&
					InstructionEquals(shex.instrStream, specMad, kUnshadowedSpecVariant2) &&
					InstructionEquals(shex.instrStream, diffuseAdd, kUnshadowedDiffuseVariant2)) {
					out.signatureVariant = 5;
					specRegister = 7;
				} else if (commonTail &&
					InstructionEquals(shex.instrStream, specMad, kVRUnshadowedSpec) &&
					InstructionEquals(shex.instrStream, diffuseAdd, kVRUnshadowedDiffuse)) {
					out.signatureVariant = 10;
				}
				if (out.signatureVariant != 0) {
					out.insertionOffset = specMad.offset;
					out.scaleTargets[0] = out.signatureVariant == 10
						? SunShadowSite::ScaleTarget{ 0, SunShadowSite::Components::kW }
						: SunShadowSite::ScaleTarget{ specRegister, SunShadowSite::Components::kXYZ };
					out.scaleTargets[1] = { 0, SunShadowSite::Components::kXYZ };
					out.scaleTargetCount = 2;
					return true;
				}
			}

			return false;
		}

	}

	void DXBCPatcher::ComputeChecksum(uint8_t* data, uint32_t size)
	{
		// DXBC checksum: modified MD5 over bytes [0x14, end) with custom finalization.
		// Based on GPUOpen DXBCChecksum.cpp and vkd3d-proton checksum.c.
		// Differs from standard MD5 in the padding/length encoding of the final block.
		if (size <= 0x14)
			return;

		const uint8_t* hashData = data + 0x14;
		uint32_t hashSize = size - 0x14;
		uint32_t numBits = hashSize * 8;
		uint32_t numBits2 = (numBits >> 2) | 1;

		uint32_t state[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };

		// Process full 64-byte blocks
		uint32_t pos = 0;
		for (; pos + 64 <= hashSize; pos += 64)
			MD5Transform(state, hashData + pos);

		uint32_t leftover = hashSize - pos;
		const uint8_t* leftoverData = hashData + pos;

		uint8_t block[64];

		if (leftover >= 56) {
			// Path A: leftover >= 56 → needs two final blocks
			// Block 1: leftover data + 0x80 padding
			memset(block, 0, 64);
			memcpy(block, leftoverData, leftover);
			block[leftover] = 0x80;
			MD5Transform(state, block);

			// Block 2: numBits at dword[0], numBits2 at dword[15]
			memset(block, 0, 64);
			memcpy(block, &numBits, 4);
			memcpy(block + 60, &numBits2, 4);
			MD5Transform(state, block);
		} else {
			// Path B: leftover < 56 → single final block
			// Layout: [numBits(4)] [leftover(N)] [0x80] [zeros] [numBits2(4)]
			memset(block, 0, 64);
			memcpy(block, &numBits, 4);
			if (leftover > 0)
				memcpy(block + 4, leftoverData, leftover);
			block[4 + leftover] = 0x80;
			memcpy(block + 60, &numBits2, 4);
			MD5Transform(state, block);
		}

		memcpy(data + 4, state, 16);
	}

	// ---- DXBC Parser ----

	bool DXBCPatcher::ParseSHEX(
		const uint8_t* chunkStart,
		uint32_t availableBytes,
		ParsedSHEX& out)
	{
		out = {};
		if (!chunkStart || availableBytes < 16)
			return false;

		out.shexChunkStart = chunkStart;
		memcpy(&out.shexChunkSize, chunkStart + 4, 4);
		if (out.shexChunkSize < 8 || (out.shexChunkSize & 3) != 0 ||
			static_cast<uint64_t>(out.shexChunkSize) + 8 > availableBytes) {
			return false;
		}

		const uint32_t* dwords = reinterpret_cast<const uint32_t*>(chunkStart + 8);
		// First two DWORDs: version token and length
		out.versionToken = dwords[0];
		uint32_t lengthDwords = dwords[1];
		if (lengthDwords < 2 ||
			static_cast<uint64_t>(lengthDwords) * sizeof(uint32_t) != out.shexChunkSize) {
			return false;
		}
		out.instrStream = dwords + 2;
		out.instrStreamDwords = lengthDwords - 2;  // exclude version+length tokens

		uint32_t pos = 0;
		bool inDecls = true;
		bool foundLateDecl = false;
		while (pos < out.instrStreamDwords) {
			uint32_t token = out.instrStream[pos];
			uint32_t opcode = GetOpcodeFromToken(token);
			uint32_t len = GetInstrLength(token);

			// dcl_immediateConstantBuffer has customized length encoding
			if (opcode == DXBCOpcodes::DCL_IMMEDIATE_CB) {
				// Length is in the token itself but uses customized format
				// Bit 31 = extended, bits 24-30 = 0 means check next DWORD
				if (len == 0 && (pos + 1) < out.instrStreamDwords) {
					len = out.instrStream[pos + 1];
				}
			}

			if (len == 0 || static_cast<uint64_t>(pos) + len > out.instrStreamDwords) {
				logger::warn("[DXBCPatcher] Zero-length instruction at offset {}, opcode 0x{:03X}", pos, opcode);
				return false;
			}
			if (!inDecls && IsDeclaration(opcode))
				return false;

			if (opcode == DXBCOpcodes::DCL_TEMPS) {
				if (len != 2 || out.tempsDeclOffset != 0xFFFFFFFF)
					return false;
				out.tempsDeclOffset = pos;
				out.tempCount = out.instrStream[pos + 1];
				if (out.tempCount > 4096)
					return false;
			}

			const uint32_t* words = out.instrStream + pos;
			if (IsResourceDeclarationOpcode(opcode)) {
				uint32_t resourceRegister = 0;
				if (!GetImmediateRegisterIndex(words, len, 1, 7, resourceRegister) ||
					!out.declaredResourceRegisters.insert(resourceRegister).second) {
					return false;
				}
			}
			if (opcode == DXBCOpcodes::DCL_FUNCTION_BODY ||
				opcode == DXBCOpcodes::DCL_FUNCTION_TABLE ||
				opcode == DXBCOpcodes::DCL_INTERFACE ||
				opcode == DXBCOpcodes::INTERFACE_CALL) {
				out.usesDynamicLinkage = true;
			}

			// Track output register masks (dcl_output = 0x65, dcl_output_siv = 0x66).
			// Component mask in bits 7:4 of the operand token.
			if ((opcode == DXBCOpcodes::DCL_OUTPUT || opcode == 0x66 || opcode == 0x67) && len >= 3) {
				uint32_t operandToken = out.instrStream[pos + 1];
				uint32_t mask = (operandToken >> 4) & 0xF;
				uint32_t regIdx = 0;
				if (!GetImmediateRegisterIndex(words, len, 1, 2, regIdx))
					return false;
				out.outputRegisterMasks[regIdx] |= mask;
			}

			// Track input register usage and detect SV_Position.
			// dcl_input_ps (0x62) and dcl_input_ps_siv (0x64) both declare input registers.
			if ((opcode == DXBCOpcodes::DCL_INPUT_PS || opcode == DXBCOpcodes::DCL_INPUT_PS_SIV) && len >= 3) {
				if (opcode == DXBCOpcodes::DCL_INPUT_PS_SIV && len < 4)
					return false;
				uint32_t regIdx = 0;
				if (!GetImmediateRegisterIndex(words, len, 1, 1, regIdx))
					return false;
				if (out.maxInputRegister == 0xFFFFFFFF || regIdx > out.maxInputRegister)
					out.maxInputRegister = regIdx;
				out.declaredInputRegisters.insert(regIdx);
				out.inputRegisterMasks[regIdx] |= (out.instrStream[pos + 1] >> 4) & 0xF;

				// Detect SV_Position: dcl_input_ps_siv vN.xyzw, position
				// System value is in the last DWORD of the instruction. System value 1 = SV_Position.
				if (opcode == DXBCOpcodes::DCL_INPUT_PS_SIV) {
					uint32_t systemValue = out.instrStream[pos + len - 1];
					if (systemValue == 1) {  // SV_Position
						if (out.svPositionRegister != 0xFFFFFFFF &&
							out.svPositionRegister != regIdx) {
							return false;
						}
						out.svPositionRegister = regIdx;
						out.svPositionMask |= (out.instrStream[pos + 1] >> 4) & 0xF;
					}
				}
			}

			if (opcode == DXBCOpcodes::RET) {
				out.retOffsets.push_back(pos);
			}
			if (opcode == DXBCOpcodes::RETC)
				out.hasConditionalReturn = true;

			if (inDecls && !IsDeclaration(opcode)) {
				out.declEndOffset = pos;
				inDecls = false;
			}

			// Track where "late" declarations start (dcl_input_ps, dcl_output, dcl_temps)
			// New resource/sampler/CB declarations must be inserted BEFORE these.
			if (inDecls && !foundLateDecl && IsDeclaration(opcode) &&
				opcode >= DXBCOpcodes::DCL_INPUT_PS && opcode <= DXBCOpcodes::DCL_TEMPS) {
				out.resourceDeclEndOffset = pos;
				foundLateDecl = true;
			}

			pos += len;
		}

		if (out.retOffsets.empty()) {
			logger::warn("[DXBCPatcher] No ret instruction found");
			return false;
		}

		return true;
	}

	bool DXBCPatcher::Parse(const uint8_t* data, uint32_t size, ParsedDXBC& out)
	{
		out = {};
		if (!data || size < 0x20 || memcmp(data, "DXBC", 4) != 0) {
			logger::warn("[DXBCPatcher] Not a valid DXBC blob");
			return false;
		}

		out.data = data;
		memcpy(&out.totalSize, data + 0x18, 4);
		memcpy(&out.chunkCount, data + 0x1C, 4);
		if (out.totalSize != size || out.chunkCount == 0 ||
			out.chunkCount > (size - 0x20) / sizeof(uint32_t)) {
			return false;
		}

		const uint32_t headerSize = 0x20 + out.chunkCount * sizeof(uint32_t);
		struct ChunkRange
		{
			uint32_t begin;
			uint32_t end;
		};
		std::vector<ChunkRange> chunkRanges;
		chunkRanges.reserve(out.chunkCount);
		out.chunkOffsets.reserve(out.chunkCount);

		for (uint32_t i = 0; i < out.chunkCount; i++) {
			uint32_t offset;
			memcpy(&offset, data + 0x20 + i * 4, 4);
			if ((offset & 3) != 0 || offset < headerSize || offset > size - 8)
				return false;
			uint32_t chunkSize = 0;
			memcpy(&chunkSize, data + offset + 4, 4);
			const uint64_t chunkEnd = static_cast<uint64_t>(offset) + 8 + chunkSize;
			if (chunkEnd > size)
				return false;
			out.chunkOffsets.push_back(offset);
			chunkRanges.push_back({ offset, static_cast<uint32_t>(chunkEnd) });
		}
		std::sort(chunkRanges.begin(), chunkRanges.end(),
			[](const ChunkRange& left, const ChunkRange& right) {
				return left.begin < right.begin;
			});
		for (size_t i = 1; i < chunkRanges.size(); i++) {
			if (chunkRanges[i].begin < chunkRanges[i - 1].end)
				return false;
		}

		bool foundInputSignature = false;
		for (uint32_t i = 0; i < out.chunkCount; i++) {
			const uint32_t offset = out.chunkOffsets[i];
			uint32_t chunkSize = 0;
			memcpy(&chunkSize, data + offset + 4, 4);
			if (memcmp(data + offset, "SHEX", 4) == 0 || memcmp(data + offset, "SHDR", 4) == 0) {
				if (out.shexChunkIndex >= 0)
					return false;
				out.shexChunkIndex = static_cast<int>(i);
				if (!ParseSHEX(data + offset, 8 + chunkSize, out.shex))
					return false;
			}

				// Parse ISGN (Input Signature) to find SV_Position register assignment.
				// ISGN format: [FourCC(4)][ChunkSize(4)][ElementCount(4)][Pad(4)][Elements...]
				// Each element: [NameOffset(4)][SemanticIndex(4)][SystemValueType(4)]
				//               [ComponentType(4)][Register(4)][Mask(1)][ReadWriteMask(1)][Pad(2)]
			if (memcmp(data + offset, "ISGN", 4) == 0) {
				if (foundInputSignature || chunkSize < 8)
					return false;
				foundInputSignature = true;
					const uint8_t* isgnData = data + offset + 8;  // After FourCC + size
				uint32_t elementCount;
				memcpy(&elementCount, isgnData, 4);
				if (elementCount > (chunkSize - 8) / 24)
					return false;
						// Elements start at isgnData + 8 (after count + padding)
						const uint8_t* elemBase = isgnData + 8;
						for (uint32_t e = 0; e < elementCount; e++) {
							const uint8_t* elem = elemBase + e * 24;  // Each element is 24 bytes
							uint32_t systemValue;
							memcpy(&systemValue, elem + 8, 4);
							if (systemValue == 1) {  // SV_Position
								uint32_t reg;
								memcpy(&reg, elem + 16, 4);
								if (out.isgnSvPositionRegister != 0xFFFFFFFF &&
									out.isgnSvPositionRegister != reg) {
									return false;
								}
								out.isgnSvPositionRegister = reg;
							}
						}
			}
		}

		if (out.shexChunkIndex < 0) {
			logger::warn("[DXBCPatcher] No SHEX/SHDR chunk found");
			return false;
		}

		return true;
	}

	// ---- Patcher ----

	ID3DBlob* DXBCPatcher::PatchShader(ID3DBlob* vanillaBlob, const DXBCPatch& patch)
	{
		return PatchShaderInternal(vanillaBlob, patch, false, 0, nullptr);
	}

	bool DXBCPatcher::IsSunShadowCandidate(const void* data, size_t size) noexcept
	{
		try {
			if (!data || size == 0 || size > std::numeric_limits<uint32_t>::max())
				return false;
			ParsedDXBC parsed;
			if (!Parse(static_cast<const uint8_t*>(data), static_cast<uint32_t>(size), parsed))
				return false;
			if (parsed.shex.svPositionRegister != 0xFFFFFFFF &&
				((parsed.isgnSvPositionRegister != 0xFFFFFFFF &&
				  parsed.shex.svPositionRegister != parsed.isgnSvPositionRegister) ||
				 (parsed.shex.svPositionMask & 0x3u) != 0x3u))
				return false;
			SunShadowSite site;
			return FindSunShadowSite(parsed.shex, site);
		} catch (...) {
			return false;
		}
	}

	ID3DBlob* DXBCPatcher::PatchSunShadowShader(
		ID3DBlob* vanillaBlob,
		const DXBCPatch& patch,
		uint32_t factorTempRegister,
		SunShadowPatchInfo* outInfo)
	{
		if (outInfo)
			*outInfo = {};
		return PatchShaderInternal(vanillaBlob, patch, true, factorTempRegister, outInfo);
	}

	ID3DBlob* DXBCPatcher::PatchShaderInternal(
		ID3DBlob* vanillaBlob,
		const DXBCPatch& patch,
		bool strictSunShadow,
		uint32_t factorTempRegister,
		SunShadowPatchInfo* outInfo)
	{
		if (!vanillaBlob)
			return nullptr;

		const auto* srcData = static_cast<const uint8_t*>(vanillaBlob->GetBufferPointer());
		const size_t blobSize = vanillaBlob->GetBufferSize();
		if (!srcData || blobSize > std::numeric_limits<uint32_t>::max())
			return nullptr;
		uint32_t srcSize = static_cast<uint32_t>(blobSize);

		ParsedDXBC parsed;
		if (!Parse(srcData, srcSize, parsed))
			return nullptr;
		if (parsed.shex.svPositionRegister != 0xFFFFFFFF &&
			parsed.isgnSvPositionRegister != 0xFFFFFFFF &&
			parsed.shex.svPositionRegister != parsed.isgnSvPositionRegister) {
			return nullptr;
		}
		// The payload reads SV_Position.xy. An existing declaration that omits
		// either component cannot be reused; fail closed before classifying.
		if (parsed.shex.svPositionRegister != 0xFFFFFFFF &&
			(parsed.shex.svPositionMask & 0x3u) != 0x3u) {
			return nullptr;
		}
		// Legacy callers insert before `ret` only; a conditional return would
		// skip the payload on that path, so such shaders are refused.
		if (!strictSunShadow && parsed.shex.hasConditionalReturn)
			return nullptr;

		SunShadowSite sunSite;
		if (strictSunShadow) {
			if (!FindSunShadowSite(parsed.shex, sunSite))
				return nullptr;

			if (outInfo) {
				outInfo->signatureMatched = true;
				outInfo->insertionOffset = sunSite.insertionOffset;
				outInfo->cascadeTempRegister = sunSite.cascadeTempRegister;
				outInfo->scaledTargetCount = sunSite.scaleTargetCount;
				outInfo->signatureVariant = sunSite.signatureVariant;
			}

			// Strict sunlight patches are a single t47 load and never redirect or
			// post-multiply an output. Validate this public API contract here too,
			// rather than relying solely on DFLightPatcher's snippet compiler.
			uint32_t patchResourceRegister = 0xFFFFFFFF;
			if (patch.newDeclarations.size() != 1 ||
				patch.newDeclarations.front().empty() ||
				GetOpcodeFromToken(patch.newDeclarations.front().front()) !=
					DXBCOpcodes::DCL_RESOURCE ||
				GetInstrLength(patch.newDeclarations.front().front()) !=
					patch.newDeclarations.front().size() ||
				!GetImmediateRegisterIndex(
					patch.newDeclarations.front().data(),
					static_cast<uint32_t>(patch.newDeclarations.front().size()),
					1, 7, patchResourceRegister) ||
				patchResourceRegister != 47) {
				return nullptr;
			}

			// The payload is inserted only at the audited terminal composition site.
			if (!patch.redirectedOutputs.empty() || patch.preRetInstructions.empty() ||
				factorTempRegister >= patch.additionalTempRegisters ||
				sunSite.scaleTargetCount == 0 ||
				sunSite.scaleTargetCount > sunSite.scaleTargets.size() ||
				patch.tempRemapPositions.size() != patch.preRetInstructions.size() ||
				patch.svPositionFixupPositions.size() != patch.preRetInstructions.size()) {
				return nullptr;
			}
		}

		// Make a mutable copy of the patch for per-shader adjustments.
		DXBCPatch mPatch = patch;

		// If the target shader doesn't have SV_Position declared in SHEX but the
		// patch's pre-ret instructions need it (for texture loads via SV_Position coords),
		// add a dcl_input_ps_siv declaration.
		// CRITICAL: Use the register index from the ISGN chunk (authoritative source for
		// input register assignments). Using maxInputRegister+1 would create a register
		// that doesn't exist in the ISGN, causing a d3d11.dll crash at draw time.
		if (parsed.shex.svPositionRegister == 0xFFFFFFFF) {
			bool needsSvPosition = false;
			for (const auto& fixups : mPatch.svPositionFixupPositions) {
				if (!fixups.empty()) {
					needsSvPosition = true;
					break;
				}
			}
			if (needsSvPosition) {
				if (parsed.isgnSvPositionRegister == 0xFFFFFFFF) {
					// SV_Position not in ISGN either — can't patch this variant safely.
					return nullptr;
				}
				uint32_t regIdx = parsed.isgnSvPositionRegister;
				if (parsed.shex.declaredInputRegisters.count(regIdx)) {
					const auto mask = parsed.shex.inputRegisterMasks.find(regIdx);
					if (mask == parsed.shex.inputRegisterMasks.end() ||
						(mask->second & 0x3u) != 0x3u) {
						// Declared without .xy: reusing it would read undeclared
						// components, and a second declaration would conflict.
						return nullptr;
					}
					// Register v{regIdx} is already declared in SHEX (as dcl_input_ps for
					// texcoord or similar). Don't add a conflicting dcl_input_ps_siv — the
					// register is already readable. Just set the fixup target.
					parsed.shex.svPositionRegister = regIdx;
					logger::info("[DXBCPatcher] SV_Position v{} already declared as dcl_input_ps, reusing (no new decl)", regIdx);
				} else {
					// Register not declared in SHEX — add dcl_input_ps_siv.
					mPatch.newDeclarations.push_back({
						0x04002064,  // opcode 0x64, length=4, interp=4 (linear_noperspective)
						0x001010F2,  // operand: input type(1), 4-comp, mask=xyzw, 1D indexed
						regIdx,      // register index from ISGN
						0x00000001,  // system value: SV_Position
					});
					parsed.shex.svPositionRegister = regIdx;
					logger::info("[DXBCPatcher] Added dcl_input_ps_siv v{}.xyzw, position (from ISGN)", regIdx);
				}
			}
		}

		// Calculate the temp register base for remapping.
		uint32_t tempBase = parsed.shex.tempCount;

		// Calculate the new temp declaration without allowing a wrapped or
		// out-of-profile register count to enter the bytecode.
		constexpr uint32_t kMaxShaderTemps = 4096;
		if (mPatch.minTempRegisters > kMaxShaderTemps ||
			mPatch.additionalTempRegisters > kMaxShaderTemps - parsed.shex.tempCount) {
			return nullptr;
		}
		uint32_t newTempCount = std::max(mPatch.minTempRegisters,
			parsed.shex.tempCount + mPatch.additionalTempRegisters);

		// Locate SHEX chunk boundaries in source
		uint32_t shexFileOffset = parsed.chunkOffsets[parsed.shexChunkIndex];

		// Build new instruction stream
		std::vector<uint32_t> newInstrStream;
		newInstrStream.reserve(parsed.shex.instrStreamDwords + 256);  // generous reserve

		const uint32_t* src = parsed.shex.instrStream;
		uint32_t pos = 0;
		bool newDeclsInserted = false;

		// Phase 1: Copy declarations up to declEndOffset, inserting new declarations
		// at the correct position (before dcl_input_ps/dcl_output/dcl_temps).
		uint32_t declEnd = parsed.shex.declEndOffset;
		if (declEnd == 0xFFFFFFFF)
			declEnd = parsed.shex.instrStreamDwords;

		uint32_t newDeclInsertPoint = parsed.shex.resourceDeclEndOffset;
		if (newDeclInsertPoint == 0xFFFFFFFF)
			newDeclInsertPoint = declEnd;

		while (pos < declEnd) {
			if (!newDeclsInserted && pos >= newDeclInsertPoint) {
				for (const auto& decl : mPatch.newDeclarations)
					for (uint32_t dw : decl)
						newInstrStream.push_back(dw);
				newDeclsInserted = true;
			}

			uint32_t token = src[pos];
			uint32_t opcode = GetOpcodeFromToken(token);
			uint32_t len = GetInstrLength(token);
			if (opcode == DXBCOpcodes::DCL_IMMEDIATE_CB && len == 0 && (pos + 1) < parsed.shex.instrStreamDwords)
				len = src[pos + 1];
			if (len == 0)
				break;

			// Update dcl_temps if needed
			if (opcode == DXBCOpcodes::DCL_TEMPS && newTempCount > parsed.shex.tempCount) {
				newInstrStream.push_back(token);
				newInstrStream.push_back(newTempCount);
				pos += len;
				continue;
			}

			for (uint32_t i = 0; i < len; i++)
				newInstrStream.push_back(src[pos + i]);
			pos += len;
		}

		if (!newDeclsInserted) {
			for (const auto& decl : mPatch.newDeclarations)
				for (uint32_t dw : decl)
					newInstrStream.push_back(dw);
		}

		// If the shader had no dcl_temps and we need one, add it
		if (parsed.shex.tempsDeclOffset == 0xFFFFFFFF && newTempCount > 0) {
			newInstrStream.push_back(0x02000068);  // dcl_temps, len=2
			newInstrStream.push_back(newTempCount);
		}

		// Build redirect map: output register index → absolute temp register index
		std::unordered_map<uint32_t, uint32_t> redirectMap;
		for (const auto& [outIdx, addTempIdx] : mPatch.redirectedOutputs) {
			redirectMap[outIdx] = tempBase + addTempIdx;
		}

		// Track which output registers were ACTUALLY redirected (the original shader
		// wrote to them). Pre-ret instructions that write to non-redirected outputs
		// will be skipped to avoid writing to undeclared output registers (crash).
		std::unordered_set<uint32_t> actuallyRedirectedOutputs;
		bool sunPayloadInserted = false;

		auto appendSunPayload = [&]() -> bool {
			for (size_t instrIdx = 0; instrIdx < mPatch.preRetInstructions.size(); instrIdx++) {
				const auto& instr = mPatch.preRetInstructions[instrIdx];
				if (instr.empty())
					return false;

				// The cloud-load snippet's output is rewritten to an added temp by
				// DFLightPatcher. An output destination here would reintroduce the
				// broad output-MUL path this splice is specifically designed to avoid.
				if (instr.size() >= 2) {
					uint32_t firstOp = 1;
					uint32_t opcodeToken = instr[0];
					while ((opcodeToken >> 31) && firstOp < instr.size()) {
						opcodeToken = instr[firstOp++];
					}
					if (firstOp < instr.size() && ((instr[firstOp] >> 12) & 0xFF) == 2)
						return false;
				}

				for (size_t dwIdx = 0; dwIdx < instr.size(); dwIdx++) {
					uint32_t dw = instr[dwIdx];
					for (uint32_t remapPos : mPatch.tempRemapPositions[instrIdx]) {
						if (remapPos >= instr.size())
							return false;
						if (remapPos == dwIdx) {
							dw += tempBase;
							break;
						}
					}
					for (uint32_t fixupPos : mPatch.svPositionFixupPositions[instrIdx]) {
						if (fixupPos >= instr.size() || parsed.shex.svPositionRegister == 0xFFFFFFFF)
							return false;
						if (fixupPos == dwIdx) {
							dw = parsed.shex.svPositionRegister;
							break;
						}
					}
					newInstrStream.push_back(dw);
				}
			}

			// Scale only the direct-light intermediates selected by the exact
			// signature. Environment addends are never destinations here.
			const uint32_t factorTemp = tempBase + factorTempRegister;
			for (uint32_t i = 0; i < sunSite.scaleTargetCount; i++) {
				const auto& target = sunSite.scaleTargets[i];
				if (target.tempRegister >= parsed.shex.tempCount)
					return false;
				if (target.components == SunShadowSite::Components::kW) {
					newInstrStream.insert(newInstrStream.end(), {
						0x07000038,                 // mul, length 7
						0x00100082, target.tempRegister, // rN.w
						0x00100FF6, target.tempRegister, // rN.wwww
						0x00100006, factorTemp      // rFactor.xxxx
					});
				} else if (target.components == SunShadowSite::Components::kY) {
					newInstrStream.insert(newInstrStream.end(), {
						0x07000038,                 // mul, length 7
						0x00100022, target.tempRegister, // rN.y
						0x00100556, target.tempRegister, // rN.yyyy
						0x00100006, factorTemp      // rFactor.xxxx
					});
				} else {
					newInstrStream.insert(newInstrStream.end(), {
						0x07000038,                 // mul, length 7
						0x00100072, target.tempRegister, // rN.xyz
						0x00100246, target.tempRegister, // rN.xyzx
						0x00100006, factorTemp      // rFactor.xxxx
					});
				}
			}
			return true;
		};

		// Phase 3: Copy body instructions. The sunlight path inserts once at the
		// original-SHEX terminal MAD offset; legacy callers retain pre-ret behavior.
		while (pos < parsed.shex.instrStreamDwords) {
			uint32_t token = src[pos];
			uint32_t opcode = GetOpcodeFromToken(token);
			uint32_t len = GetInstrLength(token);
			if (len == 0)
				break;

			if (strictSunShadow && pos == sunSite.insertionOffset) {
				if (sunPayloadInserted || !appendSunPayload())
					return nullptr;
				sunPayloadInserted = true;
			}

			// Instructions with two destinations can write an output through
			// their second operand, which the redirect below never inspects.
			// Refuse such shaders rather than leave an output unredirected.
			if (!redirectMap.empty() && WritesOutputInSecondDestination(src + pos, len, opcode))
				return nullptr;

			// Redirect output register writes to temp registers.
			if (!redirectMap.empty() && len >= 2 && !IsDeclaration(opcode) &&
				opcode != DXBCOpcodes::RET) {
				uint32_t firstOpPos = pos + 1;
				{
					uint32_t tok = token;
					while ((tok >> 31) && firstOpPos < pos + len) {
						tok = src[firstOpPos];
						firstOpPos++;
					}
				}

				if (firstOpPos < pos + len) {
					uint32_t operandToken = src[firstOpPos];
					uint32_t opType = (operandToken >> 12) & 0xFF;

					if (opType == 2) {  // Output register type
						uint32_t indexDim = (operandToken >> 20) & 0x3;
						if (indexDim == 1) {  // 1D indexing
							uint32_t idxPos = firstOpPos + 1;
							if (operandToken >> 31)
								idxPos++;

							if (idxPos < pos + len) {
								uint32_t outIdx = src[idxPos];
								auto it = redirectMap.find(outIdx);
								if (it != redirectMap.end()) {
									actuallyRedirectedOutputs.insert(outIdx);
									uint32_t newOperandToken = (operandToken & ~(0xFFu << 12));
									uint32_t newIdx = it->second;

									for (uint32_t i = 0; i < len; i++) {
										if (pos + i == firstOpPos)
											newInstrStream.push_back(newOperandToken);
										else if (pos + i == idxPos)
											newInstrStream.push_back(newIdx);
										else
											newInstrStream.push_back(src[pos + i]);
									}
									pos += len;
									continue;
								}
							}
						}
					}
				}
			}

			if (!strictSunShadow && opcode == DXBCOpcodes::RET) {
				// Insert pre-ret instructions with temp register remapping + SV_Position fixup
				for (size_t instrIdx = 0; instrIdx < mPatch.preRetInstructions.size(); instrIdx++) {
					const auto& instr = mPatch.preRetInstructions[instrIdx];

					// Detect if this instruction writes to an output register.
					uint32_t destOperandPos = 0xFFFFFFFF;
					uint32_t destOutIdx = 0xFFFFFFFF;
					uint32_t clampedMask = 0xF;  // Default: all components
					if (instr.size() >= 2) {
						uint32_t firstOp = 1;
						{
							uint32_t tok = instr[0];
							while ((tok >> 31) && firstOp < instr.size()) {
								tok = instr[firstOp];
								firstOp++;
							}
						}
						if (firstOp < instr.size()) {
							uint32_t operandToken = instr[firstOp];
							uint32_t destType = (operandToken >> 12) & 0xFF;
							if (destType == 2) {  // Output register
								destOperandPos = static_cast<uint32_t>(firstOp);
								uint32_t idxPos = firstOp + 1;
								if (operandToken >> 31)
									idxPos++;
								if (idxPos < instr.size()) {
									destOutIdx = instr[idxPos];

									// Skip if original shader never wrote to this output
									if (!actuallyRedirectedOutputs.count(destOutIdx))
										continue;

									// Clamp write mask to target shader's declared output mask.
									// Writing to undeclared output components is invalid DXBC.
									auto maskIt = parsed.shex.outputRegisterMasks.find(destOutIdx);
									if (maskIt != parsed.shex.outputRegisterMasks.end()) {
										uint32_t instrMask = (operandToken >> 4) & 0xF;
										clampedMask = instrMask & maskIt->second;
										if (clampedMask == 0)
											continue;  // No valid components — skip instruction
									}
								}
							}
						}
					}

					for (size_t dwIdx = 0; dwIdx < instr.size(); dwIdx++) {
						uint32_t dw = instr[dwIdx];

						// Apply output mask clamping
						if (static_cast<uint32_t>(dwIdx) == destOperandPos && destOutIdx != 0xFFFFFFFF) {
							dw = (dw & ~0xF0u) | ((clampedMask & 0xF) << 4);
						}

						if (instrIdx < mPatch.tempRemapPositions.size()) {
							for (uint32_t remapPos : mPatch.tempRemapPositions[instrIdx]) {
								if (remapPos == static_cast<uint32_t>(dwIdx)) {
									dw += tempBase;
									break;
								}
							}
						}
						if (instrIdx < mPatch.svPositionFixupPositions.size()) {
							for (uint32_t fixupPos : mPatch.svPositionFixupPositions[instrIdx]) {
								if (fixupPos == static_cast<uint32_t>(dwIdx) &&
									parsed.shex.svPositionRegister != 0xFFFFFFFF) {
									dw = parsed.shex.svPositionRegister;
									break;
								}
							}
						}
						newInstrStream.push_back(dw);
					}
				}
			}

			// Copy original instruction
			for (uint32_t i = 0; i < len; i++)
				newInstrStream.push_back(src[pos + i]);
			pos += len;
		}

		if (strictSunShadow && !sunPayloadInserted)
			return nullptr;

		// --- Build output DXBC blob with EXACT correct size ---
		if (newInstrStream.size() >
			(static_cast<size_t>(std::numeric_limits<uint32_t>::max()) / 4) - 2) {
			return nullptr;
		}
		uint32_t newShexDataSize =
			(2 + static_cast<uint32_t>(newInstrStream.size())) * 4;
		uint32_t newShexChunkSize = newShexDataSize;
		if (newShexChunkSize <= parsed.shex.shexChunkSize)
			return nullptr;
		const uint32_t shexSizeDelta =
			newShexChunkSize - parsed.shex.shexChunkSize;
		const uint64_t finalSize64 = static_cast<uint64_t>(srcSize) + shexSizeDelta;
		if (finalSize64 > std::numeric_limits<uint32_t>::max()) {
			// Nothing was actually added (shouldn't happen with valid patch)
			return nullptr;
		}
		const uint32_t finalSize = static_cast<uint32_t>(finalSize64);

		ID3DBlob* outBlob = nullptr;
		if (FAILED(D3DCreateBlob(finalSize, &outBlob)))
			return nullptr;
		auto* dst = static_cast<uint8_t*>(outBlob->GetBufferPointer());

		uint32_t writePos = 0;

		// Copy header + chunk offsets
		uint32_t preShexSize = shexFileOffset;
		memcpy(dst, srcData, preShexSize);
		writePos = preShexSize;

		// Write SHEX chunk header
		memcpy(dst + writePos, "SHEX", 4);
		if (memcmp(srcData + shexFileOffset, "SHDR", 4) == 0)
			memcpy(dst + writePos, "SHDR", 4);
		writePos += 4;
		memcpy(dst + writePos, &newShexChunkSize, 4);
		writePos += 4;

		// Write version + length tokens
		const uint32_t* origShexData = reinterpret_cast<const uint32_t*>(srcData + shexFileOffset + 8);
		uint32_t versionToken = origShexData[0];
		uint32_t newLengthDwords = 2 + static_cast<uint32_t>(newInstrStream.size());
		memcpy(dst + writePos, &versionToken, 4);
		writePos += 4;
		memcpy(dst + writePos, &newLengthDwords, 4);
		writePos += 4;

		// Write new instruction stream
		memcpy(dst + writePos, newInstrStream.data(), newInstrStream.size() * 4);
		writePos += static_cast<uint32_t>(newInstrStream.size()) * 4;

		// Copy any chunks after SHEX
		uint32_t origShexEnd = shexFileOffset + 8 + parsed.shex.shexChunkSize;
		if (origShexEnd < srcSize) {
			uint32_t remainingSize = srcSize - origShexEnd;
			if (static_cast<uint64_t>(writePos) + remainingSize != finalSize) {
				outBlob->Release();
				return nullptr;
			}
			memcpy(dst + writePos, srcData + origShexEnd, remainingSize);
			writePos += remainingSize;
		}
		if (writePos != finalSize) {
			outBlob->Release();
			return nullptr;
		}

		// Update chunk offsets for chunks after SHEX
		for (uint32_t i = 0; i < parsed.chunkCount; i++) {
			if (parsed.chunkOffsets[i] > shexFileOffset) {
				uint32_t newOffset = parsed.chunkOffsets[i] + shexSizeDelta;
				memcpy(dst + 0x20 + i * 4, &newOffset, 4);
			}
		}

		// Update total file size in DXBC header
		memcpy(dst + 0x18, &finalSize, 4);

		// Fix ISGN ReadWriteMask for SV_Position: if the original shader did not
		// read both screen-space coordinates, the driver may omit their setup.
		// The cloud payload consumes only x/y, so preserve every existing bit and
		// add exactly those two rather than broadening usage to z/w.
		{
			bool patchUsesSvPos = false;
			for (const auto& fixups : mPatch.svPositionFixupPositions) {
				if (!fixups.empty()) {
					patchUsesSvPos = true;
					break;
				}
			}
			if (patchUsesSvPos) {
				// Find ISGN chunk in output blob and patch the SV_Position ReadWriteMask
				for (uint32_t i = 0; i < parsed.chunkCount; i++) {
					uint32_t chunkOff = parsed.chunkOffsets[i];
					// Adjust offset for chunks after SHEX (they shifted by shexSizeDelta)
					if (chunkOff > shexFileOffset)
						chunkOff += shexSizeDelta;
					if (chunkOff + 8 > finalSize)
						continue;
					if (memcmp(dst + chunkOff, "ISGN", 4) != 0)
						continue;

					uint32_t chunkSize;
					memcpy(&chunkSize, dst + chunkOff + 4, 4);
					const uint8_t* isgnBase = dst + chunkOff + 8;
					if (chunkSize < 8)
						break;

					uint32_t elementCount;
					memcpy(&elementCount, isgnBase, 4);
					uint8_t* elemBase = dst + chunkOff + 8 + 8;  // After count + padding

					for (uint32_t e = 0; e < elementCount; e++) {
						uint8_t* elem = elemBase + e * 24;
						if (elem + 24 > dst + chunkOff + 8 + chunkSize)
							break;
						uint32_t systemValue;
						memcpy(&systemValue, elem + 8, 4);
						if (systemValue == 1) {  // SV_Position
							uint8_t oldMask = elem[21];  // ReadWriteMask byte
							elem[21] = static_cast<uint8_t>(oldMask | 0x03); // x/y used
							if (elem[21] != oldMask) {
								logger::debug("[DXBCPatcher] Fixed ISGN SV_Position ReadWriteMask: "
									"0x{:02X} → 0x{:02X}", oldMask, elem[21]);
							}
							break;
						}
					}
					break;  // Only one ISGN chunk
				}
			}
		}

		// Recompute checksum
		ComputeChecksum(dst, finalSize);

		return outBlob;
	}

	// ---- Snippet Compiler ----

	std::vector<std::vector<uint32_t>> DXBCPatcher::CompileSnippetInstructions(
		const char* hlslSource, const char* entryPoint, const char* profile)
	{
		std::vector<std::vector<uint32_t>> result;

		ID3DBlob* shaderBlob = nullptr;
		ID3DBlob* errorBlob = nullptr;
		HRESULT hr = D3DCompile(hlslSource, strlen(hlslSource), nullptr, nullptr, nullptr,
			entryPoint, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shaderBlob, &errorBlob);

		if (errorBlob) errorBlob->Release();
		if (FAILED(hr) || !shaderBlob)
			return result;

		// Parse the compiled blob to find SHEX
		ParsedDXBC parsed;
		if (!Parse(static_cast<const uint8_t*>(shaderBlob->GetBufferPointer()),
				static_cast<uint32_t>(shaderBlob->GetBufferSize()), parsed)) {
			shaderBlob->Release();
			return result;
		}

		// Extract body instructions (between declEnd and first ret)
		const uint32_t* src = parsed.shex.instrStream;
		uint32_t pos = parsed.shex.declEndOffset;
		if (pos == 0xFFFFFFFF)
			pos = 0;

		while (pos < parsed.shex.instrStreamDwords) {
			uint32_t token = src[pos];
			uint32_t opcode = GetOpcodeFromToken(token);
			uint32_t len = GetInstrLength(token);
			if (len == 0 || opcode == DXBCOpcodes::RET)
				break;

			std::vector<uint32_t> instr(src + pos, src + pos + len);
			result.push_back(std::move(instr));
			pos += len;
		}

		shaderBlob->Release();
		return result;
	}

	std::vector<std::vector<uint32_t>> DXBCPatcher::ExtractDeclarations(
		ID3DBlob* blob, const std::vector<uint32_t>& opcodeFilter)
	{
		std::vector<std::vector<uint32_t>> result;
		if (!blob)
			return result;

		ParsedDXBC parsed;
		if (!Parse(static_cast<const uint8_t*>(blob->GetBufferPointer()),
				static_cast<uint32_t>(blob->GetBufferSize()), parsed))
			return result;

		const uint32_t* src = parsed.shex.instrStream;
		uint32_t pos = 0;
		uint32_t declEnd = parsed.shex.declEndOffset;
		if (declEnd == 0xFFFFFFFF)
			declEnd = parsed.shex.instrStreamDwords;

		while (pos < declEnd) {
			uint32_t token = src[pos];
			uint32_t opcode = GetOpcodeFromToken(token);
			uint32_t len = GetInstrLength(token);
			if (opcode == DXBCOpcodes::DCL_IMMEDIATE_CB && len == 0 && (pos + 1) < parsed.shex.instrStreamDwords)
				len = src[pos + 1];
			if (len == 0)
				break;

			for (uint32_t filterOp : opcodeFilter) {
				if (opcode == filterOp) {
					std::vector<uint32_t> decl(src + pos, src + pos + len);
					result.push_back(std::move(decl));
					break;
				}
			}
			pos += len;
		}

		return result;
	}
}
