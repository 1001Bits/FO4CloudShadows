// SPDX-License-Identifier: GPL-3.0-or-later
// Engine ABI/formulas adapted from Fallout 4 Community Shaders at
// d1ccd465cb38b81baad90973953be1a95e025644; see THIRD_PARTY_NOTICES.md.
// Standalone FO4 world-cloud shadow mask.
//
// The cloud source is an immutable R16_FLOAT (R32_FLOAT fallback) TextureCube
// rasterized each main-sky frame from live stock cloud geometry, textures and
// constants. All overlapping surfaces blend; clear/blue sky is exactly zero.
//
// CPU ABI requirements:
//   * b1 is the same ID3D11Buffer that the active DFLight PS has at b12.
//   * Non-VR: b12 c20-c27 are the far/near reprojection rows used by stock
//     DFLight. b12 c12-c15 is inverse view and c35 is the absolute camera.
//   * VR: b12 c32-c39 / c40-c47 are the exact eye-indexed far/near inverse
//     view-projection rows observed in vanilla VR DFLight DXBC. They recover
//     eye-relative Ni-world coordinates; c59/c60 supply the absolute eyes.
//     The output and depth textures are doubled-width stereo [left|right].
//   * b2 is the active DFLight PS b2 unchanged. Non-VR uses c0/c1/c27; VR uses
//     c0/c1/c2/c45 exactly as observed in the same corpus.
//   * VR b8 c0 is rebound unchanged at b3. Its x component is used by the
//     stock side-by-side NDC transform; assuming a fixed stereo separation
//     would make reconstruction diverge from the consuming DFLight draw.
//   * WorldCloudTiles is sampled directionally at LOD 0.

#ifndef FO4CS_SHADER_VR
#define FO4CS_SHADER_VR 0
#endif
#ifndef FO4CS_SUN_MASK
#define FO4CS_SUN_MASK 0
#endif

Texture2D<float> DepthTexture : register(t0);
#if FO4CS_SUN_MASK
Texture2D<float> WorldCloudTiles : register(t1);
#else
TextureCube<float> WorldCloudTiles : register(t1);
#endif
SamplerState WorldCloudSampler : register(s0);
SamplerState EngineDepthSampler : register(s1);
RWTexture2D<float> OutputTexture : register(u0);
// Eight float4 records written only by dispatch thread (0,0). This UAV is
// diagnostic-only; an unavailable/null binding never changes OutputTexture.
RWStructuredBuffer<float4> CloudTelemetry : register(u1);
// One only where native depth produced a finite scene receiver. This separate
// diagnostic surface prevents clear-sky/background identity pixels from being
// misreported as evidence for correctly lit terrain. It is bound and written
// only for a dispatch satisfying an explicit screen-evidence request.
RWTexture2D<uint> ReceiverValidity : register(u2);

#define CLOUD_SHADOW_MAX_LAYERS 16

// Plugin-owned constants. Keep packoffsets in exact sync with the C++ ABI.
cbuffer CloudShadowScreenCB : register(b0)
{
	float4 ViewToWorld_row0 : packoffset(c0);
	float4 ViewToWorld_row1 : packoffset(c1);
	float4 ViewToWorld_row2 : packoffset(c2);
	float4 ViewToWorld_row3 : packoffset(c3);

	// xy = writable output extent; zw = reciprocal extent.
	float4 OutputSizeAndInvSize : packoffset(c4);
	// depthUV = outputPixelCentre * xy + zw.
	float4 OutputPixelToDepthUV : packoffset(c5);
	// ndc = outputPixelCentre * xy + zw (including the Y inversion).
	float4 OutputPixelToNDC : packoffset(c6);

	// xyz is the optional single-cloud diagnostic selector direction; w is the
	// sun angular semi-diameter in radians (about 0.00465 for the real sun).
	float4 SunDirectionAndAngularRadius : packoffset(c7);
	// x opacity, y source/capture validity, z explicit receiver-validity write
	// enable; w enables acceptance/menu telemetry. Production uses only x/y.
	float4 ShadowParams : packoffset(c8);
	// x layer count; yz single-cloud outer/inner cap cosines (-2 disables);
	// w debug mode.
	float4 ModelParams : packoffset(c9);

	// x cloud height above the planet surface; y planet radius; z is retained
	// for the stable CPU ABI; w is negative only when the field is disabled.
	float4 LayerGeometry[CLOUD_SHADOW_MAX_LAYERS] : packoffset(c10);
	// xyz committed world-fixed cloud-field origin; w layer activity.
	float4 LayerOptics[CLOUD_SHADOW_MAX_LAYERS] : packoffset(c26);
	// xyz absolute camera/player proxy; w horizontal diagnostic disk radius.
	float4 DiagnosticReceiverAndRadius : packoffset(c42);
    // Visible solar disc direction; w=1 valid, -1 invalid, 0 engine-basis test.
    float4 VisibleSunDirectionAndValidity : packoffset(c43);
    float4 SunMaskRightAndHalfWidth : packoffset(c44);
    float4 SunMaskUpAndHeight : packoffset(c45);
    float4 SunMaskDirectionAndPlanet : packoffset(c46);
    float4 SunMaskCenterAndValid : packoffset(c47);
};

