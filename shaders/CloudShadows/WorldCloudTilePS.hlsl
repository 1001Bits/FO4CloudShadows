// SPDX-License-Identifier: GPL-3.0-or-later
// Legacy build-manifest asset. The runtime no longer compiles or binds this
// procedural tile shader; native cubemap MRT capture is the only producer.

Texture2D<float4> Texture0 : register(t0);
Texture2D<float4> Texture1 : register(t1);
SamplerState Sampler0 : register(s0);
SamplerState Sampler1 : register(s1);

cbuffer WorldCloudTileCaptureCB : register(b0)
{
	float4 CaptureCB_pad_0_2[3] : packoffset(c0);
	// x = technique (5/6/7), y = calibration, z/w = suppress t0/t1.
	float4 CaptureParams : packoffset(c3);
};

cbuffer SkyPSPerGeometry : register(b2)
{
	// x is texture lerp/fade; y is native cloud brightness (not opacity).
	float2 PParams : packoffset(c0);
};

struct PSIn
{
	float4 position : SV_Position;
	float2 texcoord0 : TEXCOORD0;
	float2 texcoord1 : TEXCOORD1;
	nointerpolation float layerAlpha : TEXCOORD2;
};

float4 main(PSIn input) : SV_Target0
{
	float4 cloud = Texture0.SampleLevel(Sampler0, input.texcoord0, 0.0);
	if (CaptureParams.z > 0.5)
		cloud = 0.0;

	float transitionFade = 1.0;
	if (CaptureParams.x > 5.5 && CaptureParams.x < 6.5) {
		float4 nextCloud = Texture1.SampleLevel(
			Sampler1, input.texcoord1, 0.0);
		if (CaptureParams.w > 0.5)
			nextCloud = 0.0;
		cloud = lerp(cloud, nextCloud, PParams.x);
	} else if (CaptureParams.x > 6.5) {
		transitionFade = saturate(PParams.x - 0.4) * (5.0 / 3.0);
	}

	float coverage = saturate(
		cloud.a * input.layerAlpha * transitionFade * CaptureParams.y);
	return coverage.xxxx;
}
