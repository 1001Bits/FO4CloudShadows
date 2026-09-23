// SPDX-License-Identifier: GPL-3.0-or-later
// BSSky ABI/formulas adapted from Fallout 4 Community Shaders at
// d1ccd465cb38b81baad90973953be1a95e025644; see THIRD_PARTY_NOTICES.md.
// Visible-cloud vertex replacement. Position and motion inputs reproduce the
// native BSSky cloud VS. The pixel shader maps the camera-centred dome ray onto
// the same absolute spherical shell and committed cubemap sampled by shadows.

#ifndef FO4CS_SHADER_VR
#define FO4CS_SHADER_VR 0
#endif

cbuffer WorldCloudSkyCB : register(b0)
{
	// x shell height, y planet radius, z TextureCubeArray index, w active blend.
	float4 ShellGeometry : packoffset(c0);
	float4 CaptureOrigin : packoffset(c1);
	// Authoritative absolute current/previous camera origins (posAdjust).
	float4 CameraOrigin : packoffset(c2);
	float4 PreviousCameraOrigin : packoffset(c3);
};

cbuffer SkyPerGeometry : register(b2)
{
#if !FO4CS_SHADER_VR
	row_major float4x4 WorldViewProj : packoffset(c0);
	row_major float3x4 World : packoffset(c4);
	float4 BlendColor[3] : packoffset(c7);
	float2 TexCoordOff : packoffset(c10);
	row_major float3x4 PreviousWorld : packoffset(c11);
#else
	row_major float4x4 WorldViewProjEye0 : packoffset(c0);
	row_major float4x4 WorldViewProjEye1 : packoffset(c4);
	row_major float3x4 World : packoffset(c8);
	float4 BlendColor[3] : packoffset(c11);
	float2 TexCoordOff : packoffset(c14);
	row_major float3x4 PreviousWorld : packoffset(c15);
#endif
};

#if FO4CS_SHADER_VR
// Native Fallout 4 VR Sky stereo parameters.  Keep this at the engine's
// original b8 binding so the replacement consumes the exact draw-local value.
cbuffer SkyStereoParams : register(b8)
{
	float4 StereoSeparation : packoffset(c0);
};
#endif

struct VSIn
{
	float4 position : POSITION0;
	float2 texcoord : TEXCOORD0;
	float4 color : COLOR0;
#if FO4CS_SHADER_VR
	uint eyeIndex : SV_InstanceID;
#endif
};

struct VSOut
{
	float4 position : SV_Position;
	float3 worldDirection : TEXCOORD0;
	float3 rayOrigin : TEXCOORD1;
	#if FO4CS_SHADER_VR
	nointerpolation uint eyeIndex : POSITION2;
	float cullDistance : SV_CullDistance0;
	float clipDistance : SV_ClipDistance0;
	#endif
	float4 currentPosition : POSITION0;
	float4 previousPosition : POSITION1;
};

#if FO4CS_SHADER_VR
float4 TransformWorldViewProj(float4 localPosition, uint eyeIndex)
{
	return eyeIndex != 0u ?
		mul(WorldViewProjEye1, localPosition) :
		mul(WorldViewProjEye0, localPosition);
}
#endif

VSOut main(VSIn input)
{
	float4 localPosition = float4(input.position.xyz, 1.0);
#if FO4CS_SHADER_VR
	const uint eyeIndex = input.eyeIndex & 1u;
	float4 viewPosition = TransformWorldViewProj(localPosition, eyeIndex);
	// Exact stock VR Sky stereo packing and per-eye half-plane clipping.
	const float stereo = StereoSeparation.x;
	const float eyeSign = eyeIndex != 0u ? 1.0 : -1.0;
	const float stereoDistance = eyeSign * viewPosition.x + viewPosition.w;
	const float clipX = viewPosition.x;
	viewPosition.x = 0.5 * (2.0 - stereo) * clipX +
		(eyeIndex != 0u ? 0.5 : -0.5) * stereo * viewPosition.w;
#else
	float4 viewPosition = mul(WorldViewProj, localPosition);
#endif
	float3 worldPosition = mul(World, localPosition);
	float3 centre = float3(World[0][3], World[1][3], World[2][3]);

	VSOut output;
	// Native sky depth convention: force the dome onto the far plane.
	output.position.xy = viewPosition.xy;
	output.position.zw = viewPosition.ww;
	output.worldDirection = worldPosition - centre;
	output.rayOrigin = CameraOrigin.xyz;
	#if FO4CS_SHADER_VR
	output.eyeIndex = eyeIndex;
	output.cullDistance = stereo > 0.0 ? stereoDistance : 0.0;
	output.clipDistance = stereo > 0.0 ? stereoDistance : 1.0;
	#endif

	output.currentPosition = float4(worldPosition, 1.0);
	output.previousPosition = float4(mul(PreviousWorld, localPosition), 1.0);
	return output;
}
