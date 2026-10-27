// Test 3: SM 6.6 heap indexing. The root signature has only root constants; every resource and the sampler come
// from ResourceDescriptorHeap / SamplerDescriptorHeap.
#define RS "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED), RootConstants(num32BitConstants=4, b0)"
cbuffer Consts : register(b0) { uint outIndex; uint texIndex; uint samplerIndex; uint mul; };

[RootSignature(RS)]
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    RWStructuredBuffer<uint> o = ResourceDescriptorHeap[outIndex];
    o[tid.x] = tid.x * mul + 5u;
    if (tid.x == 0u) {
        Texture2D<float4> t = ResourceDescriptorHeap[texIndex];
        SamplerState s = SamplerDescriptorHeap[samplerIndex];
        float4 c = t.SampleLevel(s, float2(0.5, 0.5), 0.0);
        o[64] = (uint)round(c.x * 255.0);
        o[65] = (uint)round(c.y * 255.0);
    }
}
