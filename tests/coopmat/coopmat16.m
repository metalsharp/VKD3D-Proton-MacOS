// coopmat16: building blocks of SPIRV-Cross's 16x16 cooperative matrices on Metal, tested on this GPU.
//  1. The layout of simdgroup_matrix<T, 8, 8>: which elements each SIMD lane's thread_elements() hold.
//  2. D = A * B + C for 16x16x16 matrices held as AMD RDNA 4 lays out WMMA operands, each lane 8 elements:
//     A[t % 16][(t / 16) * 8 + e], B[(t / 16) * 8 + e][t % 16], C and D [(t / 16) * 8 + e][t % 16].
//     "mma" moves them into simdgroup_matrix tiles with SIMD shuffles and multiplies there; "alu" gathers each lane's
//     rows and column and multiplies with FMAs. Both are checked against the CPU.
//  3. Their throughput.
//   clang -fobjc-arc -framework Metal -framework Foundation tests/coopmat/coopmat16.m -o coopmat16 && ./coopmat16
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static NSString *const kSource = @
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    // Metal's 8x8 layout (MLX's BaseMMAFrag): lane l holds row x, columns y and y + 1.
    "static inline ushort2 spvSimdCoord(ushort l) { ushort q = l >> 2; return ushort2((q & 4) + ((l >> 1) & 3), (q & 2) * 2 + (l & 1) * 2); }\n"
    "kernel void layout(device float *out [[buffer(0)]], ushort l [[thread_index_in_simdgroup]]) {\n"
    "    threadgroup float idx[64];\n"
    "    for (ushort i = l; i < 64; i += 32) idx[i] = float(i);\n"
    "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
    "    simdgroup_float8x8 m; simdgroup_load(m, idx, 8);\n"
    "    out[l * 2] = m.thread_elements()[0]; out[l * 2 + 1] = m.thread_elements()[1];\n"
    "    ushort2 c = spvSimdCoord(l); out[64 + l * 2] = float(c.x * 8 + c.y); out[64 + l * 2 + 1] = float(c.x * 8 + c.y + 1);\n"
    "}\n"
    "template <typename T> struct spvRdnaMat { vec<T, 4> lo, hi; };\n"
    "template <typename M, typename T> static inline void spvSetTile(thread M &m, vec<T, 2> v) { m.thread_elements()[0] = v.x; m.thread_elements()[1] = v.y; }\n"
    "template <typename M> static inline auto spvGetTile(thread M &m) { return vec<decltype(m.thread_elements()[0] + 0), 2>(m.thread_elements()[0], m.thread_elements()[1]); }\n"
    "template <typename T> static inline T spvPick(vec<T, 4> lo, vec<T, 4> hi, ushort i) { return i < 4 ? lo[i & 3] : hi[i & 3]; }\n"
    // Element (r, c) of a 16x16 matrix in RDNA 4's layout lives in lane owner, element idx.
    "static inline ushort2 spvRdnaOwnerA(ushort r, ushort c) { return ushort2(r + (c & 8) * 2, c & 7); }\n"
    "static inline ushort2 spvRdnaOwnerBC(ushort r, ushort c) { return ushort2(c + (r & 8) * 2, r & 7); }\n"
    // Two elements of a lane's Metal tile, (r, c) and (r, c + 1), gathered from their owners.
    "template <typename T> static inline vec<T, 2> spvGather(thread const spvRdnaMat<T> &m, ushort2 o0, ushort2 o1) {\n"
    "    vec<T, 4> l0 = simd_shuffle(m.lo, o0.x), h0 = simd_shuffle(m.hi, o0.x);\n"
    "    vec<T, 4> l1 = simd_shuffle(m.lo, o1.x), h1 = simd_shuffle(m.hi, o1.x);\n"
    "    return vec<T, 2>(spvPick(l0, h0, o0.y), spvPick(l1, h1, o1.y));\n"
    "}\n"
    "template <typename T> static inline vec<T, 2> spvGatherPair(thread const spvRdnaMat<T> &m, ushort2 o) {\n"
    "    vec<T, 4> l0 = simd_shuffle(m.lo, o.x), h0 = simd_shuffle(m.hi, o.x);\n"
    "    return vec<T, 2>(spvPick(l0, h0, o.y), spvPick(l0, h0, ushort(o.y + 1)));\n"
    "}\n"
    // D = A B + C through simdgroup_matrix tiles. A: (r, c) and (r, c + 1) share an owner (a pair); B, C: two owners.
    "static inline spvRdnaMat<float> spvMulAddMMA(thread const spvRdnaMat<half> &a, thread const spvRdnaMat<half> &b,\n"
    "        thread const spvRdnaMat<float> &c, ushort l) {\n"
    "    ushort2 x = spvSimdCoord(l);\n"
    "    simdgroup_half8x8 ta[2][2], tb[2][2]; simdgroup_float8x8 tc[2][2];\n"
    "    for (ushort i = 0; i < 2; i++) for (ushort j = 0; j < 2; j++) {\n"
    "        ushort r = i * 8 + x.x, col = j * 8 + x.y;\n"
    "        spvSetTile(ta[i][j], spvGatherPair(a, spvRdnaOwnerA(r, col)));\n"
    "        spvSetTile(tb[i][j], spvGather(b, spvRdnaOwnerBC(r, col), spvRdnaOwnerBC(r, col + 1)));\n"
    "        spvSetTile(tc[i][j], spvGather(c, spvRdnaOwnerBC(r, col), spvRdnaOwnerBC(r, col + 1)));\n"
    "    }\n"
    "    for (ushort i = 0; i < 2; i++) for (ushort j = 0; j < 2; j++) {\n"
    "        simdgroup_multiply_accumulate(tc[i][j], ta[i][0], tb[0][j], tc[i][j]);\n"
    "        simdgroup_multiply_accumulate(tc[i][j], ta[i][1], tb[1][j], tc[i][j]);\n"
    "    }\n"
    // Back to RDNA 4's layout: lane l's element e is D[(l / 16) * 8 + e][l % 16], in tile (e row half, column half),
    // held by the Metal lane whose coordinate is ((l / 16) * 8 + e) % 8, (l % 16) % 8 (column pair).
    "    float2 t00 = spvGetTile(tc[0][0]), t01 = spvGetTile(tc[0][1]);\n"
    "    float2 t10 = spvGetTile(tc[1][0]), t11 = spvGetTile(tc[1][1]);\n"
    "    float4 lo = float4(t00, t01), hi = float4(t10, t11);\n"
    "    spvRdnaMat<float> d;\n"
    "    ushort col = l & 15, rh = l >> 4;\n"
    "    for (ushort e = 0; e < 8; e++) {\n"
    // Metal lane holding row e (within the tile) and column pair col & 6: invert spvSimdCoord.
    "        ushort cc = col & 7, src = ((e & 4) << 2) + ((e & 3) << 1) + ((cc & 4) << 1) + ((cc >> 1) & 1);\n"
    "        float4 s = rh ? simd_shuffle(hi, src) : simd_shuffle(lo, src);\n"
    "        float v = (col & 8) ? s[2 + (cc & 1)] : s[cc & 1];\n"
    "        if (e < 4) d.lo[e] = v; else d.hi[e - 4] = v;\n"
    "    }\n"
    "    return d;\n"
    "}\n"
    // D = A B + C with FMAs: lane l computes D[(l / 16) * 8 + e][l % 16] for e = 0..7.
    "static inline spvRdnaMat<float> spvMulAddALU(thread const spvRdnaMat<half> &a, thread const spvRdnaMat<half> &b,\n"
    "        thread const spvRdnaMat<float> &c, ushort l) {\n"
    "    ushort col = l & 15, rh = l >> 4;\n"
    "    half4 bl0 = simd_shuffle(b.lo, col), bh0 = simd_shuffle(b.hi, col);\n"
    "    half4 bl1 = simd_shuffle(b.lo, ushort(col + 16)), bh1 = simd_shuffle(b.hi, ushort(col + 16));\n"
    "    spvRdnaMat<float> d;\n"
    "    for (ushort e = 0; e < 8; e++) {\n"
    "        ushort r = rh * 8 + e;\n"
    "        half4 al0 = simd_shuffle(a.lo, r), ah0 = simd_shuffle(a.hi, r);\n"
    "        half4 al1 = simd_shuffle(a.lo, ushort(r + 16)), ah1 = simd_shuffle(a.hi, ushort(r + 16));\n"
    "        float s = (e < 4 ? c.lo[e] : c.hi[e - 4]);\n"
    "        s += dot(float4(al0), float4(bl0)) + dot(float4(ah0), float4(bh0)) + dot(float4(al1), float4(bl1)) + dot(float4(ah1), float4(bh1));\n"
    "        if (e < 4) d.lo[e] = s; else d.hi[e - 4] = s;\n"
    "    }\n"
    "    return d;\n"
    "}\n"
    "template <typename T> static inline spvRdnaMat<T> spvLoadA(device const T *p, ushort l) {\n"
    "    spvRdnaMat<T> m; for (ushort e = 0; e < 4; e++) { m.lo[e] = p[(l % 16) * 16 + (l / 16) * 8 + e]; m.hi[e] = p[(l % 16) * 16 + (l / 16) * 8 + e + 4]; } return m; }\n"
    "template <typename T> static inline spvRdnaMat<T> spvLoadBC(device const T *p, ushort l) {\n"
    "    spvRdnaMat<T> m; for (ushort e = 0; e < 4; e++) { m.lo[e] = p[((l / 16) * 8 + e) * 16 + l % 16]; m.hi[e] = p[((l / 16) * 8 + e + 4) * 16 + l % 16]; } return m; }\n"
    "kernel void check(device const half *A [[buffer(0)]], device const half *B [[buffer(1)]], device const float *C [[buffer(2)]],\n"
    "                  device float *D [[buffer(3)]], constant uint &mode [[buffer(4)]], ushort l [[thread_index_in_simdgroup]]) {\n"
    "    spvRdnaMat<half> a = spvLoadA(A, l), b = spvLoadBC(B, l); spvRdnaMat<float> c = spvLoadBC(C, l);\n"
    "    spvRdnaMat<float> d = mode ? spvMulAddALU(a, b, c, l) : spvMulAddMMA(a, b, c, l);\n"
    "    for (ushort e = 0; e < 8; e++) D[((l / 16) * 8 + e) * 16 + l % 16] = e < 4 ? d.lo[e] : d.hi[e - 4];\n"
    "}\n"
    // Throughput: each SIMD-group chains ITER multiply-adds.
    "kernel void bench(device const half *A [[buffer(0)]], device const half *B [[buffer(1)]], device const float *C [[buffer(2)]],\n"
    "                  device float *D [[buffer(3)]], constant uint &mode [[buffer(4)]], uint g [[threadgroup_position_in_grid]],\n"
    "                  ushort l [[thread_index_in_simdgroup]]) {\n"
    "    spvRdnaMat<half> a = spvLoadA(A, l), b = spvLoadBC(B, l); spvRdnaMat<float> c = spvLoadBC(C, l);\n"
    "    for (uint i = 0; i < 256; i++) {\n"
    "        c = mode ? spvMulAddALU(a, b, c, l) : spvMulAddMMA(a, b, c, l);\n"
    "        a.lo = half4(c.lo * 0.001f); \n"
    "    }\n"
    "    D[g * 32 + l] = c.lo.x + c.hi.w;\n"
    "}\n"
    // The ceiling: 16x16x16 multiply-adds kept in Metal's layout (2x2 tiles, 8 simdgroup_multiply_accumulate).
    "kernel void bench_native(device const half *A [[buffer(0)]], device float *D [[buffer(3)]], uint g [[threadgroup_position_in_grid]],\n"
    "                         ushort l [[thread_index_in_simdgroup]]) {\n"
    "    simdgroup_half8x8 a[2][2], b[2][2]; simdgroup_float8x8 c[2][2];\n"
    "    for (int i = 0; i < 2; i++) for (int j = 0; j < 2; j++) { simdgroup_load(a[i][j], A + i * 128 + j * 8, 16); simdgroup_load(b[i][j], A + j * 128 + i * 8, 16); c[i][j] = make_filled_simdgroup_matrix<float, 8, 8>(0.f); }\n"
    "    for (uint it = 0; it < 256; it++)\n"
    "        for (int i = 0; i < 2; i++) for (int j = 0; j < 2; j++) {\n"
    "            simdgroup_multiply_accumulate(c[i][j], a[i][0], b[0][j], c[i][j]);\n"
    "            simdgroup_multiply_accumulate(c[i][j], a[i][1], b[1][j], c[i][j]);\n"
    "        }\n"
    "    D[g * 32 + l] = c[0][0].thread_elements()[0] + c[1][1].thread_elements()[1];\n"
    "}\n";

