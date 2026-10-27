#define RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), RootConstants(num32BitConstants=4, b0)"
cbuffer C : register(b0) { float4 offs; };
struct PSIn { float4 pos : SV_Position; float4 color : COLOR0; };
[RootSignature(RS)]
float4 main(PSIn i) : SV_Target0 { return i.color * offs.w; }
