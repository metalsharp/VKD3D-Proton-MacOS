// Test 6: a typed SRV whose first element is not at the start of its texel view.
#define RS "DescriptorTable(SRV(t0)), UAV(u1)"
Buffer<uint> src : register(t0);
RWStructuredBuffer<uint> res : register(u1);
[RootSignature(RS)]
[numthreads(4, 1, 1)]
void main(uint tid : SV_DispatchThreadID)
{
    res[1 + tid] = src[tid];
    uint d; src.GetDimensions(d);
    if (tid == 0) res[0] = d;
}