static unsigned short f2h(float f)
{
    __fp16 h = (__fp16)f; unsigned short u; memcpy(&u, &h, 2); return u;
}
static float h2f(unsigned short u)
{
    __fp16 h; memcpy(&h, &u, 2); return (float)h;
}

int main(void)
{
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> queue = [dev newCommandQueue];
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSource options:[MTLCompileOptions new] error:&err];
        if (!lib) { printf("compile failed: %s\n", err.localizedDescription.UTF8String); return 1; }

        /* 1. layout */
        id<MTLComputePipelineState> pso = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"layout"] error:&err];
        id<MTLBuffer> out = [dev newBufferWithLength:128 * 4 options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:pso];
        [enc setBuffer:out offset:0 atIndex:0];
        [enc dispatchThreads:MTLSizeMake(32, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
        [enc endEncoding]; [cb commit]; [cb waitUntilCompleted];
        float *o = (float *)out.contents;
        int bad = 0, failures = 0;
        for (int i = 0; i < 64; i++) bad += o[i] != o[64 + i];
        printf("simdgroup_matrix 8x8 layout: %s\n", bad ? "DIFFERENT from the formula" : "as expected");
        failures += !!bad;

        /* 2. correctness */
        id<MTLBuffer> A = [dev newBufferWithLength:512 options:MTLResourceStorageModeShared];
        id<MTLBuffer> B = [dev newBufferWithLength:512 options:MTLResourceStorageModeShared];
        id<MTLBuffer> C = [dev newBufferWithLength:1024 options:MTLResourceStorageModeShared];
        id<MTLBuffer> D = [dev newBufferWithLength:1024 * 1024 options:MTLResourceStorageModeShared];
        unsigned short *a = A.contents, *b = B.contents; float *c = C.contents;
        srand(1);
        for (int i = 0; i < 256; i++) { a[i] = f2h((rand() % 64 - 32) / 16.0f); b[i] = f2h((rand() % 64 - 32) / 16.0f); c[i] = (rand() % 100) / 7.0f; }
        id<MTLComputePipelineState> check = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"check"] error:&err];
        id<MTLComputePipelineState> bench = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"bench"] error:&err];
        const char *names[2] = {"simdgroup_matrix + shuffles", "FMAs + shuffles"};
        for (uint32_t mode = 0; mode < 2; mode++) {
            cb = [queue commandBuffer]; enc = [cb computeCommandEncoder];
            [enc setComputePipelineState:check];
            [enc setBuffer:A offset:0 atIndex:0]; [enc setBuffer:B offset:0 atIndex:1]; [enc setBuffer:C offset:0 atIndex:2];
            [enc setBuffer:D offset:0 atIndex:3]; [enc setBytes:&mode length:4 atIndex:4];
            [enc dispatchThreads:MTLSizeMake(32, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
            [enc endEncoding]; [cb commit]; [cb waitUntilCompleted];
            float *d = D.contents; double maxerr = 0;
            for (int r = 0; r < 16; r++) for (int col = 0; col < 16; col++) {
                double s = c[r * 16 + col];
                for (int k = 0; k < 16; k++) s += h2f(a[r * 16 + k]) * h2f(b[k * 16 + col]);
                maxerr = fmax(maxerr, fabs(s - d[r * 16 + col]));
            }
            printf("D = A B + C, %s: max error %g %s\n", names[mode], maxerr, maxerr < 1e-3 ? "(ok)" : "(WRONG)");
            failures += maxerr >= 1e-3;

            /* 3. throughput: 32768 SIMD-groups x 256 multiply-adds of 16x16x16 */
            double best = 1e9;
            for (int run = 0; run < 4; run++) {
                cb = [queue commandBuffer]; enc = [cb computeCommandEncoder];
                [enc setComputePipelineState:bench];
                [enc setBuffer:A offset:0 atIndex:0]; [enc setBuffer:B offset:0 atIndex:1]; [enc setBuffer:C offset:0 atIndex:2];
                [enc setBuffer:D offset:0 atIndex:3]; [enc setBytes:&mode length:4 atIndex:4];
                [enc dispatchThreadgroups:MTLSizeMake(8192, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
                [enc endEncoding]; [cb commit]; [cb waitUntilCompleted];
                double ms = (cb.GPUEndTime - cb.GPUStartTime) * 1000.0;
                if (run && ms < best) best = ms;
            }
            double macs = 8192.0 * 256 * 4096;
            printf("  throughput: %.2f ms, %.0f G multiply-adds/s\n", best, macs / best / 1e6);
        }
        id<MTLComputePipelineState> native = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"bench_native"] error:&err];
        double best = 1e9;
        for (int run = 0; run < 4; run++) {
            cb = [queue commandBuffer]; enc = [cb computeCommandEncoder];
            [enc setComputePipelineState:native];
            [enc setBuffer:A offset:0 atIndex:0]; [enc setBuffer:D offset:0 atIndex:3];
            [enc dispatchThreadgroups:MTLSizeMake(8192, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
            [enc endEncoding]; [cb commit]; [cb waitUntilCompleted];
            double ms = (cb.GPUEndTime - cb.GPUStartTime) * 1000.0;
            if (run && ms < best) best = ms;
        }
        printf("D = A B + C kept in simdgroup_matrix tiles (the ceiling): %.2f ms, %.0f G multiply-adds/s\n", best,
               8192.0 * 256 * 4096 / best / 1e6);
        return failures ? 1 : 0;
    }
}