#if !FO4CS_SHADER_VR
// Exact native flat/AE DFLight b12 layout, rebound unchanged at CS b1.
// OG Renderer::SetPerFrameConstants (RVA 0x1D11160) writes transposed inverse
// view at byte 0xC0, inverse projection at 0x140, and posAdjust at 0x230.
// Unjittered view-projection is at 0x250 (c37), not at c12. Using the bound
// inverse view avoids substituting a different/stale scene camera.
cbuffer EnginePerFrame : register(b1)
{
	float4 EnginePerFrame_pad_0_11[12] : packoffset(c0);
	float4 EngineViewToWorld_row0 : packoffset(c12);
	float4 EngineViewToWorld_row1 : packoffset(c13);
	float4 EngineViewToWorld_row2 : packoffset(c14);
	float4 EngineViewToWorld_row3 : packoffset(c15);
	float4 EnginePerFrame_pad_16_19[4] : packoffset(c16);
	float4 FarReprojection_row0 : packoffset(c20);
	float4 FarReprojection_row1 : packoffset(c21);
	float4 FarReprojection_row2 : packoffset(c22);
	float4 FarReprojection_row3 : packoffset(c23);
	float4 NearReprojection_row0 : packoffset(c24);
	float4 NearReprojection_row1 : packoffset(c25);
	float4 NearReprojection_row2 : packoffset(c26);
	float4 NearReprojection_row3 : packoffset(c27);
	float4 EnginePerFrame_pad_28_34[7] : packoffset(c28);
	float4 EngineCameraPosition : packoffset(c35);
};

cbuffer EnginePerCall : register(b2)
{
	float4 EngineScreenSize : packoffset(c0);
	float4 EngineSunDirectionView : packoffset(c1);
	float4 EnginePerCall_pad_2_26[25] : packoffset(c2);
	float4 EngineDepthUVScale : packoffset(c27);
};
#else
// Exact native VR DFLight b12/b2 layout observed in vanilla VR DFLight DXBC:
//   * c32-c39 = far inverse reprojection for eye 0/1
//   * c40-c47 = near inverse reprojection for eye 0/1
//   * c59/c60 = absolute per-eye Ni-world positions
//   * c1/c2   = per-eye sun direction in that same Ni-world basis
//   * c45.xy  = additional stereo depth-UV scale
cbuffer EnginePerFrame : register(b1)
{
	float4 EnginePerFrame_pad_0_31[32] : packoffset(c0);
	float4 FarReprojectionEye0_row0 : packoffset(c32);
	float4 FarReprojectionEye0_row1 : packoffset(c33);
	float4 FarReprojectionEye0_row2 : packoffset(c34);
	float4 FarReprojectionEye0_row3 : packoffset(c35);
	float4 FarReprojectionEye1_row0 : packoffset(c36);
	float4 FarReprojectionEye1_row1 : packoffset(c37);
	float4 FarReprojectionEye1_row2 : packoffset(c38);
	float4 FarReprojectionEye1_row3 : packoffset(c39);
	float4 NearReprojectionEye0_row0 : packoffset(c40);
	float4 NearReprojectionEye0_row1 : packoffset(c41);
	float4 NearReprojectionEye0_row2 : packoffset(c42);
	float4 NearReprojectionEye0_row3 : packoffset(c43);
	float4 NearReprojectionEye1_row0 : packoffset(c44);
	float4 NearReprojectionEye1_row1 : packoffset(c45);
	float4 NearReprojectionEye1_row2 : packoffset(c46);
	float4 NearReprojectionEye1_row3 : packoffset(c47);
	float4 EnginePerFrame_pad_48_58[11] : packoffset(c48);
	float4 EngineEyePosition0 : packoffset(c59);
	float4 EngineEyePosition1 : packoffset(c60);
};

cbuffer EnginePerCall : register(b2)
{
	float4 EngineScreenSize : packoffset(c0);
	float4 EngineSunDirectionWorldEye0 : packoffset(c1);
	float4 EngineSunDirectionWorldEye1 : packoffset(c2);
	float4 EnginePerCall_pad_3_44[42] : packoffset(c3);
	float4 EngineDepthUVScaleStereo : packoffset(c45);
};

cbuffer EngineStereoParams : register(b3)
{
	float4 EngineStereoSeparation : packoffset(c0);
};
#endif

// Portable finite test. Proton/Wine's vkd3d-based d3dcompiler lacks the
// isfinite intrinsic, and fxc may fold NaN comparisons; the exponent bit test
// is exact IEEE-754 on both compilers. A macro, not overloads: vkd3d cannot
// prioritize between compatible overloads (E5017).
#define IS_FINITE(x) ((asuint(x) & 0x7F800000u) != 0x7F800000u)
bool GetNormalizedSunDirection(uint eyeIndex, out float3 sunDirection)
{
    if (VisibleSunDirectionAndValidity.w < -0.5) {
        sunDirection = 0.0;
        return false;
    }
#if FO4CS_SHADER_VR
	sunDirection = eyeIndex != 0u ?
		EngineSunDirectionWorldEye1.xyz :
		EngineSunDirectionWorldEye0.xyz;
#else
	// DFLight SetupGeometry maps world sunlight through this draw's view
	// matrix. Undo that exact transform with the paired GPU inverse view.
	sunDirection = float3(
		dot(EngineViewToWorld_row0.xyz, EngineSunDirectionView.xyz),
		dot(EngineViewToWorld_row1.xyz, EngineSunDirectionView.xyz),
		dot(EngineViewToWorld_row2.xyz, EngineSunDirectionView.xyz));
#endif
	if (VisibleSunDirectionAndValidity.w > 0.5)
        sunDirection = VisibleSunDirectionAndValidity.xyz;
    float lengthSquared = dot(sunDirection, sunDirection);
	if (!all(IS_FINITE(sunDirection)) || lengthSquared <= 1.0e-8) {
		sunDirection = 0.0;
		return false;
	}
	sunDirection *= rsqrt(lengthSquared);
	return all(IS_FINITE(sunDirection));
}

float3 GetCloudFieldOrigin()
{
	// The producer confirms this origin from the first valid player lighting
	// view, and retains it until a world/load transition. Recentring the shell
	// on EngineCameraPosition makes every terrain shadow slide with the player.
	return LayerOptics[0].xyz;
}

