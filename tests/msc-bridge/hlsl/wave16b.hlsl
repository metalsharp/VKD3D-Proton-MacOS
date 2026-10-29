// 16-bit values through groupshared memory, wave operations across several waves per group, 16-bit buffer
// loads/stores and a 16-bit float typed UAV (cs_6_2, -enable-16bit-types); wave16test.c checks each against the CPU.
RWByteAddressBuffer output : register(u0);
RWByteAddressBuffer raw : register(u1);
RWTexture2D<float4> tex : register(u2);

groupshared half gs_h[128];
groupshared half2 gs_h2[128];
groupshared float gs_wave[4];

#define SLOTS 12
void put(uint t, uint slot, float v) { output.Store((t * SLOTS + slot) * 4, asuint(v)); }
void puti(uint t, uint slot, int v) { output.Store((t * SLOTS + slot) * 4, asuint(v)); }

[numthreads(128, 1, 1)]
void main(uint t : SV_GroupIndex)
{
    half h = (half)(t & 15) * 0.25h;
    gs_h[t] = h;
    gs_h2[t] = half2(h, -h);
    GroupMemoryBarrierWithGroupSync();
    put(t, 0, gs_h[127 - t]);
    put(t, 1, gs_h2[(t + 33) & 127].y);
    half ws = WaveActiveSum(h);
    if (WaveIsFirstLane())
        gs_wave[t / WaveGetLaneCount()] = ws;
    GroupMemoryBarrierWithGroupSync();
    put(t, 2, gs_wave[0] + gs_wave[1] + gs_wave[2] + gs_wave[3]);
    puti(t, 3, WaveGetLaneIndex());
    // 16-bit raw buffer access at 2-byte granularity
    half2 r2 = raw.Load<half2>(t * 4);
    put(t, 4, r2.x + r2.y);
    half one = raw.Load<half>(t * 2 + 2);
    put(t, 5, one);
    raw.Store<half>(1024 + t * 2, h * 2.0h);
    AllMemoryBarrier();
    put(t, 6, raw.Load<half>(1024 + ((t ^ 1) * 2)));
    // 16-bit float typed UAV (R16G16B16A16_FLOAT)
    float4 tv = tex[uint2(t & 15, t >> 4)];
    put(t, 7, tv.x + tv.y * 2.0 + tv.z * 4.0);
    half4 hv = (half4)tv;
    put(t, 8, (float)(hv.x * hv.y));
    put(t, 9, f16tof32(f32tof16((float)t * 0.25f)));   // exact: f32tof16 rounds differently on the two paths
    half mn = min(h, (half)WaveReadLaneAt(h, 3));
    put(t, 10, mn);
    put(t, 11, WaveActiveMax(gs_h[(t * 7) & 127]));
}
