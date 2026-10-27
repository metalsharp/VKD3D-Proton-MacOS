#define RS "RootConstants(num32BitConstants=1, b0), UAV(u0)"
cbuffer C : register(b0) { uint v; };
RWStructuredBuffer<uint> o : register(u0);
[RootSignature(RS)]
[numthreads(1, 1, 1)]
void main() { o[v] = v * 10 + 1; }