float3 GetRenderCameraOrigin()
{
#if FO4CS_SHADER_VR
	return 0.5 * (EngineEyePosition0.xyz + EngineEyePosition1.xyz);
#else
	return EngineCameraPosition.xyz;
#endif
}

float4 MultiplyRows(
	float4 row0, float4 row1, float4 row2, float4 row3, float4 value)
{
	return float4(
		dot(row0, value),
		dot(row1, value),
		dot(row2, value),
		dot(row3, value));
}

#if FO4CS_SHADER_VR
uint GetStereoEyeIndex(float2 uv)
{
	return uv.x >= 0.5 ? 1u : 0u;
}

float4 MultiplyReprojectionRowsVR(
	float4 row0, float4 row1, float4 row2, float4 row3,
	float4 vanillaPosition)
{
	// Vanilla packs the source as (ndc.x, depth, ndc.y, 1) and therefore
	// swizzles every matrix row .xzyw before its dp4. Keep that exact operand
	// ordering visible here rather than relying on an algebraic reordering.
	return float4(
		dot(row0.xzyw, vanillaPosition),
		dot(row1.xzyw, vanillaPosition),
		dot(row2.xzyw, vanillaPosition),
		dot(row3.xzyw, vanillaPosition));
}

float4 MultiplyFarReprojectionVR(uint eyeIndex, float4 value)
{
	return eyeIndex != 0u ?
		MultiplyReprojectionRowsVR(
			FarReprojectionEye1_row0, FarReprojectionEye1_row1,
			FarReprojectionEye1_row2, FarReprojectionEye1_row3,
			value) :
		MultiplyReprojectionRowsVR(
			FarReprojectionEye0_row0, FarReprojectionEye0_row1,
			FarReprojectionEye0_row2, FarReprojectionEye0_row3,
			value);
}

float4 MultiplyNearReprojectionVR(uint eyeIndex, float4 value)
{
	return eyeIndex != 0u ?
		MultiplyReprojectionRowsVR(
			NearReprojectionEye1_row0, NearReprojectionEye1_row1,
			NearReprojectionEye1_row2, NearReprojectionEye1_row3,
			value) :
		MultiplyReprojectionRowsVR(
			NearReprojectionEye0_row0, NearReprojectionEye0_row1,
			NearReprojectionEye0_row2, NearReprojectionEye0_row3,
			value);
}
#endif

