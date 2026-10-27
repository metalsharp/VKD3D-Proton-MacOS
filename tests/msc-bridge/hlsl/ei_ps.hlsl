#define RS "RootConstants(num32BitConstants=1, b0)"
struct PSIn { float4 pos : SV_Position; float4 col : COLOR0; };
[RootSignature(RS)]
float4 main(PSIn i) : SV_Target0 { return i.col; }
