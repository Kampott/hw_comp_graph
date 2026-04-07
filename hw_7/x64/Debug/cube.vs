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
    float4 params; // x - shininess, y - useNormalMap
};

struct VSInput
{
    float3 pos  : POSITION;
    float3 norm : NORMAL;
    float3 tang : TANGENT;
    float2 uv   : TEXCOORD;
};

struct VSOutput
{
    float4 pos      : SV_Position;
    float4 worldPos : POSITION1;
    float3 norm     : NORMAL;
    float3 tang     : TANGENT;
    float2 uv       : TEXCOORD;
    float4 color    : COLOR0;
};

VSOutput vs(VSInput input)
{
    VSOutput result;
    float4 worldPos = mul(float4(input.pos, 1.0f), model);
    result.pos      = mul(worldPos, vp);
    result.worldPos = worldPos;
    result.norm     = mul(input.norm, (float3x3)normalMatrix);
    result.tang     = mul(input.tang, (float3x3)normalMatrix);
    result.uv       = input.uv;
    result.color    = color;
    return result;
}