// Matches native DFLight's split depth encoding exactly:
//   encoded <= 0.01 : nearDepth = encoded * 100
//   encoded >  0.01 : farDepth  = encoded * 1.01 - 0.01
bool ReconstructWorldPosition(
	uint2 outputPixel,
	out float rawDepth,
	out float3 world,
	out uint eyeIndex)
{
	float2 outputPixelCentre = float2(outputPixel) + 0.5;
#if FO4CS_SHADER_VR
	float2 outputUV = outputPixelCentre * OutputSizeAndInvSize.zw;
	eyeIndex = GetStereoEyeIndex(outputUV);

	float2 screenUV = outputPixelCentre * EngineScreenSize.xy;
	float2 depthUV = screenUV * EngineDepthUVScaleStereo.xy;
	if (any(depthUV < 0.0) || any(depthUV >= 1.0)) {
		rawDepth = 1.0;
		world = 0.0;
		return false;
	}

	float2 depthTexelStep = EngineScreenSize.xy * EngineDepthUVScaleStereo.xy;
	rawDepth = DepthTexture.SampleGrad(
		EngineDepthSampler, depthUV,
		depthTexelStep.xx, depthTexelStep.yy);

	// Exact stock SBS transform (VR DFLight):
	//   baseX = SV_Position.x * b2.c0.x * 2 - 1
	//   x = (baseX + (+0.5 left / -0.5 right) * b8.c0.x)
	//       * (b8.c0.x + 1)
	//   y = (1 - SV_Position.y * b2.c0.y) * 2 - 1
	const float stereoSeparation = EngineStereoSeparation.x;
	if (!IS_FINITE(stereoSeparation)) {
		rawDepth = 1.0;
		world = 0.0;
		return false;
	}
	float2 nativeNDC = screenUV * float2(2.0, -2.0) +
		float2(-1.0, 1.0);
	const float eyeOffset = eyeIndex != 0u ? -0.5 : 0.5;
	nativeNDC.x = (nativeNDC.x + eyeOffset * stereoSeparation) *
		(stereoSeparation + 1.0);

	float nativeDepth;
	float4 relativeWorldH;
	if (rawDepth <= 0.01) {
		nativeDepth = rawDepth * 100.0;
		relativeWorldH = MultiplyNearReprojectionVR(
			eyeIndex,
			float4(nativeNDC.x, nativeDepth, nativeNDC.y, 1.0));
	} else {
		nativeDepth = rawDepth * 1.01 - 0.01;
		relativeWorldH = MultiplyFarReprojectionVR(
			eyeIndex,
			float4(nativeNDC.x, nativeDepth, nativeNDC.y, 1.0));
	}

	if (!all(IS_FINITE(relativeWorldH)) ||
		abs(relativeWorldH.w) <= 1.0e-7) {
		world = 0.0;
		return false;
	}
	// c32/c40 are inverse ViewProj, not inverse projection. Vanilla VR VS
	// proves the basis: it maps eye-relative world through c12[12+eye*4],
	// maps that clip position back through c12[32+eye*4], then adds
	// c12[59+eye].z for absolute fog height. Therefore rotating this value by
	// the Ni camera again would double-rotate it.
	const float3 eyePosition = eyeIndex != 0u ?
		EngineEyePosition1.xyz : EngineEyePosition0.xyz;
	world = relativeWorldH.xyz / relativeWorldH.w + eyePosition;
	return all(IS_FINITE(world));
#else
	eyeIndex = 0u;
	// Exact stock 1.10.163 DFLight sequence:
	//   depthUV = SV_Position.xy * b2.c27.xy * b2.c0.xy
	//   screenUV = SV_Position.xy * b2.c0.xy
	// c0.zw are not the NDC scale.
	// 1.11.240's DFLight per-call buffer has 25 registers and its depth
	// fetch uses SV_Position.xy * c0.xy only (no dynamic-resolution scale);
	// c27 lies outside that bound buffer and reads as zero. A zero or
	// non-finite scale is never legitimate, so it selects the unscaled fetch
	// (verified 10 Sep 2026: AE masks were uniformly neutral with zero valid
	// receivers before this guard; OG behaviour is unchanged).
	float2 depthUVScale = (all(IS_FINITE(EngineDepthUVScale.xy)) &&
		all(EngineDepthUVScale.xy > 0.0)) ? EngineDepthUVScale.xy : float2(1.0, 1.0);
	float2 screenUV = outputPixelCentre * EngineScreenSize.xy;
	float2 depthUV = screenUV * depthUVScale;
	if (any(depthUV < 0.0) || any(depthUV >= 1.0)) {
		rawDepth = 1.0;
		world = 0.0;
		return false;
	}

	// Native DFLight uses SampleGrad with the scalar coarse derivatives
	// splatted into both gradient components. For a one-thread-per-pixel
	// dispatch those derivatives are exactly ScreenSize.x and ScreenSize.y.
	float2 depthTexelStep = EngineScreenSize.xy * depthUVScale;
	rawDepth = DepthTexture.SampleGrad(
		EngineDepthSampler, depthUV,
		depthTexelStep.xx, depthTexelStep.yy);

	float nativeDepth;
	float4 viewH;
	float2 ndc = screenUV * float2(2.0, -2.0) + float2(-1.0, 1.0);
	if (rawDepth <= 0.01) {
		nativeDepth = rawDepth * 100.0;
		viewH = MultiplyRows(
			NearReprojection_row0, NearReprojection_row1,
			NearReprojection_row2, NearReprojection_row3,
			float4(ndc, nativeDepth, 1.0));
	} else {
		nativeDepth = rawDepth * 1.01 - 0.01;
		viewH = MultiplyRows(
			FarReprojection_row0, FarReprojection_row1,
			FarReprojection_row2, FarReprojection_row3,
			float4(ndc, nativeDepth, 1.0));
	}

	if (!all(IS_FINITE(viewH)) || abs(viewH.w) <= 1.0e-7) {
		world = 0.0;
		return false;
	}
	// Stock DFLight reconstructs view space. Use the inverse view and origin
	// uploaded with these projection rows, not a separately read scene camera.
	float3 relativeView = viewH.xyz / viewH.w;
	float4 relativePosition = float4(relativeView, 1.0);
	float3 worldPosition = float3(
		dot(EngineViewToWorld_row0, relativePosition),
		dot(EngineViewToWorld_row1, relativePosition),
		dot(EngineViewToWorld_row2, relativePosition)) + EngineCameraPosition.xyz;
	if (!all(IS_FINITE(worldPosition))) {
		world = 0.0;
		return false;
	}
	world = worldPosition;
	return true;
#endif
}

// The preview reconstructs two camera-space points using the SAME GPU
// reprojection rows as native DFLight. Subtracting them removes translation
// without consulting a CPU camera, terrain receiver, sun, or field origin.
bool GetCameraRay(uint2 pixel, out float3 ray)
{
    float2 screenUV = (float2(pixel) + 0.5) * EngineScreenSize.xy;
    float2 ndc = screenUV * float2(2, -2) + float2(-1, 1);
    float4 a, b;
#if FO4CS_SHADER_VR
    uint eye = GetStereoEyeIndex((float2(pixel) + 0.5) * OutputSizeAndInvSize.zw);
    float separation = EngineStereoSeparation.x;
    ndc.x = (ndc.x + (eye != 0 ? -0.5 : 0.5) * separation) * (separation + 1);
    a = MultiplyFarReprojectionVR(eye, float4(ndc.x, 0.1, ndc.y, 1));
    b = MultiplyFarReprojectionVR(eye, float4(ndc.x, 0.9, ndc.y, 1));
#else
    a = MultiplyRows(FarReprojection_row0, FarReprojection_row1,
        FarReprojection_row2, FarReprojection_row3, float4(ndc, 0.1, 1));
    b = MultiplyRows(FarReprojection_row0, FarReprojection_row1,
        FarReprojection_row2, FarReprojection_row3, float4(ndc, 0.9, 1));
#endif
    ray = 0;
    if (!all(IS_FINITE(a)) || !all(IS_FINITE(b)) || abs(a.w) < 1.0e-7 || abs(b.w) < 1.0e-7)
        return false;
    float3 delta = b.xyz / b.w - a.xyz / a.w;
#if !FO4CS_SHADER_VR
    delta = float3(dot(EngineViewToWorld_row0.xyz, delta),
        dot(EngineViewToWorld_row1.xyz, delta), dot(EngineViewToWorld_row2.xyz, delta));
#endif
    float lengthSquared = dot(delta, delta);
    if (!all(IS_FINITE(delta)) || lengthSquared < 1.0e-10) return false;
    ray = delta * rsqrt(lengthSquared);
    return true;
}

