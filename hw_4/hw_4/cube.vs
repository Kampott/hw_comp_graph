cbuffer SceneBuffer : register(b0)
{
    row_major float4x4 vp;
    row_major float4x4 vpSky;
    float4 cameraPos;
};

cbuffer GeomBuffer : register(b1)
{
    row_major float4x4 model;
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
    float4 p = float4(input.pos, 1.0f);
    result.pos = mul(mul(p, model), vp);
    result.uv = input.uv;
    return result;
}