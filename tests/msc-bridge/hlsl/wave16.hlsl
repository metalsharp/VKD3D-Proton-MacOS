// Wave operations on native 16-bit values (cs_6_2, -enable-16bit-types): wave16test.c checks each against the CPU.
RWByteAddressBuffer output : register(u0);

#define SLOTS 24

void put(uint lane, uint slot, float v) { output.Store((lane * SLOTS + slot) * 4, asuint(v)); }
void puti(uint lane, uint slot, int v) { output.Store((lane * SLOTS + slot) * 4, asuint(v)); }

[numthreads(32, 1, 1)]
void main(uint tid : SV_GroupIndex)
{
    uint l = WaveGetLaneIndex();
    half h = (half)(l & 15) * 0.25h;
    int16_t s = (int16_t)((int)l * 3 - 40);
    uint16_t u = (uint16_t)(l * 1000 + 7);
    half4 v = half4(h, h * 2.0h, -h, 1.0h);

    puti(l, 0, WaveGetLaneCount());
    put(l, 1, WaveActiveSum(h));
    put(l, 2, WaveActiveMax(h));
    put(l, 3, WavePrefixSum(h));
    put(l, 4, WaveReadLaneAt(h, 5));
    put(l, 5, QuadReadAcrossX(h));
    put(l, 6, WaveReadLaneFirst(h + 1.0h));
    puti(l, 7, WaveActiveSum(s));
    puti(l, 8, WaveActiveMin(s));
    puti(l, 9, WavePrefixSum(s));
    puti(l, 10, WaveReadLaneAt(s, 7));
    puti(l, 11, QuadReadAcrossDiagonal(s));
    puti(l, 12, WaveActiveMin(u));
    puti(l, 13, WaveReadLaneAt(u, 30));
    puti(l, 14, WaveActiveMax(u));
    half4 vs = WaveActiveSum(v);
    put(l, 15, vs.x);
    put(l, 16, vs.y);
    put(l, 17, vs.z);
    put(l, 18, vs.w);
    put(l, 19, WaveReadLaneAt(v, 9).y);
    put(l, 20, WaveActiveMin(h));
    puti(l, 21, WaveActiveAllEqual(h) ? 1 : 0);
    puti(l, 22, WaveActiveCountBits(h > 1.0h));
    put(l, 23, QuadReadLaneAt(h, 2));
}
