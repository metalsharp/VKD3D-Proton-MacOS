// Test 8: non-uniform sampler indexing, one sampler per lane (even wrap, odd clamp).
#define RS "DescriptorTable(SRV(t0)), DescriptorTable(Sampler(s0, numDescriptors=64)), UAV(u1)"
Texture2D<float4> tex : register(t0);
SamplerState samps[64] : register(s0);
RWStructuredBuffer<uint> res : register(u1);
[RootSignature(RS)]
[numthreads(64, 1, 1)]
void main(uint tid : SV_DispatchThreadID)
{
    res[tid] = (uint)round(tex.SampleLevel(samps[NonUniformResourceIndex(tid)], float2(1.25, 0.5), 0.0).x * 255.0);
}
