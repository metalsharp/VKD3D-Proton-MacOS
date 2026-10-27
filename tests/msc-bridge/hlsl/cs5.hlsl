// Test 5: a typed UAV whose first element is not at the start of its texel view.
#define RS "DescriptorTable(UAV(u0)), UAV(u1)"
RWBuffer<uint> typed : register(u0);
RWStructuredBuffer<uint> res : register(u1);
[RootSignature(RS)]
[numthreads(4, 1, 1)]
void main(uint tid : SV_DispatchThreadID)
{
    typed[tid] = 100 + tid;
    uint d; typed.GetDimensions(d);
    if (tid == 0) res[0] = d;
}
