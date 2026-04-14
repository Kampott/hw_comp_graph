cbuffer ModelBuffer : register(b0)
{
    row_major float4x4 m;
};

cbuffer SceneBuffer : register(b1)
{
    row_major float4x4 vp;
};

struct VSInput
{
    float3 pos : POSITION;
    float4 color : COLOR;
};

struct VSOutput
{
    float4 pos : SV_Position;
    float4 color : COLOR;
};

VSOutput vs(VSInput input)
{
    VSOutput output;
    float4 p = float4(input.pos, 1.0f);
    output.pos = mul(mul(p, m), vp);
    output.color = input.color;
    return output;
}