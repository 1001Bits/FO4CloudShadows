// Test-only observer of the production reconstruction functions.
#include "../shaders/CloudShadows/FO4CloudShadowScreenCS.hlsl"
RWTexture2D<float> ProjectionProbe : register(u3);

[numthreads(8, 8, 1)]
void probe(uint3 tid : SV_DispatchThreadID)
{
    float depth;
    float3 receiver;
    uint eye;
    bool valid = ReconstructWorldPosition(tid.xy, depth, receiver, eye);
    ProjectionProbe[uint2(tid.x * 4, tid.y)] = receiver.x;
    ProjectionProbe[uint2(tid.x * 4 + 1, tid.y)] = receiver.y;
    ProjectionProbe[uint2(tid.x * 4 + 2, tid.y)] = receiver.z;
    ProjectionProbe[uint2(tid.x * 4 + 3, tid.y)] = valid ? depth : -1.0;
    if (all(tid.xy == uint2(0, 0))) {
        float3 sun;
        bool sunValid = GetNormalizedSunDirection(0, sun);
        ProjectionProbe[uint2(0, 8)] = sun.x;
        ProjectionProbe[uint2(1, 8)] = sun.y;
        ProjectionProbe[uint2(2, 8)] = sun.z;
        ProjectionProbe[uint2(3, 8)] = sunValid ? 1.0 : 0.0;
    }
}
