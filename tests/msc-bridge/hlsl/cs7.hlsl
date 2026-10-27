// Test 7: non-uniform descriptor indexing, one descriptor per lane.
#define RS "DescriptorTable(SRV(t0, numDescriptors=64)), UAV(u1)"
StructuredBuffer<uint> bufs[64] : register(t0);
RWStructuredBuffer<uint> res : register(u1);
[RootSignature(RS)]
[numthreads(64, 1, 1)]
void main(uint tid : SV_DispatchThreadID)
{
    res[tid] = bufs[NonUniformResourceIndex(tid)][0];
}