#if !FO4CS_SUN_MASK
RWTexture2D<float4> SkyPreviewOutput : register(u0);
[numthreads(8, 8, 1)]
void mainSkyPreview(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= (uint2)OutputSizeAndInvSize.xy)) return;
    float3 ray;
    if (!GetCameraRay(id.xy, ray)) {
        SkyPreviewOutput[id.xy] = float4(1, 0.5, 0, 1);
        return;
    }
    float opacity = saturate(WorldCloudTiles.SampleLevel(WorldCloudSampler, ray, 0));
    if (ModelParams.w > 1.5) {
        // Raw capture: white = opaque cloud, black = clear direction.
        SkyPreviewOutput[id.xy] = float4(opacity.xxx, 1);
    } else {
        float rawDepth; float3 unused; uint eye;
        ReconstructWorldPosition(id.xy, rawDepth, unused, eye);
        // Tint only the visible sky; foreground objects remain readable.
        SkyPreviewOutput[id.xy] = float4(1, 0, 1,
            IS_FINITE(rawDepth) && rawDepth >= 1.0 ? opacity * 0.65 : 0);
    }
}
#endif

float SampleCapturedCloudPoint(float3 cloudPoint, float mip)
{
#if FO4CS_SUN_MASK
    if (SunMaskCenterAndValid.w < 0.5 || SunMaskRightAndHalfWidth.w <= 0) return 0;
    float3 relative = cloudPoint - SunMaskCenterAndValid.xyz;
    float2 uv = float2(dot(relative, SunMaskRightAndHalfWidth.xyz),
        -dot(relative, SunMaskUpAndHeight.xyz)) / (2 * SunMaskRightAndHalfWidth.w) + 0.5;
    if (!all(IS_FINITE(uv)) || any(uv < 0) || any(uv > 1)) return 0;
    float2 edge = min(uv, 1 - uv);
    float fade = saturate(min(edge.x, edge.y) * (512.0 / 8.0));
    return saturate(WorldCloudTiles.SampleLevel(WorldCloudSampler, uv, 0)) * fade;
#else
    return saturate(WorldCloudTiles.SampleLevel(WorldCloudSampler, normalize(cloudPoint), mip));
#endif
}

bool GetCloudShadowSamplePoint(
	float3 receiverAbsolute, float3 rayToSun, float cloudHeight,
	float planetRadius, float3 captureOrigin, out float3 samplePoint)
{
	samplePoint = 0.0;
	if (!IS_FINITE(cloudHeight) || !IS_FINITE(planetRadius) ||
		!all(IS_FINITE(captureOrigin)) || !all(IS_FINITE(receiverAbsolute)) ||
		!all(IS_FINITE(rayToSun)) || cloudHeight <= 0.0 || planetRadius <= 0.0)
		return false;

	float shellRadius = planetRadius + cloudHeight;
	if (!IS_FINITE(shellRadius) || shellRadius <= planetRadius)
		return false;

	// Put the receiver in the capture-centred spherical-shell frame. The
	// planet centre is one radius below the capture origin, matching Skyrim's
	// cloud-shadow projection while retaining Fallout's absolute world space.
	float3 relativeReceiver = receiverAbsolute - captureOrigin;
	float3 planetOffset = float3(0.0, 0.0, planetRadius);
	// Expand the thin-shell deficit before division. Computing
	// 1-|normalizedPlanetPosition|^2 subtracts two almost-equal float32 values
	// at Fallout's Earth-scale radius and shifts the hit by about a cube texel.
	float deficitNumerator =
		2.0 * planetRadius * (cloudHeight - relativeReceiver.z) +
		cloudHeight * cloudHeight - dot(relativeReceiver, relativeReceiver);
	if (!IS_FINITE(deficitNumerator) || deficitNumerator <= 0.0)
		return false;
	float radialDeficit = deficitNumerator / (shellRadius * shellRadius);
	float rayDot =
		(planetRadius * rayToSun.z + dot(relativeReceiver, rayToSun)) /
		shellRadius;
	float discriminant = rayDot * rayDot + radialDeficit;
	if (!IS_FINITE(rayDot) || !IS_FINITE(radialDeficit) ||
		!IS_FINITE(discriminant) || discriminant < 0.0)
		return false;

	// For an above-horizon sun ray, -rayDot + sqrt(discriminant) subtracts
	// nearly equal floats to recover the thin cloud-shell distance. Use the
	// rationalized positive root so a 35k-unit shell remains temporally stable.
	float root = sqrt(max(discriminant, 0.0));
	float stableDenominator = rayDot + root;
	if (!IS_FINITE(stableDenominator) || stableDenominator <= 1.0e-7)
		return false;
	float distanceToShell =
		shellRadius * radialDeficit / stableDenominator;
	if (!IS_FINITE(distanceToShell) || distanceToShell <= 0.0)
		return false;

	// Keep the point relative to the committed cubemap camera origin. This is the
	// exact TextureCube direction from the capture origin to the point where
	// this receiver-to-sun ray intersects the cloud shell.
	samplePoint = relativeReceiver + rayToSun * distanceToShell;
	return all(IS_FINITE(samplePoint));
}

