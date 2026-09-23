// Observe the actual production lookup at fixed absolute terrain locations.
// Only the test's camera constants change between dispatches.
#include "../shaders/CloudShadows/FO4CloudShadowScreenCS.hlsl"

[numthreads(8, 8, 1)]
void probe(uint3 tid : SV_DispatchThreadID)
{
    float3 fixedReceiver = DiagnosticReceiverAndRadius.xyz +
        float3((float2(tid.xy) - 3.5) * 4000.0, 0.0);
    float3 sun;
    OutputTexture[tid.xy] = GetNormalizedSunDirection(0, sun) ?
        SampleProductionCloudCoverage(fixedReceiver, sun) : 0.0;
}
