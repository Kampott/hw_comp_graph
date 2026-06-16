struct VSInput
{
    uint vertexId : SV_VertexID;
};

struct VSOutput
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD;
};

VSOutput vs(VSInput vertex)
{
    VSOutput result;
    float2 pos = float2(0, 0);

    if (vertex.vertexId == 0) pos = float2(-1, -3);
    else if (vertex.vertexId == 1) pos = float2(-1,  1);
    else                           pos = float2( 3,  1);

    result.pos = float4(pos, 0, 1);
    result.uv  = float2(pos.x * 0.5 + 0.5, 0.5 - pos.y * 0.5);
    return result;
}