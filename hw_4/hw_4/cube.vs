cbuffer SceneBuffer : register(b0)
{
    float4x4 vp;
    float4 cameraPos;
};

cbuffer GeomBuffer : register(b1)
{
    float4x4 model;
    float4 size;
};

struct VSInput
{
    float3 pos : POSITION;
    float2 uv  : TEXCOORD;
};

struct VSOutput
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD;
};

VSOutput vs(VSInput input)
{
    VSOutput result;
    result.pos = mul(vp, mul(model, float4(input.pos, 1.0f)));
    result.uv = input.uv;
    return result;
}