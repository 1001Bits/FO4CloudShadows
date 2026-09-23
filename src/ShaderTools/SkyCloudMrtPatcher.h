// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <d3dcompiler.h>

#include <cstddef>
#include <cstdint>

namespace SIE
{
	enum class SkyCloudPixelShaderVariant : uint32_t
	{
		kUnsupported = 0,
		kFlatClouds,
		kFlatCloudsLerp,
		kFlatCloudsFade,
		kVRClouds,
		kVRCloudsLerp,
		kVRCloudsFade
	};

	/// Private SV_Target1 payload emitted by the authenticated cloud shader.
	enum class SkyCloudMrtPayload : uint32_t
	{
		/// Legacy composite capture: stock visible-cloud alpha in every channel.
		kOpacity = 0,
		/// Per-layer live-animation mapping for an RGBA32F target:
		///   xy = the draw's animated texture coordinate;
		///   z  = native interpolated cloud/vertex alpha;
		///   w  = geometry-validity marker (1 for covered pixels).
		kUvMapping
	};

	struct SkyCloudMrtPatchInfo
	{
		uint64_t sourceHash = 0;
		uint32_t sourceSize = 0;
		SkyCloudPixelShaderVariant variant =
			SkyCloudPixelShaderVariant::kUnsupported;
		SkyCloudMrtPayload payload = SkyCloudMrtPayload::kOpacity;
		bool identityMatched = false;
		bool structureMatched = false;
		bool mappingInputsMatched = false;
		bool patchVerified = false;
	};

	/// Builds the cubemap-only version of an authenticated stock BSSky cloud
	/// pixel shader.
	///
	/// Fallout 4 already declares SV_Target0 and SV_Target1 in all six stock
	/// Clouds/CloudsLerp/CloudsFade pixel shaders. Target 1 normally contains
	/// motion vectors. During the native reflection-cubemap pass it is safe to
	/// bind a private RTV at slot 1 and use this replacement shader, which:
	///
	///  * preserves the complete stock SV_Target0 value;
	///  * writes either stock opacity or the per-layer animated UV/alpha/validity
	///    mapping selected by SkyCloudMrtPayload to SV_Target1; and
	///  * changes no texture, sampler, input, or constant-buffer ABI.
	///
	/// The method fails closed unless the source is one of the exact stock
	/// 1.10.163/1.11.240/VR 1.2.72 cloud bytecodes and its decoded output layout
	/// matches the audited two-target contract. The returned blob is owned by
	/// the caller.
	class SkyCloudMrtPatcher
	{
	public:
		static uint64_t HashDXBC(const void* data, size_t size) noexcept;

		static SkyCloudPixelShaderVariant Identify(
			const void* data, size_t size) noexcept;

		static ID3DBlob* Patch(
			ID3DBlob* stockCloudShader,
			SkyCloudMrtPatchInfo* outInfo = nullptr);

		/// Selects the private Target1 payload. Mapping mode is accepted only when
		/// the authenticated shader also declares the exact v1.xy texture-coordinate
		/// and v2.w native cloud-alpha inputs consumed by the injected tail.
		static ID3DBlob* Patch(
			ID3DBlob* stockCloudShader,
			SkyCloudMrtPayload payload,
			SkyCloudMrtPatchInfo* outInfo = nullptr);

		/// Convenience overload for ID3D11Device::CreatePixelShader interception,
		/// where the exact stock bytes are available before the live shader object
		/// is created.
		static ID3DBlob* Patch(
			const void* stockBytecode,
			size_t bytecodeSize,
			SkyCloudMrtPatchInfo* outInfo = nullptr);

		static ID3DBlob* Patch(
			const void* stockBytecode,
			size_t bytecodeSize,
			SkyCloudMrtPayload payload,
			SkyCloudMrtPatchInfo* outInfo = nullptr);

		static constexpr bool IsFlat(
			SkyCloudPixelShaderVariant variant) noexcept
		{
			return variant >= SkyCloudPixelShaderVariant::kFlatClouds &&
				variant <= SkyCloudPixelShaderVariant::kFlatCloudsFade;
		}

		static constexpr bool IsVR(
			SkyCloudPixelShaderVariant variant) noexcept
		{
			return variant >= SkyCloudPixelShaderVariant::kVRClouds &&
				variant <= SkyCloudPixelShaderVariant::kVRCloudsFade;
		}
	};
}
