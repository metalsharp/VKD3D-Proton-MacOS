// Test 1: RootConstants(b0, 2 values), root UAV u0, descriptor table with one UAV u1.
#define RS "RootConstants(num32BitConstants=2, b0), UAV(u0), DescriptorTable(UAV(u1))"
cbuffer Consts : register(b0) { uint a; uint b; };
RWStructuredBuffer<uint> out0 : register(u0);
RWStructuredBuffer<uint> out1 : register(u1);

[RootSignature(RS)]
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint i = tid.x;
    out0[i] = a * i + b;
    out1[i] = (a ^ i) + b * 3u;
}
