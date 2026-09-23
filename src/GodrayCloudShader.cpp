// SPDX-License-Identifier: GPL-3.0-only
#include "GodrayCloudShader.h"

namespace FO4CS::GodrayCloudShader
{
    const char* CommonSource() noexcept
    {
        return R"hlsl(
cbuffer NativePass : register(b0) { float4 PassData[20]; };
cbuffer CloudGodray : register(b13) { float4 CloudGeometry; float4 CloudOrigin; float4 ExpectedEye; float4 VisibleSunDirectionAndValidity;
    float4 SunRight; float4 SunUp; float4 SunDirection; float4 SunCenter; };
#if FO4CS_SUN_MASK
Texture2D<float> CloudCube : register(t47);
#else
TextureCube<float> CloudCube : register(t47);
#endif
SamplerState CloudSampler : register(s15);

#define IS_FINITE(x) ((asuint(x) & 0x7F800000u) != 0x7F800000u)
float CloudT(float3 receiver, float3 rayToSun, float3 fieldOrigin)
{
    float height = CloudGeometry.x;
    float radius = CloudGeometry.y;
    float shell = radius + height;
    float3 relative = receiver - fieldOrigin;
    float deficitNumerator = 2.0 * radius * (height - relative.z) +
        height * height - dot(relative, relative);
    float deficit = deficitNumerator / max(shell * shell, 1.0e-8);
    float rayDot = (radius * rayToSun.z + dot(relative, rayToSun)) /
        max(shell, 1.0e-8);
    float discriminant = rayDot * rayDot + deficit;
    float root = sqrt(max(discriminant, 0.0));
    float denominator = max(rayDot + root, 1.0e-7);
    float distanceToShell = shell * deficit / denominator;
    float3 cubeVector = relative + rayToSun * distanceToShell;
    float cubeLengthSquared = dot(cubeVector, cubeVector);
    float3 cubeDirection = cubeVector * rsqrt(max(cubeLengthSquared, 1.0e-8));
#if FO4CS_SUN_MASK
    float3 offset = cubeVector - SunCenter.xyz;
    float2 uv = float2(dot(offset, SunRight.xyz), -dot(offset, SunUp.xyz)) /
        max(2.0 * SunRight.w, 1.0e-7) + 0.5;
    float2 edge = min(uv, 1.0 - uv);
    bool inMap = SunCenter.w > 0.5 && all(IS_FINITE(uv)) && all(uv >= 0) && all(uv <= 1);
    float opacity = inMap ? saturate(CloudCube.SampleLevel(CloudSampler, uv, 0)) *
        saturate(min(edge.x, edge.y) * (512.0 / 8.0)) : 0;
#else
    float opacity = saturate(CloudCube.SampleLevel(CloudSampler, cubeDirection, 0.0));
#endif
    bool valid = CloudGeometry.w > 0.5 && height > 0.0 && radius > 0.0 &&
        CloudOrigin.w > 0.0 && rayToSun.z > 0.0 &&
        deficitNumerator > 0.0 && discriminant >= 0.0 &&
        cubeLengthSquared > 1.0e-8;
    float shadowed = saturate(1.0 - opacity * saturate(CloudOrigin.w) *
        max(CloudGeometry.z, 0.0));
    return valid ? shadowed : 1.0;
}

float3 IntegratedFactor(float3 eye, float3 endpoint)
{
    float3 eyeError = eye - ExpectedEye.xyz;
    bool eyeSpaceAttested = ExpectedEye.w > 0.0 &&
        dot(eyeError, eyeError) <= ExpectedEye.w * ExpectedEye.w;
    float3 delta = endpoint - eye;
    float distance = length(delta);
    // Match the ground mask's visible-sun contract. Native beam geometry
    // remains untouched; only the cloud lookup uses this direction.
    float3 light = VisibleSunDirectionAndValidity.xyz;
    float lightLengthSquared = dot(light, light);
    if (VisibleSunDirectionAndValidity.w <= 0.5 ||
        !all(IS_FINITE(light)) || lightLengthSquared <= 1.0e-8 || light.z <= 0.0)
        return 1.0;
    float3 rayToSun = light * rsqrt(lightLengthSquared);
    float3 sigmaT = max(PassData[18].xyz + PassData[19].xyz, 0.0);
    float3 denominator = 0.0;
    float3 numerator = 0.0;
    float3 extinction;
    extinction = exp2(-sigmaT * (distance * 0.0694318442) * 1.442695);
    denominator += 0.3478548451 * extinction;
    numerator += 0.3478548451 * extinction * CloudT(
        eye + delta * 0.0694318442, rayToSun, CloudOrigin.xyz);
    extinction = exp2(-sigmaT * (distance * 0.3300094782) * 1.442695);
    denominator += 0.6521451549 * extinction;
    numerator += 0.6521451549 * extinction * CloudT(
        eye + delta * 0.3300094782, rayToSun, CloudOrigin.xyz);
    extinction = exp2(-sigmaT * (distance * 0.6699905218) * 1.442695);
    denominator += 0.6521451549 * extinction;
    numerator += 0.6521451549 * extinction * CloudT(
        eye + delta * 0.6699905218, rayToSun, CloudOrigin.xyz);
    extinction = exp2(-sigmaT * (distance * 0.9305681558) * 1.442695);
    denominator += 0.3478548451 * extinction;
    numerator += 0.3478548451 * extinction * CloudT(
        eye + delta * 0.9305681558, rayToSun, CloudOrigin.xyz);
    // Equal regularization is deliberately applied to both terms. If all
    // quadrature samples underflow, or if the captured direction is blue sky
    // (CloudT == 1), numerator and denominator still divide to exactly one
    // instead of spuriously extinguishing the native volume.
    float3 factor = saturate(
        (numerator + 1.0e-8) / (denominator + 1.0e-8));
    return eyeSpaceAttested && distance > 1.0e-7 ? factor : 1.0;
}

)hlsl";
    }

    std::string PayloadSource(bool screenIntegral, bool sunMask)
    {
        const char* entry = screenIntegral
            ? R"hlsl(Texture2DMS<float> SceneDepth : register(t2);
float4 main(float4 nativeIntegral : CLOUD_BASE,
    float4 position : SV_Position,
    uint sampleIndex : SV_SampleIndex) : SV_Target0
{
    float2 ndc = PassData[13].zw * PassData[14].ww * position.xy;
    ndc = ndc * float2(2.0, -2.0) + float2(-1.0, 1.0);
    float depth = SceneDepth.Load(int2(position.xy), sampleIndex);
    float4 endpointH = ndc.y * PassData[9] + ndc.x * PassData[8] +
        depth * PassData[10] + PassData[11];
    float3 endpoint = endpointH.xyz / endpointH.w;
    float3 factor = IntegratedFactor(PassData[14].xyz, endpoint);
    return float4(nativeIntegral.xyz * factor, nativeIntegral.w);
}
)hlsl"
            : R"hlsl(float4 main(float3 endpoint : TEXCOORD0,
    float4 nativeIntegral : CLOUD_BASE) : SV_Target0
{
    float3 factor = IntegratedFactor(PassData[14].xyz, endpoint);
    return float4(nativeIntegral.xyz * factor, nativeIntegral.w);
}
)hlsl";
        return std::string(sunMask ? "#define FO4CS_SUN_MASK 1\n" : "#define FO4CS_SUN_MASK 0\n") + CommonSource() + entry;
    }
}
