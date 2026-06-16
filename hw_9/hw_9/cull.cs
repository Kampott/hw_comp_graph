#include "lighting.hlsli"

cbuffer SceneBuffer : register(b0)
{
    row_major float4x4 vp;
    float4 cameraPos;
    int4 lightCount;
    Light lights[10];
    float4 ambientColor;
    float4 frustum[6];
};

cbuffer CullParams : register(b1)
{
    uint4 numShapes;      
    float4 bbMin[100];
    float4 bbMax[100];
};

RWStructuredBuffer<uint> indirectArgs : register(u0);
RWStructuredBuffer<uint4> objectIds    : register(u1);

bool IsBoxInside(in float4 frustumPlanes[6], in float3 bMin, in float3 bMax)
{
    for (int i = 0; i < 6; i++)
    {
        float3 norm = frustumPlanes[i].xyz;
        float4 p = float4(
            norm.x < 0 ? bMin.x : bMax.x,
            norm.y < 0 ? bMin.y : bMax.y,
            norm.z < 0 ? bMin.z : bMax.z,
            1.0
        );
        float s = dot(p, frustumPlanes[i]);
        if (s < 0.0f)
            return false;
    }
    return true;
}

[numthreads(64, 1, 1)]
void cs(uint3 globalThreadId : SV_DispatchThreadID)
{
    if (globalThreadId.x >= numShapes.x)
        return;

float4 frustumPlanes[6];
for (int i = 0; i < 6; i++)
    frustumPlanes[i] = frustum[i];

if (IsBoxInside(frustumPlanes,
                bbMin[globalThreadId.x].xyz,
                bbMax[globalThreadId.x].xyz))
{
    uint id = 0;
    InterlockedAdd(indirectArgs[1], 1, id);
    objectIds[id] = uint4(globalThreadId.x, 0, 0, 0);
}
}