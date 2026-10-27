#define RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), RootConstants(num32BitConstants=4, b0)"
cbuffer C : register(b0) { float4 offs; };
struct VSIn { float2 pos : POSITION; float4 color : COLOR0; };
struct VSOut { float4 pos : SV_Position; float4 color : COLOR0; };
[RootSignature(RS)]
VSOut main(VSIn i) { VSOut o; o.pos = float4(i.pos + offs.xy, 0.5, 1.0); o.color = i.color; return o; }
