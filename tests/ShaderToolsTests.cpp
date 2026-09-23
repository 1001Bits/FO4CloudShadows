// Standalone ShaderTools contract tests. No Fallout 4 shader bytecode is
// embedded or distributed; exhaustive corpus mode consumes a user-supplied,
// legally extracted flat or VR DFLight/PS directory.

#include "ShaderTools/DFLightPatcher.h"
#include "ShaderTools/DXBCPatcher.h"
#include "F4SECompat.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <bit>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <span>
#include <system_error>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
	template <class Tag, typename Tag::type Member>
	struct PrivateMemberAccess
	{
		friend typename Tag::type GetPrivateMember(Tag) { return Member; }
	};

	struct BuildPayloadTag
	{
		using type = SIE::DXBCPatch (SIE::DFLightPatcher::*)(uint32_t&);
		friend type GetPrivateMember(BuildPayloadTag);
	};

	template struct PrivateMemberAccess<
		BuildPayloadTag, &SIE::DFLightPatcher::BuildCloudShadowPatch>;

	struct ComputeChecksumTag
	{
		using type = void (*)(uint8_t*, uint32_t);
		friend type GetPrivateMember(ComputeChecksumTag);
	};

	template struct PrivateMemberAccess<
		ComputeChecksumTag, &SIE::DXBCPatcher::ComputeChecksum>;

	class TestState
	{
	public:
		void Require(bool condition, std::string_view description)
		{
			if (condition)
				return;
			failures_++;
			std::cerr << "FAIL: " << description << '\n';
		}

		int Failures() const noexcept { return failures_; }

	private:
		int failures_ = 0;
	};

	void* FO4CS_F4SEAPI TestQueryInterface(std::uint32_t)
	{
		return nullptr;
	}

	FO4CS::F4SECompat::PluginHandle FO4CS_F4SEAPI TestPluginHandle()
	{
		return 1u;
	}

	std::uint32_t FO4CS_F4SEAPI TestReleaseIndex()
	{
		return 0u;
	}

	FO4CS::F4SECompat::RawInterfacePrefix MakeInterfacePrefix(
		FO4CS::F4SECompat::Version f4se,
		FO4CS::F4SECompat::Version runtime,
		bool editor = false)
	{
		return {
			.f4seVersion = f4se.Pack(),
			.runtimeVersion = runtime.Pack(),
			.editorVersion = 0u,
			.isEditor = editor ? 1u : 0u,
			.queryInterface = &TestQueryInterface,
			.getPluginHandle = &TestPluginHandle,
			.getReleaseIndex = &TestReleaseIndex,
		};
	}

	void RunF4SECompatibilityTests(TestState& tests)
	{
		using namespace FO4CS::F4SECompat;

		SYSTEM_INFO systemInfo{};
		GetSystemInfo(&systemInfo);
		const std::size_t pageSize = systemInfo.dwPageSize;
		auto* pages = static_cast<std::byte*>(VirtualAlloc(
			nullptr, pageSize * 2u, MEM_RESERVE | MEM_COMMIT,
			PAGE_READWRITE));
		tests.Require(pages != nullptr,
			"guard pages allocate for the exact F4SEVR interface-prefix test");
		if (pages) {
			DWORD previousProtection = 0;
			const bool guarded = VirtualProtect(
				pages + pageSize, pageSize, PAGE_NOACCESS,
				&previousProtection) != FALSE;
			tests.Require(guarded,
				"the byte immediately after the F4SEVR prefix is inaccessible");
			if (guarded) {
				auto* exactPrefix =
					reinterpret_cast<RawInterfacePrefix*>(
						pages + pageSize - sizeof(RawInterfacePrefix));
				*exactPrefix = MakeInterfacePrefix(
					kF4SEVR0621, kF4SEVRProxyRuntime);
				Interface vr;
				tests.Require(vr.Normalize(exactPrefix),
					"Normalize accepts the exact 40-byte F4SEVR 0.6.21 prefix "
					"without reading extension fields");
				tests.Require(
					ClassifyHost(vr, L"Fallout4VR.exe", kVRRuntime) ==
						RuntimeTarget::kVR,
					"exact Fallout4VR 1.2.72 plus the 1.10.138 proxy is accepted");
				tests.Require(
					ClassifyHost(vr, L"Fallout4.exe", kVRRuntime) ==
						RuntimeTarget::kUnsupported,
					"the VR extender cannot authorize the flat executable");
				tests.Require(
					ClassifyHost(vr, L"Fallout4VR.exe", kLegacyRuntime) ==
						RuntimeTarget::kUnsupported,
					"the VR proxy cannot authorize a non-1.2.72 main image");
			}
			VirtualFree(pages, 0, MEM_RELEASE);
		}

		RawExtendedInterface flat{};
		flat.prefix = MakeInterfacePrefix(
			kF4SEWithPluginInfo, kLegacyRuntime);
		Interface og;
		tests.Require(og.Normalize(&flat) &&
			ClassifyHost(og, L"Fallout4.exe", kLegacyRuntime) ==
				RuntimeTarget::kLegacy,
			"exact Fallout4 1.10.163 host classification succeeds");

		flat.prefix = MakeInterfacePrefix(
			kF4SEWithPluginInfo, kAERuntime);
		Interface ae;
		tests.Require(ae.Normalize(&flat) &&
			ClassifyHost(ae, L"FALLOUT4.EXE", kAERuntime) ==
				RuntimeTarget::kAE,
			"exact Fallout4 1.11.240 host classification succeeds case-insensitively");
		tests.Require(
			ClassifyHost(ae, L"Fallout4.exe", kLegacyRuntime) ==
				RuntimeTarget::kUnsupported,
			"flat reported and actual runtime versions must match exactly");

		flat.prefix.isEditor = 1u;
		Interface editor;
		tests.Require(editor.Normalize(&flat) &&
			ClassifyHost(editor, L"Fallout4.exe", kAERuntime) ==
				RuntimeTarget::kUnsupported,
			"editor hosts are rejected");
	}

	uint32_t ReadU32(const std::vector<uint8_t>& bytes, size_t offset)
	{
		uint32_t value = 0;
		if (offset <= bytes.size() && bytes.size() - offset >= sizeof(value))
			std::memcpy(&value, bytes.data() + offset, sizeof(value));
		return value;
	}

	bool WriteU32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value)
	{
		if (offset > bytes.size() || bytes.size() - offset < sizeof(value))
			return false;
		std::memcpy(bytes.data() + offset, &value, sizeof(value));
		return true;
	}

	ComPtr<ID3DBlob> MakeBlob(const std::vector<uint8_t>& bytes)
	{
		ComPtr<ID3DBlob> blob;
		if (bytes.empty() || FAILED(D3DCreateBlob(bytes.size(), &blob)))
			return {};
		std::memcpy(blob->GetBufferPointer(), bytes.data(), bytes.size());
		return blob;
	}

	std::vector<uint8_t> ReadFile(const std::filesystem::path& path)
	{
		std::ifstream stream(path, std::ios::binary | std::ios::ate);
		if (!stream)
			return {};
		const std::streamoff length = stream.tellg();
		if (length <= 0 || static_cast<uint64_t>(length) > 4u * 1024u * 1024u)
			return {};
		stream.seekg(0, std::ios::beg);
		std::vector<uint8_t> bytes(static_cast<size_t>(length));
		stream.read(
			reinterpret_cast<char*>(bytes.data()),
			static_cast<std::streamsize>(length));
		return stream && stream.gcount() == static_cast<std::streamsize>(length) ?
			bytes : std::vector<uint8_t>{};
	}

	bool CompileFixtureContainer(std::vector<uint8_t>& bytes)
	{
		static constexpr char kSource[] = R"(
struct Outputs
{
    float4 diffuse : SV_Target0;
    float4 specular : SV_Target1;
};

Outputs main(float4 position : SV_Position)
{
    Outputs result;
    result.diffuse = float4(position.xy, 0.0, 0.0);
    result.specular = float4(position.yx, 0.0, 1.0);
    return result;
}
)";

		ComPtr<ID3DBlob> shader;
		ComPtr<ID3DBlob> errors;
		const HRESULT hr = D3DCompile(
			kSource, sizeof(kSource) - 1, "ShaderToolsSyntheticFixture.hlsl",
			nullptr, nullptr, "main", "ps_5_0",
			D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &errors);
		if (FAILED(hr) || !shader) {
			if (errors && errors->GetBufferPointer())
				std::cerr << static_cast<const char*>(errors->GetBufferPointer()) << '\n';
			return false;
		}
		const auto* begin = static_cast<const uint8_t*>(shader->GetBufferPointer());
		bytes.assign(begin, begin + shader->GetBufferSize());
		return true;
	}

	bool ReplaceWithDirectSunFixture(std::vector<uint8_t>& bytes, bool recompiled = false,
		std::span<const uint32_t> customBody = {}, uint32_t temps = 8)
	{
		if (bytes.size() > std::numeric_limits<uint32_t>::max())
			return false;
		SIE::DXBCPatcher::ParsedDXBC parsed{};
		if (!SIE::DXBCPatcher::Parse(
				bytes.data(), static_cast<uint32_t>(bytes.size()), parsed) ||
			parsed.shexChunkIndex < 0 ||
			parsed.shex.declEndOffset == 0xFFFFFFFF) {
			return false;
		}

		std::vector<uint32_t> stream;
		uint32_t pos = 0;
		bool foundTemps = false;
		while (pos < parsed.shex.declEndOffset) {
			const uint32_t token = parsed.shex.instrStream[pos];
			const uint32_t opcode = token & 0x7FF;
			uint32_t length = (token >> 24) & 0x7F;
			if (opcode == SIE::DXBCOpcodes::DCL_IMMEDIATE_CB && length == 0 &&
				pos + 1 < parsed.shex.instrStreamDwords) {
				length = parsed.shex.instrStream[pos + 1];
			}
			if (length == 0 || pos + length > parsed.shex.declEndOffset)
				return false;

			stream.insert(stream.end(), parsed.shex.instrStream + pos,
				parsed.shex.instrStream + pos + length);
			if (opcode == SIE::DXBCOpcodes::DCL_TEMPS) {
				if (length != 2)
					return false;
				stream.back() = temps;
				foundTemps = true;
			}
			pos += length;
		}
		if (!foundTemps) {
			stream.push_back(0x02000068); // dcl_temps, length 2
			stream.push_back(temps);
		}

		// Exact audited shadowed direct-only terminal layout (signature variant 3).
		static constexpr std::array<uint32_t, 42> kDirectSunBody{
			0x07000038, 0x00100072, 0x00000001, 0x00100FF6,
			0x00000000, 0x00100246, 0x00000007,
			0x07000038, 0x00102072, 0x00000001, 0x00100FF6,
			0x00000001, 0x00100246, 0x00000001,
			0x07000038, 0x00100072, 0x00000000, 0x00100FF6,
			0x00000001, 0x00100246, 0x00000000,
			0x05000036, 0x00100082, 0x00000000, 0x00004001, 0x00000000,
			0x0A00000E, 0x001020F2, 0x00000000, 0x00100E46, 0x00000000,
			0x00004002, 0x40400000, 0x40400000, 0x40400000, 0x40400000,
			0x05000036, 0x00102082, 0x00000001, 0x00004001, 0x3F800000,
			0x0100003E
		};
		if (!customBody.empty()) {
			stream.insert(stream.end(), customBody.begin(), customBody.end());
		} else if (recompiled) {
			stream.insert(stream.end(), kDirectSunBody.begin(), kDirectSunBody.begin() + 21);
			// The compiler-normalized output of the same direct-light program.
			static constexpr std::array<uint32_t, 21> kRecompiledOutput{
				0x0A000038, 0x00102072, 0, 0x00100246, 0,
				0x00004002, 0x3EAAAAAB, 0x3EAAAAAB, 0x3EAAAAAB, 0,
				0x05000036, 0x00102082, 0, 0x00004001, 0,
				0x05000036, 0x00102082, 1, 0x00004001, 0x3F800000,
				0x0100003E
			};
			stream.insert(stream.end(), kRecompiledOutput.begin(), kRecompiledOutput.end());
		} else {
			stream.insert(stream.end(), kDirectSunBody.begin(), kDirectSunBody.end());
		}

		const uint32_t shexOffset = parsed.chunkOffsets[
			static_cast<size_t>(parsed.shexChunkIndex)];
		const uint32_t oldChunkBytes = 8 + parsed.shex.shexChunkSize;
		if (static_cast<uint64_t>(shexOffset) + oldChunkBytes > bytes.size() ||
			stream.size() > (std::numeric_limits<uint32_t>::max() / 4u) - 2u) {
			return false;
		}
		const uint32_t newChunkSize =
			(2u + static_cast<uint32_t>(stream.size())) * sizeof(uint32_t);
		const uint32_t newChunkBytes = 8 + newChunkSize;
		const int64_t delta = static_cast<int64_t>(newChunkBytes) - oldChunkBytes;
		const int64_t newSizeSigned = static_cast<int64_t>(bytes.size()) + delta;
		if (newSizeSigned <= 0 ||
			newSizeSigned > std::numeric_limits<uint32_t>::max()) {
			return false;
		}

		std::vector<uint8_t> rebuilt(static_cast<size_t>(newSizeSigned));
		std::memcpy(rebuilt.data(), bytes.data(), shexOffset);
		std::memcpy(rebuilt.data() + shexOffset, bytes.data() + shexOffset, 4);
		if (!WriteU32(rebuilt, static_cast<size_t>(shexOffset) + 4, newChunkSize) ||
			!WriteU32(rebuilt, static_cast<size_t>(shexOffset) + 8,
				parsed.shex.versionToken) ||
			!WriteU32(rebuilt, static_cast<size_t>(shexOffset) + 12,
				2u + static_cast<uint32_t>(stream.size()))) {
			return false;
		}
		std::memcpy(rebuilt.data() + shexOffset + 16, stream.data(),
			stream.size() * sizeof(uint32_t));

		const size_t oldSuffix = static_cast<size_t>(shexOffset) + oldChunkBytes;
		const size_t newSuffix = static_cast<size_t>(shexOffset) + newChunkBytes;
		std::memcpy(rebuilt.data() + newSuffix, bytes.data() + oldSuffix,
			bytes.size() - oldSuffix);

		for (uint32_t i = 0; i < parsed.chunkCount; i++) {
			if (parsed.chunkOffsets[i] <= shexOffset)
				continue;
			const int64_t adjusted = static_cast<int64_t>(parsed.chunkOffsets[i]) + delta;
			if (adjusted < 0 || adjusted > std::numeric_limits<uint32_t>::max() ||
				!WriteU32(rebuilt, 0x20u + static_cast<size_t>(i) * 4u,
					static_cast<uint32_t>(adjusted))) {
				return false;
			}
		}
		if (!WriteU32(rebuilt, 0x18, static_cast<uint32_t>(rebuilt.size())))
			return false;
		GetPrivateMember(ComputeChecksumTag{})(
			rebuilt.data(), static_cast<uint32_t>(rebuilt.size()));

		SIE::DXBCPatcher::ParsedDXBC verification{};
		if (!SIE::DXBCPatcher::Parse(
			rebuilt.data(), static_cast<uint32_t>(rebuilt.size()), verification)) {
			return false;
		}
		bytes = std::move(rebuilt);
		return true;
	}

	bool BuildSyntheticDirectSunFixture(std::vector<uint8_t>& bytes)
	{
		return CompileFixtureContainer(bytes) && ReplaceWithDirectSunFixture(bytes);
	}

	SIE::DXBCPatch BuildCloudPayload(uint32_t& factorRegister)
	{
		SIE::DFLightPatcher builder;
		const auto build = GetPrivateMember(BuildPayloadTag{});
		return (builder.*build)(factorRegister);
	}

	bool Rejects(
		const std::vector<uint8_t>& bytes,
		const SIE::DXBCPatch& patch,
		uint32_t factorRegister,
		SIE::DXBCPatcher::SunShadowPatchInfo* info = nullptr)
	{
		ComPtr<ID3DBlob> blob = MakeBlob(bytes);
		if (!blob)
			return false;
		ID3DBlob* output = SIE::DXBCPatcher::PatchSunShadowShader(
			blob.Get(), patch, factorRegister, info);
		if (!output)
			return true;
		output->Release();
		return false;
	}

	bool Rejects(
		ID3DBlob* blob,
		const SIE::DXBCPatch& patch,
		uint32_t factorRegister)
	{
		ID3DBlob* output = SIE::DXBCPatcher::PatchSunShadowShader(
			blob, patch, factorRegister, nullptr);
		if (!output)
			return true;
		output->Release();
		return false;
	}

	bool RecomputeChecksum(std::vector<uint8_t>& bytes)
	{
		if (bytes.size() > std::numeric_limits<uint32_t>::max())
			return false;
		GetPrivateMember(ComputeChecksumTag{})(
			bytes.data(), static_cast<uint32_t>(bytes.size()));
		return true;
	}

	void RunRecompiledCompositionTests(TestState& tests, ID3D11Device* device,
		ID3D11DeviceContext* context, const SIE::DXBCPatch& patch, uint32_t factorRegister)
	{
		// Render known direct + ambient terms through the real patched PS. A
		// factor of zero must leave ambient light and both alpha channels intact.
		const char* vertexSource = R"(
float4 main(uint id : SV_VertexID) : SV_Position {
    return float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0, 1);
})";
		ComPtr<ID3DBlob> vertexBytes;
		ComPtr<ID3D11VertexShader> vertex;
		if (FAILED(D3DCompile(vertexSource, std::strlen(vertexSource), nullptr, nullptr,
			nullptr, "main", "vs_5_0", 0, 0, &vertexBytes, nullptr)) ||
			FAILED(device->CreateVertexShader(vertexBytes->GetBufferPointer(),
				vertexBytes->GetBufferSize(), nullptr, &vertex))) {
			tests.Require(false, "composition fixture vertex shader creates"); return;
		}
		D3D11_TEXTURE2D_DESC description{};
		description.Width = description.Height = description.ArraySize = description.MipLevels = 1;
		description.SampleDesc.Count = 1;
		description.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
		description.BindFlags = D3D11_BIND_RENDER_TARGET;
		std::array<ComPtr<ID3D11Texture2D>, 2> targets;
		std::array<ComPtr<ID3D11RenderTargetView>, 2> views;
		for (size_t i = 0; i < targets.size(); ++i) {
			if (FAILED(device->CreateTexture2D(&description, nullptr, &targets[i])) ||
				FAILED(device->CreateRenderTargetView(targets[i].Get(), nullptr, &views[i]))) {
				tests.Require(false, "composition fixture targets create"); return;
			}
		}
		description.BindFlags = 0;
		description.Usage = D3D11_USAGE_STAGING;
		description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ComPtr<ID3D11Texture2D> readback;
		if (FAILED(device->CreateTexture2D(&description, nullptr, &readback))) {
			tests.Require(false, "composition readback creates"); return;
		}
		description.Format = DXGI_FORMAT_R32_FLOAT;
		description.Usage = D3D11_USAGE_DEFAULT;
		description.CPUAccessFlags = 0;
		description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		ComPtr<ID3D11Texture2D> mask;
		ComPtr<ID3D11ShaderResourceView> maskView;
		if (FAILED(device->CreateTexture2D(&description, nullptr, &mask)) ||
			FAILED(device->CreateShaderResourceView(mask.Get(), nullptr, &maskView))) {
			tests.Require(false, "composition factor texture creates"); return;
		}
		D3D11_RASTERIZER_DESC rasterDescription{};
		rasterDescription.FillMode = D3D11_FILL_SOLID;
		rasterDescription.CullMode = D3D11_CULL_NONE;
		rasterDescription.DepthClipEnable = TRUE;
		ComPtr<ID3D11RasterizerState> raster;
		if (FAILED(device->CreateRasterizerState(&rasterDescription, &raster))) {
			tests.Require(false, "composition rasterizer creates"); return;
		}
		context->ClearState();
		context->VSSetShader(vertex.Get(), nullptr, 0);
		context->RSSetState(raster.Get());
		const D3D11_VIEWPORT viewport{ 0, 0, 1, 1, 0, 1 };
		context->RSSetViewports(1, &viewport);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		ID3D11RenderTargetView* rawViews[]{ views[0].Get(), views[1].Get() };
		context->OMSetRenderTargets(2, rawViews, nullptr);
		ID3D11ShaderResourceView* rawMask = maskView.Get();
		context->PSSetShaderResources(47, 1, &rawMask);
		for (const bool shadowed : { false, true }) {
			std::vector<uint32_t> body;
			auto initialize = [&](uint32_t reg, std::array<float, 4> values) {
				body.insert(body.end(), { 0x08000036, 0x001000F2, reg, 0x00004002 });
				for (float value : values) body.push_back(std::bit_cast<uint32_t>(value));
			};
			initialize(0, { .9f, 1.2f, 1.5f, .75f });
			initialize(shadowed ? 1u : 8u, { .6f, .9f, 1.2f, .75f });
			initialize(6, { .3f, .6f, .9f, 0 });
			initialize(shadowed ? 10u : 9u, { .1f, .2f, .3f, 0 });
			if (shadowed) {
				body.insert(body.end(), {
					0x09000032, 0x00102072, 1, 0x00100246, 1, 0x00100FF6, 1, 0x00100246, 10,
					0x09000032, 0x00100072, 0, 0x00100246, 0, 0x00100FF6, 1, 0x00100246, 6 });
			} else {
				body.insert(body.end(), {
					0x09000032, 0x00102072, 1, 0x00100246, 8, 0x00100FF6, 0, 0x00100246, 9,
					0x07000000, 0x00100072, 0, 0x00100246, 0, 0x00100246, 6 });
			}
			body.insert(body.end(), {
				0x0A000038, 0x00102072, 0, 0x00100246, 0,
				0x00004002, 0x3EAAAAAB, 0x3EAAAAAB, 0x3EAAAAAB, 0,
				0x05000036, 0x00102082, 0, 0x00004001, 0,
				0x05000036, 0x00102082, 1, 0x00004001, 0x3F800000, 0x0100003E });
			std::vector<uint8_t> bytes;
			if (!CompileFixtureContainer(bytes) || !ReplaceWithDirectSunFixture(bytes, false, body, 11)) {
				tests.Require(false, "initialized composition fixture builds"); continue;
			}
			auto input = MakeBlob(bytes);
			ComPtr<ID3DBlob> patched;
			SIE::DXBCPatcher::SunShadowPatchInfo info{};
			patched.Attach(SIE::DXBCPatcher::PatchSunShadowShader(input.Get(), patch, factorRegister, &info));
			ComPtr<ID3D11PixelShader> pixel;
			if (!patched || FAILED(device->CreatePixelShader(patched->GetBufferPointer(),
				patched->GetBufferSize(), nullptr, &pixel))) {
				tests.Require(false, "patched direct-plus-ambient fixture creates"); continue;
			}
			tests.Require(info.signatureVariant == (shadowed ? 6u : 4u) &&
				info.scaledTargetCount == (shadowed ? 1u : 2u),
				"composition fixture scales the exact direct-light operands");
			context->PSSetShader(pixel.Get(), nullptr, 0);
			for (float factor : { 0.0f, 0.5f, 1.0f }) {
				context->UpdateSubresource(mask.Get(), 0, nullptr, &factor, sizeof(float), 0);
				context->Draw(3, 0);
				for (size_t target = 0; target < targets.size(); ++target) {
					context->CopyResource(readback.Get(), targets[target].Get());
					D3D11_MAPPED_SUBRESOURCE mapped{};
					if (FAILED(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
						tests.Require(false, "composition pixels read back"); continue;
					}
					std::array<float, 4> values{};
					std::memcpy(values.data(), mapped.pData, sizeof(values));
					context->Unmap(readback.Get(), 0);
					for (size_t channel = 0; channel < 3; ++channel) {
						const float lane = static_cast<float>(channel);
						const float expected = target == 0
							? ((.9f + .3f * lane) * (shadowed ? .75f : 1.0f) * factor + .3f + .3f * lane) / 3.0f
							: (.6f + .3f * lane) * .75f * factor + .1f + .1f * lane;
						tests.Require(std::isfinite(values[channel]) && std::abs(values[channel] - expected) < .00001f,
							"GPU cloud factor scales direct light and preserves ambient diffuse/specular");
					}
					tests.Require(values[3] == (target == 0 ? 0.0f : 1.0f),
						"GPU cloud factor preserves both output alpha channels");
				}
			}
		}
		context->ClearState();
	}

	void RunContainerTests(TestState& tests, const std::vector<uint8_t>& fixture)
	{
		SIE::DXBCPatcher::ParsedDXBC parsed{};
		tests.Require(SIE::DXBCPatcher::Parse(
			fixture.data(), static_cast<uint32_t>(fixture.size()), parsed),
			"synthetic fixture parses before container mutations");
		if (parsed.shexChunkIndex < 0 || parsed.chunkCount < 2)
			return;

		auto corrupted = fixture;
		corrupted.pop_back();
		SIE::DXBCPatcher::ParsedDXBC output{};
		tests.Require(!SIE::DXBCPatcher::Parse(
			corrupted.data(), static_cast<uint32_t>(corrupted.size()), output),
			"truncated container is rejected");

		corrupted = fixture;
		WriteU32(corrupted, 0x18, static_cast<uint32_t>(corrupted.size() - 1));
		tests.Require(!SIE::DXBCPatcher::Parse(
			corrupted.data(), static_cast<uint32_t>(corrupted.size()), output),
			"mismatched DXBC total size is rejected");

		corrupted = fixture;
		WriteU32(corrupted, 0x20, parsed.chunkOffsets[0] + 1);
		tests.Require(!SIE::DXBCPatcher::Parse(
			corrupted.data(), static_cast<uint32_t>(corrupted.size()), output),
			"unaligned chunk offset is rejected");

		corrupted = fixture;
		WriteU32(corrupted, 0x24, parsed.chunkOffsets[0]);
		tests.Require(!SIE::DXBCPatcher::Parse(
			corrupted.data(), static_cast<uint32_t>(corrupted.size()), output),
			"overlapping chunk ranges are rejected");

		const uint32_t shexOffset = parsed.chunkOffsets[
			static_cast<size_t>(parsed.shexChunkIndex)];
		corrupted = fixture;
		WriteU32(corrupted, static_cast<size_t>(shexOffset) + 4,
			std::numeric_limits<uint32_t>::max());
		tests.Require(!SIE::DXBCPatcher::Parse(
			corrupted.data(), static_cast<uint32_t>(corrupted.size()), output),
			"oversized SHEX extent is rejected");

		corrupted = fixture;
		WriteU32(corrupted, static_cast<size_t>(shexOffset) + 12,
			ReadU32(corrupted, static_cast<size_t>(shexOffset) + 12) + 1);
		tests.Require(!SIE::DXBCPatcher::Parse(
			corrupted.data(), static_cast<uint32_t>(corrupted.size()), output),
			"SHEX internal length mismatch is rejected");

		corrupted = fixture;
		const uint32_t firstBody = parsed.shex.declEndOffset;
		const uint32_t firstLength =
			(parsed.shex.instrStream[firstBody] >> 24) & 0x7F;
		const uint32_t lateDeclaration = firstBody + firstLength;
		const size_t lateTokenOffset = static_cast<size_t>(shexOffset) + 16u +
			static_cast<size_t>(lateDeclaration) * sizeof(uint32_t);
		uint32_t lateToken = ReadU32(corrupted, lateTokenOffset);
		lateToken = (lateToken & ~0x7FFu) | SIE::DXBCOpcodes::DCL_TEMPS;
		WriteU32(corrupted, lateTokenOffset, lateToken);
		tests.Require(!SIE::DXBCPatcher::Parse(
			corrupted.data(), static_cast<uint32_t>(corrupted.size()), output),
			"declaration after executable instructions is rejected");
	}

	void RunStrictPatchTests(
		TestState& tests,
		ID3D11Device* device,
		const std::vector<uint8_t>& fixture,
		const SIE::DXBCPatch& patch,
		uint32_t factorRegister)
	{
		ComPtr<ID3DBlob> vanilla = MakeBlob(fixture);
		tests.Require(vanilla != nullptr, "synthetic fixture blob is created");
		if (!vanilla)
			return;

		SIE::DXBCPatcher::SunShadowPatchInfo info{};
		ComPtr<ID3DBlob> patched;
		patched.Attach(SIE::DXBCPatcher::PatchSunShadowShader(
			vanilla.Get(), patch, factorRegister, &info));
		tests.Require(patched != nullptr, "synthetic direct-sun fixture is patched");
		tests.Require(info.signatureMatched && info.signatureVariant == 3 &&
			info.scaledTargetCount == 1,
			"synthetic fixture resolves to exact direct-only signature 3");
		if (!patched)
			return;

		ComPtr<ID3DBlob> disassembly;
		tests.Require(SUCCEEDED(D3DDisassemble(
			patched->GetBufferPointer(), patched->GetBufferSize(),
			0, nullptr, &disassembly)),
			"patched synthetic fixture disassembles");
		ComPtr<ID3D11PixelShader> shader;
		tests.Require(SUCCEEDED(device->CreatePixelShader(
			patched->GetBufferPointer(), patched->GetBufferSize(), nullptr, &shader)) &&
			shader != nullptr,
			"patched synthetic fixture creates on the WARP D3D11 device");

		SIE::DXBCPatcher::ParsedDXBC patchedParsed{};
		tests.Require(SIE::DXBCPatcher::Parse(
			static_cast<const uint8_t*>(patched->GetBufferPointer()),
			static_cast<uint32_t>(patched->GetBufferSize()), patchedParsed) &&
			patchedParsed.shex.declaredResourceRegisters.contains(47),
			"patched fixture declares exactly the cloud t47 resource");
		tests.Require(Rejects(patched.Get(), patch, factorRegister),
			"second splice is rejected because t47 is already declared");

		auto wrongPayload = patch;
		bool rewrotePayloadRegister = false;
		if (wrongPayload.newDeclarations.size() == 1 &&
			wrongPayload.newDeclarations.front().size() >= 3) {
			auto& declaration = wrongPayload.newDeclarations.front();
			uint32_t operandPosition = 1;
			uint32_t operandToken = declaration[operandPosition++];
			while ((operandToken >> 31) != 0 &&
				operandPosition < declaration.size()) {
				operandToken = declaration[operandPosition++];
			}
			if (operandPosition < declaration.size()) {
				declaration[operandPosition] = 46;
				rewrotePayloadRegister = true;
			}
		}
		tests.Require(rewrotePayloadRegister,
			"test locates the payload resource register");
		tests.Require(Rejects(fixture, wrongPayload, factorRegister),
			"strict splice rejects a payload that is not exactly t47");

		SIE::DXBCPatcher::ParsedDXBC parsed{};
		tests.Require(SIE::DXBCPatcher::Parse(
			fixture.data(), static_cast<uint32_t>(fixture.size()), parsed),
			"fixture parses before semantic mutations");
		if (parsed.shexChunkIndex < 0)
			return;
		const uint32_t shexOffset = parsed.chunkOffsets[
			static_cast<size_t>(parsed.shexChunkIndex)];

		auto mutated = fixture;
		const size_t terminalToken = static_cast<size_t>(shexOffset) + 16u +
			static_cast<size_t>(info.insertionOffset) * sizeof(uint32_t);
		uint32_t token = ReadU32(mutated, terminalToken);
		token = (token & ~0x7FFu) | SIE::DXBCOpcodes::ADD;
		WriteU32(mutated, terminalToken, token);
		RecomputeChecksum(mutated);
		tests.Require(Rejects(mutated, patch, factorRegister),
			"one-token terminal signature mutation fails closed");

		mutated = fixture;
		const size_t firstInstruction = static_cast<size_t>(shexOffset) + 16u;
		token = ReadU32(mutated, firstInstruction);
		token = (token & ~0x7FFu) | SIE::DXBCOpcodes::DCL_FUNCTION_BODY;
		WriteU32(mutated, firstInstruction, token);
		RecomputeChecksum(mutated);
		SIE::DXBCPatcher::ParsedDXBC linked{};
		tests.Require(SIE::DXBCPatcher::Parse(
			mutated.data(), static_cast<uint32_t>(mutated.size()), linked) &&
			linked.shex.usesDynamicLinkage,
			"dynamic-linkage declaration is detected");
		tests.Require(Rejects(mutated, patch, factorRegister),
			"dynamic-linkage shader fails closed");
	}

	class TemporaryWorkingDirectory
	{
	public:
		TemporaryWorkingDirectory()
		{
			std::error_code error;
			original_ = std::filesystem::current_path(error);
			if (error)
				return;
			path_ = std::filesystem::temp_directory_path(error) /
				("FO4CloudShadows-ShaderToolsTests-" +
					std::to_string(GetCurrentProcessId()) + '-' +
					std::to_string(std::chrono::steady_clock::now()
						.time_since_epoch().count()));
			if (error || !std::filesystem::create_directory(path_, error) || error)
				return;
			std::filesystem::current_path(path_, error);
			active_ = !error;
		}

		~TemporaryWorkingDirectory()
		{
			std::error_code ignored;
			if (!original_.empty())
				std::filesystem::current_path(original_, ignored);
			if (!path_.empty())
				std::filesystem::remove_all(path_, ignored);
		}

		bool Active() const noexcept { return active_; }

	private:
		std::filesystem::path original_;
		std::filesystem::path path_;
		bool active_ = false;
	};

	void RunLifecycleTests(
		TestState& tests,
		ID3D11Device* device,
		const std::vector<uint8_t>& fixture)
	{
		TemporaryWorkingDirectory isolatedDirectory;
		tests.Require(isolatedDirectory.Active(),
			"isolated working directory is available for lifecycle test");
		if (!isolatedDirectory.Active())
			return;

		SIE::DFLightPatcher patcher;
		const uint64_t hash = SIE::DFLightPatcher::HashDXBC(
			fixture.data(), fixture.size());
		patcher.StoreBytecode(hash, fixture.data(), fixture.size());
		patcher.Release();
		patcher.Initialize(device);
		ComPtr<ID3D11PixelShader> firstGeneration;
		firstGeneration.Attach(patcher.GetPatchedPSByHash(hash));
		tests.Require(patcher.IsInitialized() && patcher.IsReady() &&
			firstGeneration != nullptr,
			"Release preserves captured supported bytecode for first device rebuild");

		ComPtr<ID3D11PixelShader> vanillaShader;
		tests.Require(SUCCEEDED(device->CreatePixelShader(
			fixture.data(), fixture.size(), nullptr, &vanillaShader)) &&
			vanillaShader != nullptr,
			"synthetic vanilla shader creates for identity tests");
		if (vanillaShader) {
			ComPtr<ID3D11PixelShader> replacement;
			replacement.Attach(
				patcher.RegisterDescriptor(2, vanillaShader.Get()));
			tests.Require(replacement == nullptr,
				"directional descriptor alone cannot select a replacement");
			patcher.RecordPSHash(vanillaShader.Get(), hash);
			replacement.Reset();
			replacement.Attach(
				patcher.RegisterDescriptor(8, vanillaShader.Get()));
			tests.Require(replacement == nullptr,
				"punctual-light descriptor rejects an exact shader hash");
			replacement.Reset();
			replacement.Attach(
				patcher.RegisterDescriptor(2, vanillaShader.Get()));
			tests.Require(replacement != nullptr,
				"directional descriptor plus live exact hash selects replacement");
		}

		firstGeneration.Reset();
		patcher.Release();
		tests.Require(!patcher.IsInitialized() && !patcher.IsReady(),
			"device Release clears device state");
		patcher.Initialize(device);
		ComPtr<ID3D11PixelShader> secondGeneration;
		secondGeneration.Attach(patcher.GetPatchedPSByHash(hash));
		tests.Require(patcher.IsReady() && secondGeneration != nullptr,
			"second device initialization recreates retained supported shader");

		secondGeneration.Reset();
		patcher.Reset();
		patcher.Initialize(device);
		ComPtr<ID3D11PixelShader> afterReset;
		afterReset.Attach(patcher.GetPatchedPSByHash(hash));
		tests.Require(patcher.IsInitialized() && !patcher.IsReady() &&
			afterReset == nullptr,
			"explicit Reset discards captured bytecode and exact hash mapping");
		patcher.Reset();
	}

	bool ParseDescriptor(const std::filesystem::path& path, uint32_t& descriptor)
	{
		std::string stem = path.stem().string();
		const size_t suffix = stem.find('_');
		if (suffix != std::string::npos)
			stem.resize(suffix);
		if (stem.empty())
			return false;
		const char* first = stem.data();
		const char* last = first + stem.size();
		const auto [end, error] = std::from_chars(first, last, descriptor, 16);
		return error == std::errc{} && end == last;
	}

	void RunCorpusTests(
		TestState& tests,
		ID3D11Device* device,
		const std::filesystem::path& corpus,
		const SIE::DXBCPatch& patch,
		uint32_t factorRegister)
	{
		std::error_code error;
		tests.Require(std::filesystem::is_directory(corpus, error) && !error,
			"--corpus points to a DFLight/PS directory");
		if (error || !std::filesystem::is_directory(corpus, error))
			return;

		uint32_t total = 0;
		uint32_t matched = 0;
		std::array<uint32_t, 10> variants{};
		std::unordered_map<uint64_t, std::vector<uint8_t>> matchedBytecodeByHash;
		std::filesystem::directory_iterator iterator(corpus, error), end;
		for (; !error && iterator != end; iterator.increment(error)) {
			const auto& entry = *iterator;
			std::error_code entryError;
			if (!entry.is_regular_file(entryError) || entryError)
				continue;
			std::string extension = entry.path().extension().string();
			std::transform(extension.begin(), extension.end(), extension.begin(),
				[](unsigned char value) { return static_cast<char>(std::tolower(value)); });
			if (extension != ".dxbc")
				continue;

			total++;
			const std::vector<uint8_t> bytes = ReadFile(entry.path());
			if (bytes.empty() || bytes.size() > std::numeric_limits<uint32_t>::max()) {
				tests.Require(false, "every corpus file is readable and bounded");
				continue;
			}
			SIE::DXBCPatcher::ParsedDXBC parsed{};
			if (!SIE::DXBCPatcher::Parse(
					bytes.data(), static_cast<uint32_t>(bytes.size()), parsed)) {
				tests.Require(false, "every corpus DXBC container parses strictly");
				continue;
			}

			ComPtr<ID3DBlob> vanilla = MakeBlob(bytes);
			SIE::DXBCPatcher::SunShadowPatchInfo info{};
			ComPtr<ID3DBlob> output;
			output.Attach(vanilla ? SIE::DXBCPatcher::PatchSunShadowShader(
				vanilla.Get(), patch, factorRegister, &info) : nullptr);
			if (!output) {
				tests.Require(!info.signatureMatched,
					"matched corpus signature never fails during splice");
				continue;
			}

			matched++;
			tests.Require(info.signatureVariant >= 1 && info.signatureVariant <= 10,
				"corpus match reports a known signature variant");
			if (info.signatureVariant >= 1 && info.signatureVariant <= 10)
				variants[info.signatureVariant - 1]++;
			const bool twoTargets =
				info.signatureVariant == 4 || info.signatureVariant == 5 ||
				info.signatureVariant == 10;
			tests.Require(info.scaledTargetCount == (twoTargets ? 2u : 1u),
				"corpus variant scales only its exact direct-sun target count");

			uint32_t descriptor = 0;
			const bool descriptorParsed = ParseDescriptor(entry.path(), descriptor);
			const uint32_t lightKind = descriptor & 0x7F;
			tests.Require(descriptorParsed && (descriptor & 0x100) == 0 &&
				(lightKind == 1 || lightKind == 2),
				"every matched corpus shader has a directional-sun descriptor");

			const uint64_t hash = SIE::DFLightPatcher::HashDXBC(
				bytes.data(), bytes.size());
			const auto [identity, inserted] =
				matchedBytecodeByHash.try_emplace(hash, bytes);
			tests.Require(inserted || identity->second == bytes,
				"any repeated runtime hash has byte-identical DXBC");

			ComPtr<ID3DBlob> disassembly;
			tests.Require(SUCCEEDED(D3DDisassemble(
				output->GetBufferPointer(), output->GetBufferSize(),
				0, nullptr, &disassembly)),
				"every patched corpus shader disassembles");
			ComPtr<ID3D11PixelShader> shader;
			tests.Require(SUCCEEDED(device->CreatePixelShader(
				output->GetBufferPointer(), output->GetBufferSize(), nullptr, &shader)) &&
				shader != nullptr,
				"every patched corpus shader creates on WARP");
		}
		tests.Require(!error, "corpus directory iteration completes without I/O errors");

		struct ExpectedCorpus
		{
			uint32_t total;
			uint32_t matched;
			std::size_t unique;
			std::array<uint32_t, 10> variants;
			const char* name;
		};
		static constexpr std::array<ExpectedCorpus, 3> kExpectedCorpora{
			ExpectedCorpus{
				1243, 138, 66, { 55, 10, 68, 3, 2, 0, 0, 0, 0, 0 },
				"FO4 1.10.163/flat" },
			ExpectedCorpus{
				174, 136, 64, { 0, 0, 0, 2, 0, 54, 10, 60, 7, 3 },
				"FO4 VR 1.2.72" },
			ExpectedCorpus{
				169, 64, 64, { 0, 0, 0, 1, 0, 23, 10, 22, 7, 1 },
				"FO4VR Deferred 0.5 compiler-normalized lighting" },
		};
		const auto expected = std::find_if(
			kExpectedCorpora.begin(), kExpectedCorpora.end(),
			[total](const ExpectedCorpus& candidate) {
				return candidate.total == total;
			});
		tests.Require(expected != kExpectedCorpora.end(),
			"corpus total identifies a reviewed flat or VR DFLight family");
		if (expected != kExpectedCorpora.end()) {
			tests.Require(matched == expected->matched,
				"corpus resolves the exact reviewed directional-sun count");
			tests.Require(variants == expected->variants,
				"corpus resolves the exact runtime-specific signature distribution");
			tests.Require(matchedBytecodeByHash.size() == expected->unique,
				"directional descriptors deduplicate to the exact reviewed bytecode count");
			std::cout << "Corpus profile: " << expected->name << '\n';
		}
		std::cout << "Corpus: total=" << total << " matched=" << matched
			<< " unique=" << matchedBytecodeByHash.size()
			<< " variants=";
		for (std::size_t index = 0; index < variants.size(); ++index) {
			if (index != 0)
				std::cout << '/';
			std::cout << variants[index];
		}
		std::cout << '\n';
	}
}

