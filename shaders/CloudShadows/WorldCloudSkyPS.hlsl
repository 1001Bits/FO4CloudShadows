// SPDX-License-Identifier: GPL-3.0-or-later
// BSSky ABI/formulas adapted from Fallout 4 Community Shaders at
// d1ccd465cb38b81baad90973953be1a95e025644; see THIRD_PARTY_NOTICES.md.
// Legacy diagnostic visible-sky replacement. Production never binds this
// shader: Fallout draws the visible sky, while its native cubemap pass writes
// the separate R8 opacity cube consumed by FO4CloudShadowScreenCS.

TextureCube<float> WorldCloudTiles : register(t5);
SamplerState WorldCloudSampler : register(s2);

// Portable finite test. Proton/Wine's vkd3d-based d3dcompiler lacks the
// isfinite intrinsic, and fxc may fold NaN comparisons; the exponent bit test
// is exact IEEE-754 on both compilers. A macro, not overloads: vkd3d cannot
// prioritize between compatible overloads (E5017).
#define IS_FINITE(x) ((asuint(x) & 0x7F800000u) != 0x7F800000u)
cbuffer WorldCloudSkyCB : register(b0)
{
	// x shell height, y planet radius, z non-negative when valid, w active blend.
	float4 ShellGeometry : packoffset(c0);
	float4 CaptureOrigin : packoffset(c1);
	float4 CameraOrigin : packoffset(c2);
	float4 PreviousCameraOrigin : packoffset(c3);
};

cbuffer SkyPerFrame : register(b12)
{
	#if FO4CS_SHADER_VR
	float4 PerFrame[71] : packoffset(c0);
	#else
	float4 PerFrame[41] : packoffset(c0);
	#endif
};

struct PSIn
{
	float4 position : SV_Position;
	float3 worldDirection : TEXCOORD0;
	float3 rayOrigin : TEXCOORD1;
	#if FO4CS_SHADER_VR
	nointerpolation uint eyeIndex : POSITION2;
	#endif
	float4 currentPosition : POSITION0;
	float4 previousPosition : POSITION1;
};

struct PSOut
{
	float4 color : SV_Target0;
	float4 motion : SV_Target1;
};

float4 NativeMotionVector(
	float4 currentPosition, float4 previousPosition
	#if FO4CS_SHADER_VR
	, uint eyeIndex
	#endif
)
{
	#if FO4CS_SHADER_VR
	const uint eyeOffset = eyeIndex != 0u ? 4u : 0u;
	float2 current = float2(
		dot(PerFrame[63u + eyeOffset], currentPosition),
		dot(PerFrame[64u + eyeOffset], currentPosition));
	current /= dot(PerFrame[66u + eyeOffset], currentPosition);

	float2 previous = float2(
		dot(PerFrame[51u + eyeOffset], previousPosition),
		dot(PerFrame[52u + eyeOffset], previousPosition));
	previous /= dot(PerFrame[54u + eyeOffset], previousPosition);
	#else
	float2 current = float2(
		dot(PerFrame[37], currentPosition),
		dot(PerFrame[38], currentPosition));
	current /= dot(PerFrame[40], currentPosition);

	float2 previous = float2(
		dot(PerFrame[31], previousPosition),
		dot(PerFrame[32], previousPosition));
	previous /= dot(PerFrame[34], previousPosition);
	#endif
	return float4((current - previous) * float2(-0.5, 0.5), 1.0, 1.0);
}

float4 ShellMotionVector(
	float3 absoluteWorldPosition
	#if FO4CS_SHADER_VR
	, uint eyeIndex
	#endif
)
{
	// Engine object transforms are camera-relative. Convert the absolute
	// cloud-shell point into the corresponding current/previous rebased spaces
	// before applying the exact native Sky motion matrices.
	float4 currentPosition = float4(
		absoluteWorldPosition - CameraOrigin.xyz, 1.0);
	float4 previousPosition = float4(
		absoluteWorldPosition - PreviousCameraOrigin.xyz, 1.0);
	return NativeMotionVector(
		currentPosition, previousPosition
		#if FO4CS_SHADER_VR
		, eyeIndex
		#endif
	);
}

