// Focused contract tests for the cubemap-only BSSky cloud MRT patch.
// Stock Fallout shader bytecode is never embedded or distributed. Passing an
// extracted DXBC file or directory enables the positive OG/NG/VR corpus test.

#include "ShaderTools/DXBCPatcher.h"
#include "ShaderTools/SkyCloudMrtPatcher.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>

bool VerifySkyOutputEquivalence(ID3D11Device* device,
    const std::vector<uint8_t>& stock, const std::vector<uint8_t>& replacement);

using Microsoft::WRL::ComPtr;

namespace
{
	class Tests
	{
	public:
		void Require(bool condition, std::string_view description)
		{
			if (!condition) {
				++failures_;
				std::cerr << "FAIL: " << description << '\n';
			}
		}

		int Failures() const noexcept { return failures_; }

	private:
		int failures_ = 0;
	};

	std::vector<uint8_t> ReadFile(const std::filesystem::path& path)
	{
		std::ifstream input(path, std::ios::binary | std::ios::ate);
		if (!input)
			return {};
		const std::streamoff length = input.tellg();
		if (length <= 0 ||
			static_cast<uint64_t>(length) >
				std::numeric_limits<uint32_t>::max()) {
			return {};
		}
		std::vector<uint8_t> bytes(static_cast<size_t>(length));
		input.seekg(0, std::ios::beg);
		input.read(reinterpret_cast<char*>(bytes.data()),
			static_cast<std::streamsize>(length));
		if (!input || input.gcount() != static_cast<std::streamsize>(length))
			return {};
		return bytes;
	}

	ComPtr<ID3D11Device> CreateWarpDevice()
	{
		ComPtr<ID3D11Device> device;
		D3D_FEATURE_LEVEL createdLevel{};
		constexpr std::array levels{ D3D_FEATURE_LEVEL_11_0 };
		if (FAILED(D3D11CreateDevice(
				nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels.data(),
				static_cast<UINT>(levels.size()), D3D11_SDK_VERSION, &device,
				&createdLevel, nullptr))) {
			return {};
		}
		return device;
	}

	void RunFailClosedTests(Tests& tests)
	{
		tests.Require(
			SIE::SkyCloudMrtPatcher::HashDXBC(nullptr, 0) ==
				0xCBF29CE484222325ULL,
			"empty FNV-1a identity is deterministic");
		tests.Require(
			SIE::SkyCloudMrtPatcher::Identify(nullptr, 0) ==
				SIE::SkyCloudPixelShaderVariant::kUnsupported,
			"null bytecode is unsupported");

		constexpr char source[] = R"(
struct Output
{
    float4 color : SV_Target0;
    float4 motion : SV_Target1;
};
Output main(float4 value : TEXCOORD0)
{
    Output result;
    result.color = value;
    result.motion = value * 2.0;
    return result;
})";
		ComPtr<ID3DBlob> synthetic;
		ComPtr<ID3DBlob> errors;
		tests.Require(SUCCEEDED(D3DCompile(
			source, sizeof(source) - 1, nullptr, nullptr, nullptr, "main",
			"ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &synthetic,
			&errors)), "synthetic two-target pixel shader compiles");
		if (!synthetic)
			return;