int wmain(int argc, wchar_t** argv)
{
	TestState tests;
	RunF4SECompatibilityTests(tests);
	std::filesystem::path corpus;
	if (argc != 1) {
		if (argc != 3 || std::wstring_view(argv[1]) != L"--corpus") {
			std::cerr << "usage: FO4CloudShadowsShaderToolsTests [--corpus <DFLight/PS>]\n";
			return 2;
		}
		corpus = argv[2];
	}

	std::vector<uint8_t> fixture;
	tests.Require(BuildSyntheticDirectSunFixture(fixture),
		"synthetic direct-sun DXBC fixture is generated at runtime");
	if (fixture.empty())
		return 1;

	uint32_t factorRegister = 0xFFFFFFFF;
	const SIE::DXBCPatch patch = BuildCloudPayload(factorRegister);
	tests.Require(!patch.preRetInstructions.empty() &&
		factorRegister < patch.additionalTempRegisters,
		"standalone t47 cloud payload builds with a valid factor temp");
	if (patch.preRetInstructions.empty())
		return 1;

	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	D3D_FEATURE_LEVEL featureLevel{};
	const HRESULT deviceHr = D3D11CreateDevice(
		nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
		D3D11_SDK_VERSION, &device, &featureLevel, &context);
	tests.Require(SUCCEEDED(deviceHr) && device != nullptr,
		"WARP D3D11 device is available for bytecode validation");
	if (!device)
		return 1;

	RunContainerTests(tests, fixture);
	RunStrictPatchTests(tests, device.Get(), fixture, patch, factorRegister);
	std::vector<uint8_t> recompiled = fixture;
	tests.Require(ReplaceWithDirectSunFixture(recompiled, true),
		"compiler-normalized sunlight fixture is generated");
	RunStrictPatchTests(tests, device.Get(), recompiled, patch, factorRegister);
	SIE::DXBCPatcher::ParsedDXBC recompiledParsed{};
	if (SIE::DXBCPatcher::Parse(recompiled.data(), static_cast<uint32_t>(recompiled.size()), recompiledParsed)) {
		const auto streamOffset = static_cast<size_t>(
			reinterpret_cast<const uint8_t*>(recompiledParsed.shex.instrStream) - recompiled.data());
		const auto suffixOffset = recompiledParsed.shex.instrStreamDwords - 21;
		auto wrongNormalization = recompiled;
		WriteU32(wrongNormalization, streamOffset + (suffixOffset + 6u) * 4u, 0x3F000000);
		RecomputeChecksum(wrongNormalization);
		tests.Require(Rejects(wrongNormalization, patch, factorRegister),
			"a different output normalization is not the reviewed compiler suffix");
		auto wrongAlpha = recompiled;
		WriteU32(wrongAlpha, streamOffset + (suffixOffset + 14u) * 4u, 0x3F800000);
		RecomputeChecksum(wrongAlpha);
		tests.Require(Rejects(wrongAlpha, patch, factorRegister),
			"a different output alpha is not the reviewed compiler suffix");
	}
	RunLifecycleTests(tests, device.Get(), fixture);
	RunRecompiledCompositionTests(tests, device.Get(), context.Get(), patch, factorRegister);
	if (!corpus.empty())
		RunCorpusTests(tests, device.Get(), corpus, patch, factorRegister);

	if (tests.Failures() != 0) {
		std::cerr << tests.Failures() << " ShaderTools test(s) failed\n";
		return 1;
	}
	std::cout << "PASS: ShaderTools generated contracts";
	if (!corpus.empty())
		std::cout << " + exhaustive stock corpus";
	std::cout << '\n';
	return 0;
}