float SampleCloudCoverage(
	float3 receiverAbsolute, float3 rayToSun, uint layer,
	bool applyIsolation,
	out float rawCoverage, out float broadCoverage,
	out float extractedCoverage, out bool sampleValid)
{
	rawCoverage = 0.0;
	broadCoverage = 0.0;
	extractedCoverage = 0.0;
	sampleValid = false;
	float4 geometry = LayerGeometry[layer];
	float4 optics = LayerOptics[layer];
	if (geometry.w < 0.0 || optics.w <= 0.0)
		return 0.0;
	float3 samplePoint;
	if (!GetCloudShadowSamplePoint(
		receiverAbsolute, rayToSun, geometry.x, geometry.y, GetCloudFieldOrigin(),
		samplePoint))
		return 0.0;
	sampleValid = true;

	float active = saturate(optics.w);
	float directionLengthSquared = dot(samplePoint, samplePoint);
	if (!IS_FINITE(directionLengthSquared) ||
		directionLengthSquared <= 1.0e-8) {
		sampleValid = false;
		return 0.0;
	}
	float3 sampleDirection = samplePoint * rsqrt(directionLengthSquared);
	float raw = SampleCapturedCloudPoint(samplePoint, 0.0);
	rawCoverage = raw * active;
	// The coarse face-wide sample exists only in the explicit three-panel
	// capture-analysis view. Production performs exactly the one LOD-zero
	// opacity lookup at the ray/shell intersection above.
	if (ModelParams.w > 3.5 && ModelParams.w < 4.5) {
		static const float broadMip = 8.0;
		float broad = SampleCapturedCloudPoint(samplePoint, broadMip);
		broadCoverage = broad * active;
	}

	// Production follows the physical cloud-shadow rule directly: this ray is
	// occluded by exactly the opacity contributed by Fallout's authenticated
	// visible-cloud draws at the shell intersection. The coarse mip is retained
	// only for the three-panel capture diagnostic; subtracting it here would
	// invent a contrast pattern rather than cast the clouds actually in the sky.
	float isolationWeight = 1.0;
	if (applyIsolation &&
		ModelParams.y >= -1.0 && ModelParams.z >= ModelParams.y) {
		float selectorLengthSquared = dot(
			SunDirectionAndAngularRadius.xyz,
			SunDirectionAndAngularRadius.xyz);
		float sampleLengthSquared = dot(samplePoint, samplePoint);
		if (!IS_FINITE(selectorLengthSquared) ||
			selectorLengthSquared <= 1.0e-8 ||
			!IS_FINITE(sampleLengthSquared) || sampleLengthSquared <= 1.0e-8) {
			isolationWeight = 0.0;
		} else {
			float directionDot = dot(
				SunDirectionAndAngularRadius.xyz *
					rsqrt(selectorLengthSquared),
				samplePoint * rsqrt(sampleLengthSquared));
			isolationWeight = smoothstep(
				ModelParams.y, ModelParams.z, directionDot);
		}
	}
	extractedCoverage = raw * active * isolationWeight;
	// The capture target is cleared to zero and excludes the authenticated
	// constant-fill atmosphere draw. Therefore a clear texture location is
	// exactly zero coverage, while every nonzero value came from live cloud
	// opacity in this captured Sky epoch.
	return extractedCoverage;
}

float SampleVisibleCloudCoverage(
	float3 receiverAbsolute, float3 rayToSun,
	bool applyIsolation,
	out float strongestRaw, out float strongestBroad,
	out float strongestExtracted, out bool sampleValid)
{
	uint layerCount = min((uint)max(ModelParams.x, 0.0),
		(uint)CLOUD_SHADOW_MAX_LAYERS);
	float strongestCoverage = 0.0;
	strongestRaw = 0.0;
	strongestBroad = 0.0;
	strongestExtracted = 0.0;
	sampleValid = false;
	[loop]
	for (uint layer = 0; layer < layerCount; ++layer) {
		float rawCoverage;
		float broadCoverage;
		float extractedCoverage;
		bool layerSampleValid;
		strongestCoverage = max(strongestCoverage,
			SampleCloudCoverage(
				receiverAbsolute, rayToSun, layer,
				applyIsolation, rawCoverage, broadCoverage,
				extractedCoverage, layerSampleValid));
		sampleValid = sampleValid || layerSampleValid;
		strongestRaw = max(strongestRaw, rawCoverage);
		strongestBroad = max(strongestBroad, broadCoverage);
		strongestExtracted = max(strongestExtracted, extractedCoverage);
	}
	// Normal publication contains one source-over-composited field. Retain max
	// only as fail-safe behavior for an older/mixed producer publishing more.
	return strongestCoverage;
}

float SampleVisibleCloudCoverage(float3 receiverAbsolute, float3 rayToSun)
{
	float unusedRaw;
	float unusedBroad;
	float unusedExtracted;
	bool unusedValid;
	return SampleVisibleCloudCoverage(
		receiverAbsolute, rayToSun, false,
		unusedRaw, unusedBroad, unusedExtracted, unusedValid);
}

