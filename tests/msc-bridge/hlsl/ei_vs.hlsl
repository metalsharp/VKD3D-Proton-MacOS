#define RS "RootConstants(num32BitConstants=1, b0)"
cbuffer C : register(b0) { uint v; };
struct VSOut { float4 pos : SV_Position; float4 col : COLOR0; };
[RootSignature(RS)]
VSOut main(uint vid : SV_VertexID)
{
    VSOut o;
    o.col = float4((v + 1) / 255.0, 0, 0, 1);
    o.pos = float4(((float)v + 0.5) / 3.0 - 1.0, 0.0, 0.5, 1.0);  // pixel v of a 6x1 target
    return o;
}
