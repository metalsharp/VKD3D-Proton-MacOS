// Test 2: adds a typed UAV (texel buffer view), a texture SRV table and a sampler table.
#define RS "RootConstants(num32BitConstants=2, b0), UAV(u0), DescriptorTable(UAV(u1, numDescriptors=2)), DescriptorTable(SRV(t0)), DescriptorTable(Sampler(s0))"
cbuffer Consts : register(b0) { uint a; uint b; };
RWStructuredBuffer<uint> out0 : register(u0);
RWStructuredBuffer<uint> out1 : register(u1);
RWBuffer<uint> typed : register(u2);
Texture2D<float4> tex : register(t0);
SamplerState smp : register(s0);

[RootSignature(RS)]
[numthreads(8, 4, 1)]
void main(uint3 tid : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    uint i = tid.y * 8u + tid.x;
    out0[i] = a * i + b;
    out1[i] = (a ^ i) + b * 3u;
    typed[i] = a + i * 7u;
    if (i == 0u) {
        float4 c = tex.SampleLevel(smp, float2(0.5, 0.5), 0.0);
        out0[64] = (uint)round(c.x * 255.0);
        out0[65] = (uint)round(c.y * 255.0);
    }
}
