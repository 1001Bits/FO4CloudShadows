// SPDX-License-Identifier: GPL-3.0-or-later
// Legacy build-manifest asset. The runtime no longer compiles or binds this
// procedural tile shader; native cubemap MRT capture is the only producer.

#ifndef FO4CS_SHADER_VR
#define FO4CS_SHADER_VR 0
#endif

cbuffer SkyPerGeometry : register(b2)
{
#if !FO4CS_SHADER_VR
	float4 SkyPerGeometry_pad_0_6[7] : packoffset(c0);
	float4 BlendColor0 : packoffset(c7);
	float4 SkyPerGeometry_pad_8_9[2] : packoffset(c8);
	float2 TexCoordOff : packoffset(c10);
#else
	float4 SkyPerGeometry_pad_0_10[11] : packoffset(c0);
	float4 BlendColor0 : packoffset(c11);
	float4 SkyPerGeometry_pad_12_13[2] : packoffset(c12);
	float2 TexCoordOff : packoffset(c14);
#endif
};

struct VSOut
{
	float4 position : SV_Position;
	float2 texcoord0 : TEXCOORD0;
	float2 texcoord1 : TEXCOORD1;
	nointerpolation float layerAlpha : TEXCOORD2;
};

VSOut main(uint vertexID : SV_VertexID)
{
	// UVs (0,0), (2,0), (0,2) form one fullscreen triangle. Clipping leaves
	// exactly one [0,1] periodic tile, carrying Fallout's live animation offset.
	float2 tileUV = float2((vertexID << 1) & 2, vertexID & 2);

	VSOut output;
	output.position = float4(
		tileUV.x * 2.0 - 1.0,
		1.0 - tileUV.y * 2.0,
		0.5,
		1.0);
	output.texcoord0 = tileUV + TexCoordOff;
	output.texcoord1 = tileUV + TexCoordOff;
	output.layerAlpha = BlendColor0.w;
	return output;
}
