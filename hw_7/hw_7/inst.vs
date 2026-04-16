#include "lighting.hlsli"

cbuffer SceneBuffer : register(b0)
{
    row_major float4x4 vp;
    float4 cameraPos;
    int4 lightCount;
    Light lights[10];
    float4 ambientColor;
};

struct GeomBuffer
{
    row_major float4x4 model;
    row_major float4x4 norm;
    float4 shineSpeedTexIdNM; // x - shininess, y - rotation speed, z - texture id, w - normal map presence
    float4 posAngle;          // xyz - position, w - current angle
};

cbuffer GeomBufferInst : register(b1)
{
    GeomBuffer geomBuffer[100];
};

cbuffer GeomBufferInstVis : register(b2)
{
    uint4 ids[100];
};

struct VSInput
{
    float3 pos    : POSITION;
    float3 norm   : NORMAL;
    float3 tang   : TANGENT;
    float2 uv     : TEXCOORD;
    uint instId   : SV_InstanceID;
};

struct VSOutput
{
    float4 pos      : SV_Position;
    float4 worldPos : POSITION1;
    float3 norm     : NORMAL;
    float3 tang     : TANGENT;
    float2 uv       : TEXCOORD;
    nointerpolation uint instId : INST_ID;
};

VSOutput vs(VSInput vertex)
{
    VSOutput result;

    uint idx = ids[vertex.instId].x;

    float4 worldPos  = mul(float4(vertex.pos, 1.0f), geomBuffer[idx].model);
    result.pos       = mul(worldPos, vp);
    result.worldPos  = worldPos;
    result.norm      = mul(vertex.norm, (float3x3)geomBuffer[idx].norm);
    result.tang      = mul(vertex.tang, (float3x3)geomBuffer[idx].norm);
    result.uv        = vertex.uv;
    result.instId    = vertex.instId;
    return result;
}