		SIE::SkyCloudMrtPatchInfo info{};
		ComPtr<ID3DBlob> rejected;
		rejected.Attach(SIE::SkyCloudMrtPatcher::Patch(synthetic.Get(), &info));
		tests.Require(!rejected && !info.identityMatched &&
			!info.structureMatched && !info.patchVerified,
			"unknown two-target shader fails closed before structural patching");
	}

	void AddCorpusPath(
		const std::filesystem::path& path,
		std::vector<std::filesystem::path>& files)
	{
		std::error_code error;
		if (std::filesystem::is_regular_file(path, error) && !error) {
			files.push_back(path);
			return;
		}
		if (!std::filesystem::is_directory(path, error) || error)
			return;
		for (std::filesystem::recursive_directory_iterator iterator(path, error), end;
			 iterator != end && !error; iterator.increment(error)) {
			if (iterator->is_regular_file(error) && !error &&
				iterator->path().extension() == ".dxbc") {
				files.push_back(iterator->path());
			}
		}
	}

	void RunCorpusTests(
		Tests& tests,
		ID3D11Device* device,
		const std::vector<std::filesystem::path>& files)
	{
		std::array<bool, 7> observed{};
		uint32_t recognized = 0;
        std::unordered_map<uint64_t, std::vector<uint8_t>> recognizedShaders;
		for (const auto& path : files) {
			const auto bytes = ReadFile(path);
			const auto variant = SIE::SkyCloudMrtPatcher::Identify(
				bytes.data(), bytes.size());
			if (variant == SIE::SkyCloudPixelShaderVariant::kUnsupported)
				continue;
			++recognized;
            recognizedShaders[SIE::SkyCloudMrtPatcher::HashDXBC(bytes.data(), bytes.size())] = bytes;
			observed[static_cast<size_t>(variant)] = true;
			auto mutated = bytes;
			mutated.back() ^= 1;
			tests.Require(SIE::SkyCloudMrtPatcher::Identify(
				mutated.data(), mutated.size()) ==
					SIE::SkyCloudPixelShaderVariant::kUnsupported,
				"a one-byte stock cloud mutation fails exact authentication");

			constexpr std::array payloads{
				SIE::SkyCloudMrtPayload::kOpacity,
				SIE::SkyCloudMrtPayload::kUvMapping
			};
			for (const auto payload : payloads) {
				SIE::SkyCloudMrtPatchInfo info{};
				ComPtr<ID3DBlob> patched;
				patched.Attach(SIE::SkyCloudMrtPatcher::Patch(
					bytes.data(), bytes.size(), payload, &info));
				tests.Require(patched && info.identityMatched &&
					info.structureMatched && info.mappingInputsMatched &&
					info.patchVerified && info.variant == variant &&
					info.payload == payload,
					"recognized stock cloud shader payload patches and self-verifies");
				if (!patched)
					continue;

				ComPtr<ID3DBlob> disassembly;
				tests.Require(SUCCEEDED(D3DDisassemble(
					patched->GetBufferPointer(), patched->GetBufferSize(), 0,
					nullptr, &disassembly)),
					"patched stock cloud shader payload disassembles");

				ComPtr<ID3D11PixelShader> shader;
				tests.Require(device && SUCCEEDED(device->CreatePixelShader(
					patched->GetBufferPointer(), patched->GetBufferSize(), nullptr,
					&shader)) && shader,
					"patched stock cloud shader payload creates on the WARP device");

				SIE::DXBCPatcher::ParsedDXBC before{};
				SIE::DXBCPatcher::ParsedDXBC after{};
				tests.Require(SIE::DXBCPatcher::Parse(
					bytes.data(), static_cast<uint32_t>(bytes.size()), before) &&
					SIE::DXBCPatcher::Parse(
						static_cast<const uint8_t*>(patched->GetBufferPointer()),
						static_cast<uint32_t>(patched->GetBufferSize()), after) &&
					after.shex.tempCount == before.shex.tempCount + 2 &&
					after.shex.outputRegisterMasks == before.shex.outputRegisterMasks,
					"payload patch adds only two temps and preserves the two-target signature");
			}
		}

        const std::array<std::array<uint64_t, 2>, 3> compatibilityPairs{{
            { 0x0219856733164CB0ULL, 0x6D2B9D973AF2EC0DULL },
            { 0x7C98FE0C571E1916ULL, 0x9D6DBBA74E764A0CULL },
            { 0x9672C2620203FD20ULL, 0x585F89F70474752CULL }
        }};
        for (const auto& pair : compatibilityPairs) {
            if (!recognizedShaders.contains(pair[1])) continue;
            const bool equivalent = recognizedShaders.contains(pair[0]) &&
                VerifySkyOutputEquivalence(device, recognizedShaders.at(pair[0]),
                    recognizedShaders.at(pair[1]));
            tests.Require(equivalent, "VR replacement preserves stock color, alpha and motion in both eyes");
            if (equivalent) std::cout << "VR cloud compatibility PASS hash=" << std::hex << pair[1] << std::dec << '\n';
        }
		tests.Require(recognized != 0,
			"the supplied corpus contains an authenticated cloud shader");
		if (recognized != 0) {
			for (size_t index = 1; index < observed.size(); ++index) {
				tests.Require(observed[index],
					"the supplied corpus covers every flat and VR cloud variant");
			}
		}
	}
}

int main(int argc, char** argv)
{
	Tests tests;
	RunFailClosedTests(tests);

	std::vector<std::filesystem::path> corpus;
	for (int index = 1; index < argc; ++index)
		AddCorpusPath(argv[index], corpus);
	if (!corpus.empty()) {
		const auto device = CreateWarpDevice();
		tests.Require(device != nullptr, "WARP D3D11 device is available");
		RunCorpusTests(tests, device.Get(), corpus);
	}

	if (tests.Failures() != 0) {
		std::cerr << tests.Failures() << " cloud MRT test(s) failed\n";
		return 1;
	}
	std::cout << "Sky cloud MRT patch tests passed";
	if (!corpus.empty())
		std::cout << " (authenticated corpus files: " << corpus.size() << ')';
	std::cout << '\n';
	return 0;
}