void WritePhysicalCloudTelemetry(float3 sunDirection, bool sunValid)
{
	// Preserve the actual projection inputs even when the field is neutral.
	// Explicit evidence requests log these with the matching mask dispatch.
	CloudTelemetry[3] = float4(sunDirection, sunValid ? 1.0 : 0.0);
	CloudTelemetry[4] = 0.0;
	CloudTelemetry[5] = float4(0.0, 0.0, 0.0, -1.0);
	CloudTelemetry[6] = float4(GetRenderCameraOrigin(), LayerGeometry[0].x);
	CloudTelemetry[7] = LayerOptics[0];
	uint2 probePixel = (uint2)(OutputSizeAndInvSize.xy * float2(0.5, 0.75));
	float probeDepth;
	float3 probeReceiver;
	uint probeEye;
	if (ReconstructWorldPosition(probePixel, probeDepth, probeReceiver, probeEye)) {
		CloudTelemetry[4] = float4(probeReceiver, probeDepth);
		float3 probeSun;
		float3 probePoint;
		if (probeDepth >= 0.0 && probeDepth < 1.0 &&
			GetNormalizedSunDirection(probeEye, probeSun) && probeSun.z > 0.0 &&
			GetCloudShadowSamplePoint(probeReceiver, probeSun,
				LayerGeometry[0].x, LayerGeometry[0].y, GetCloudFieldOrigin(), probePoint) &&
			dot(probePoint, probePoint) > 1.0e-8) {
			float3 direction = normalize(probePoint);
			CloudTelemetry[5] = float4(direction,
				SampleCapturedCloudPoint(probePoint, 0.0));
		}
	}
	// Invalid is explicit and neutral. The CPU publishes nothing from a record
	// whose validity component is zero or non-finite.
	uint layerCount = min((uint)max(ModelParams.x, 0.0),
		(uint)CLOUD_SHADOW_MAX_LAYERS);
	// Negative w means the diagnostic dispatch completed but the physical
	// prerequisites were ineligible. y and z independently report the observed
	// sun elevation and committed-field count for acceptance classification.
	CloudTelemetry[0] = float4(
		0.0, sunValid ? sunDirection.z : -2.0, (float)layerCount, -1.0);
	CloudTelemetry[1] = float4(0.0, 0.0, 0.0, -1.0);
	CloudTelemetry[2] = 0.0;

	float3 centre = GetRenderCameraOrigin();
	float radius = DiagnosticReceiverAndRadius.w;
	if (!sunValid || sunDirection.z <= 0.0 || layerCount == 0u ||
		!all(IS_FINITE(centre)) || !IS_FINITE(radius) || radius <= 0.0)
		return;

	float unusedRaw;
	float unusedBroad;
	float unusedExtracted;
	bool centreValid;
	float centreOpacity = SampleVisibleCloudCoverage(
		centre, sunDirection, false,
		unusedRaw, unusedBroad, unusedExtracted, centreValid);
	if (!centreValid || !IS_FINITE(centreOpacity))
		return;

	// Reproducible local-area definition: 9x9 equal-area cell centres over a
	// horizontal square, retaining only centres inside the radius-4096 disk.
	// Every retained receiver has centre.z and traces the same exact ray/shell/
	// LOD-zero captured-field path as production. This is a plane diagnostic,
	// not an estimate of actual terrain surface area.
	static const uint gridWidth = 9u;
	static const float opacityThreshold = 0.01;
	float opacitySum = 0.0;
	float minimumOpacity = 1.0;
	float maximumOpacity = 0.0;
	uint shadowedSamples = 0u;
	uint sampleCount = 0u;
	bool neighbourhoodValid = true;
	[loop]
	for (uint y = 0u; y < gridWidth; ++y) {
		[loop]
		for (uint x = 0u; x < gridWidth; ++x) {
			float2 normalizedOffset =
				((float2(x, y) + 0.5) / (float)gridWidth) * 2.0 - 1.0;
			if (dot(normalizedOffset, normalizedOffset) > 1.0)
				continue;
			float3 receiver = centre +
				float3(normalizedOffset * radius, 0.0);
			bool receiverValid;
			float opacity = SampleVisibleCloudCoverage(
				receiver, sunDirection, false,
				unusedRaw, unusedBroad, unusedExtracted, receiverValid);
			neighbourhoodValid = neighbourhoodValid && receiverValid &&
				IS_FINITE(opacity);
			if (!receiverValid || !IS_FINITE(opacity))
				continue;
			opacity = saturate(opacity);
			opacitySum += opacity;
			minimumOpacity = min(minimumOpacity, opacity);
			maximumOpacity = max(maximumOpacity, opacity);
			shadowedSamples += opacity > opacityThreshold ? 1u : 0u;
			++sampleCount;
		}
	}
	if (!neighbourhoodValid || sampleCount == 0u)
		return;

	float meanOpacity = opacitySum / (float)sampleCount;
	float shadowedFraction = (float)shadowedSamples / (float)sampleCount;
	CloudTelemetry[0] = float4(
		saturate(centreOpacity), sunDirection.z, (float)layerCount, 1.0);
	CloudTelemetry[1] = float4(
		meanOpacity, maximumOpacity, shadowedFraction, 1.0);
	CloudTelemetry[2] = float4(
		minimumOpacity, (float)sampleCount, radius, opacityThreshold);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
	uint2 outputSize = (uint2)OutputSizeAndInvSize.xy;
	uint2 outputPixel = dispatchThreadID.xy;
	if (any(outputPixel >= outputSize))
		return;
	// Uniform for the whole dispatch. Normal rendering leaves u2 unbound and
	// avoids a full-resolution diagnostic UAV write; an explicit acceptance or
	// evidence request enables it for exactly the dispatch that is copied.
	const bool writeReceiverValidity = ShadowParams.z > 0.5;
	if (writeReceiverValidity)
		ReceiverValidity[outputPixel] = 0u;
	if (ShadowParams.w > 0.5 && all(outputPixel == uint2(0u, 0u))) {
		// VR uses eye zero's directional-sun constant for the common headset/
		// camera receiver. Directional sunlight should be identical for both eyes.
		float3 diagnosticSunDirection;
		bool diagnosticSunValid = GetNormalizedSunDirection(
			0u, diagnosticSunDirection);
		WritePhysicalCloudTelemetry(
			diagnosticSunDirection, diagnosticSunValid);
	}

	float rawDepth;
	float3 receiver;
	uint eyeIndex;
	if (!ReconstructWorldPosition(outputPixel, rawDepth, receiver, eyeIndex)) {
		OutputTexture[outputPixel] = 1.0;
		return;
	}
	// Native clear/background depth is exactly one. It has no shaded receiver
	// and must remain identity rather than participating in physical evidence.
	if (!IS_FINITE(rawDepth) || rawDepth < 0.0 || rawDepth >= 1.0) {
		OutputTexture[outputPixel] = 1.0;
		return;
	}
	if (writeReceiverValidity)
		ReceiverValidity[outputPixel] = 1u;

	// Keep the DFLight receiver and light vector in the same absolute Ni-world
	// frame as the captured cloud cubemaps. Flat and VR expose that basis through
	// different native constant layouts.
	float3 sunDirection;
	if (!GetNormalizedSunDirection(eyeIndex, sunDirection)) {
		OutputTexture[outputPixel] = 1.0;
		return;
	}
	// A below-horizon direction cannot illuminate this receiver. Never take the
	// far-side sphere root; every production/debug output stays exactly neutral.
	if (sunDirection.z <= 0.0) {
		OutputTexture[outputPixel] = 1.0;
		return;
	}

	float debugMode = ModelParams.w;
	if (debugMode > 0.5 && debugMode < 1.5) {
		// Small, high-contrast world cells remain obvious in a normal first-
		// person view while still proving that reconstruction follows the world.
		float2 cell = floor(receiver.xy / 128.0);
		OutputTexture[outputPixel] =
			abs(fmod(cell.x + cell.y, 2.0)) > 0.5 ? 0.15 : 1.0;
		return;
	}
	if (debugMode > 1.5 && debugMode < 2.5) {
		OutputTexture[outputPixel] = rawDepth;
		return;
	}

	if (debugMode > 3.5 && debugMode < 4.5) {
		// Capture analysis using the central sunlight ray:
		// left = raw captured opacity, middle = broad artistic sheet,
		// right = structured density retained as a shadow caster.
		float rawCoverage;
		float broadCoverage;
		float structuredCoverage;
		bool sampleValid;
		SampleVisibleCloudCoverage(
			receiver, sunDirection, true, rawCoverage, broadCoverage,
			structuredCoverage, sampleValid);
		float panel = 3.0 * float(outputPixel.x) /
			max(float(outputSize.x), 1.0);
		OutputTexture[outputPixel] = panel < 1.0
			? rawCoverage
			: (panel < 2.0
				? broadCoverage
				: structuredCoverage);
		return;
	}

	// One shaded receiver, one ray toward the sun, one opacity lookup at that
	// ray's cloud-shell intersection. This deliberately does not sample a
	// synthetic sun disc: a nearby cloud may not darken this receiver when the
	// actual receiver-to-sun ray crosses blue sky.
	float coverage = SampleVisibleCloudCoverage(receiver, sunDirection);
	// The authenticated capture is cleared to exact zero outside visible cloud
	// geometry. Keep that zero-to-one mapping explicit so blue sky publishes the
	// identity factor bit-for-bit rather than an averaged approximation.
	float transmittance = coverage > 0.0 ? 1.0 - coverage : 1.0;

	if (debugMode > 2.5 && debugMode < 3.5)
		OutputTexture[outputPixel] = transmittance;
	else {
		// Match Skyrim's opacity semantics: values above one amplify captured
		// coverage and clamp the resulting sunlight multiplier, rather than being
		// silently truncated to the old Fallout UI maximum of one.
		float shadowStrength = max(ShadowParams.x, 0.0) *
			saturate(ShadowParams.y);
		OutputTexture[outputPixel] = saturate(
			1.0 - (1.0 - transmittance) * shadowStrength);
	}
}

