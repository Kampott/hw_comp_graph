#include "lighting.hlsli"

cbuffer SceneBuffer : register(b0)
{
    row_major float4x4 vp;
    float4 cameraPos;
    int4 lightCount;
    Light lights[10];
    float4 ambientColor;
};

cbuffer GeomBuffer : register(b1)
{
    row_major float4x4 model;
    row_major float4x4 normalMatrix;
    float4 color;
    float4 params;
};

struct VSInput
{
    float3 pos : POSITION;
};

struct VSOutput
{
    float4 pos      : SV_Position;
    float3 localPos : POSITION1;
};

VSOutput vs(VSInput vertex)
{
    VSOutput result;

    float4 worldPos = mul(float4(vertex.pos * params.x, 1.0f), model);
    float4 clipPos = mul(worldPos, vp);

    result.pos = float4(clipPos.xy, 0.0f, clipPos.w);
    result.localPos = vertex.pos;

    return result;
}