bool GetCloudShellSampleDirection(
	float3 receiverAbsolute, float3 rayDirection,
	out float3 sampleDirection, out float3 shellWorld)
{
	sampleDirection = 0.0;
	shellWorld = 0.0;
	float cloudHeight = ShellGeometry.x;
	float planetRadius = ShellGeometry.y;
	if (!all(IS_FINITE(receiverAbsolute)) ||
		!all(IS_FINITE(rayDirection)) ||
		!all(IS_FINITE(CaptureOrigin.xyz)) ||
		!IS_FINITE(cloudHeight) || !IS_FINITE(planetRadius) ||
		cloudHeight <= 0.0 || planetRadius <= 0.0)
		return false;

	float directionLengthSquared = dot(rayDirection, rayDirection);
	if (!IS_FINITE(directionLengthSquared) ||
		directionLengthSquared <= 1.0e-8)
		return false;
	rayDirection *= rsqrt(directionLengthSquared);

	float shellRadius = planetRadius + cloudHeight;
	float3 relativeReceiver = receiverAbsolute - CaptureOrigin.xyz;
	float deficitNumerator =
		2.0 * planetRadius * (cloudHeight - relativeReceiver.z) +
		cloudHeight * cloudHeight - dot(relativeReceiver, relativeReceiver);
	if (!IS_FINITE(shellRadius) || shellRadius <= planetRadius ||
		!IS_FINITE(deficitNumerator) || deficitNumerator <= 0.0)
		return false;

	float radialDeficit = deficitNumerator / (shellRadius * shellRadius);
	float rayDot =
		(planetRadius * rayDirection.z +
			dot(relativeReceiver, rayDirection)) / shellRadius;
	float discriminant = rayDot * rayDot + radialDeficit;
	if (!IS_FINITE(radialDeficit) || !IS_FINITE(rayDot) ||
		!IS_FINITE(discriminant) || discriminant < 0.0)
		return false;

	// The native cloud dome, not an artificial camera-horizontal plane,
	// determines the visible extent. Downward-facing dome vertices therefore
	// use the same nearest positive shell intersection instead of becoming a
	// transparent hard edge. Terrain and other scene geometry still occlude the
	// sky normally. The ground-shadow shader independently keeps its strict
	// above-horizon sunlight gate.
	float root = sqrt(max(discriminant, 0.0));
	float stableDenominator = rayDot + root;
	if (!IS_FINITE(stableDenominator) || stableDenominator <= 1.0e-7)
		return false;
	float distanceToShell =
		shellRadius * radialDeficit / stableDenominator;
	if (!IS_FINITE(distanceToShell) || distanceToShell <= 0.0)
		return false;

	sampleDirection = relativeReceiver + rayDirection * distanceToShell;
	shellWorld = receiverAbsolute + rayDirection * distanceToShell;
	return all(IS_FINITE(sampleDirection)) && all(IS_FINITE(shellWorld)) &&
		dot(sampleDirection, sampleDirection) > 1.0e-8;
}

PSOut main(PSIn input)
{
	float coverage = 0.0;
	float3 sampleDirection = 0.0;
	float3 shellWorld = 0.0;
	bool shellHit = ShellGeometry.z >= 0.0 &&
		GetCloudShellSampleDirection(
			input.rayOrigin, input.worldDirection,
			sampleDirection, shellWorld);
	if (shellHit) {
		coverage = WorldCloudTiles.SampleLevel(
			WorldCloudSampler,
			sampleDirection, 0.0);
		coverage *= saturate(ShellGeometry.w);
	}

	PSOut output;
	output.color = coverage.xxxx;
	output.motion = shellHit
		? ShellMotionVector(
			shellWorld
			#if FO4CS_SHADER_VR
			, input.eyeIndex
			#endif
		)
		: NativeMotionVector(
			input.currentPosition, input.previousPosition
			#if FO4CS_SHADER_VR
			, input.eyeIndex
			#endif
		);
	return output;
}