// Hot production entry. The native producer publishes one source-over-
// composited visible-cloud cube, so normal lighting needs exactly one current
// LOD-zero lookup. Keeping diagnostics in `main` lets the driver discard their
// loops, extra UAV paths, and runtime debug branches from this shader entirely.
float SampleProductionCloudCoverage(
	float3 receiverAbsolute, float3 rayToSun)
{
	float4 geometry = LayerGeometry[0];
	float4 optics = LayerOptics[0];
	if (geometry.w < 0.0 || optics.w <= 0.0)
		return 0.0;

	float3 samplePoint;
	if (!GetCloudShadowSamplePoint(
		receiverAbsolute, rayToSun, geometry.x, geometry.y, GetCloudFieldOrigin(),
			samplePoint))
		return 0.0;

	float directionLengthSquared = dot(samplePoint, samplePoint);
	if (!IS_FINITE(directionLengthSquared) ||
		directionLengthSquared <= 1.0e-8)
		return 0.0;

	float3 sampleDirection = samplePoint * rsqrt(directionLengthSquared);
	float capturedOpacity = SampleCapturedCloudPoint(samplePoint, 0.0);
	return capturedOpacity * saturate(optics.w);
}

[numthreads(8, 8, 1)]
void mainProduction(uint3 dispatchThreadID : SV_DispatchThreadID)
{
	uint2 outputSize = (uint2)OutputSizeAndInvSize.xy;
	uint2 outputPixel = dispatchThreadID.xy;
	if (any(outputPixel >= outputSize))
		return;

	float rawDepth;
	float3 receiver;
	uint eyeIndex;
	if (!ReconstructWorldPosition(
			outputPixel, rawDepth, receiver, eyeIndex) ||
		!IS_FINITE(rawDepth) || rawDepth < 0.0 || rawDepth >= 1.0) {
		OutputTexture[outputPixel] = 1.0;
		return;
	}

	float3 sunDirection;
	if (!GetNormalizedSunDirection(eyeIndex, sunDirection) ||
		sunDirection.z <= 0.0) {
		OutputTexture[outputPixel] = 1.0;
		return;
	}

	// For every valid receiver: trace its one ray toward the live DFLight sun,
	// intersect the configured shell, then sample only the current authenticated
	// visible-cloud opacity. Captured blue sky is exact zero and must publish the
	// identity sunlight factor bit-for-bit.
	float coverage = SampleProductionCloudCoverage(receiver, sunDirection);
	if (coverage <= 0.0) {
		OutputTexture[outputPixel] = 1.0;
		return;
	}

	float shadowStrength = max(ShadowParams.x, 0.0) *
		saturate(ShadowParams.y);
	OutputTexture[outputPixel] = saturate(
		1.0 - coverage * shadowStrength);
}
