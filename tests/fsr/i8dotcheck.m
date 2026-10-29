// i8dotcheck: checks SPIRV-Cross's spvSDot4x8Packed/spvUDot4x8Packed/spvSUDot4x8Packed (the helpers it emits for
// SPIR-V OpSDot/OpUDot/OpSUDot on packed 4x8-bit vectors) against integer arithmetic for every pair of byte values
// in every lane, with fast math on and off, and SPIRV-Cross's earlier integer form (OLD_LOWERING).
//   clang -fobjc-arc -framework Metal -framework Foundation tests/fsr/i8dotcheck.m -o i8dotcheck && ./i8dotcheck
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <stdio.h>

static NSString *const kSource = @
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "static inline __attribute__((always_inline)) int spvSDot4x8Packed(uint a, uint b) { return int(dot(float4(as_type<char4>(a)), float4(as_type<char4>(b)))); }\n"
    "static inline __attribute__((always_inline)) uint spvUDot4x8Packed(uint a, uint b) { return uint(dot(float4(as_type<uchar4>(a)), float4(as_type<uchar4>(b)))); }\n"
    "static inline __attribute__((always_inline)) int spvSUDot4x8Packed(uint a, uint b) { return int(dot(float4(as_type<char4>(a)), float4(as_type<uchar4>(b)))); }\n"
    "template <typename T> T reduce_add(vec<T, 4> v) { return v.x + v.y + v.z + v.w; }\n"
    "kernel void k(device atomic_uint *bad [[buffer(0)]], device uint4 *example [[buffer(1)]], uint id [[thread_position_in_grid]]) {\n"
    "    // id = byte a (8 bits) | byte b (8 bits) | lane pattern (4 bits)\n"
    "    uint x = id & 0xffu, y = (id >> 8) & 0xffu, pattern = id >> 16;\n"
    "    // Every lane gets x or a neighbour of x, so lanes hold different values and the extremes in all positions.\n"
    "    uint a = 0, b = 0;\n"
    "    for (uint l = 0; l < 4; l++) {\n"
    "        uint xa = (pattern & (1u << l)) ? x : (x ^ 0x80u);\n"
    "        uint yb = (pattern & (1u << l)) ? (y ^ 0x7fu) : y;\n"
    "        a |= xa << (8 * l); b |= yb << (8 * l);\n"
    "    }\n"
    "    char4 sa = as_type<char4>(a), sb = as_type<char4>(b); uchar4 ua = as_type<uchar4>(a), ub = as_type<uchar4>(b);\n"
    "    int refS = int(sa.x) * sb.x + int(sa.y) * sb.y + int(sa.z) * sb.z + int(sa.w) * sb.w;\n"
    "    uint refU = uint(ua.x) * ub.x + uint(ua.y) * ub.y + uint(ua.z) * ub.z + uint(ua.w) * ub.w;\n"
    "    int refSU = int(sa.x) * int(ub.x) + int(sa.y) * int(ub.y) + int(sa.z) * int(ub.z) + int(sa.w) * int(ub.w);\n"
    "#ifdef OLD_LOWERING\n"
    "    // SPIRV-Cross before this change: uint4(as_type<char4>(a)) * uint4(as_type<char4>(b)), summed.\n"
    "    int s = int(reduce_add(uint4(as_type<char4>(a)) * uint4(as_type<char4>(b))));\n"
    "    uint u = reduce_add(uint4(as_type<uchar4>(a)) * uint4(as_type<uchar4>(b)));\n"
    "    int su = refSU;\n"
    "#else\n"
    "    int s = spvSDot4x8Packed(a, b); uint u = spvUDot4x8Packed(a, b); int su = spvSUDot4x8Packed(a, b);\n"
    "#endif\n"
    "    if (s != refS || u != refU || su != refSU) {\n"
    "        if (atomic_fetch_add_explicit(bad, 1u, memory_order_relaxed) == 0) example[0] = uint4(a, b, uint(s), uint(refS));\n"
    "    }\n"
    "}\n";

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> queue = [dev newCommandQueue];
        int failures = 0;
        // math modes: safe, fast, and relaxed with fast functions and size optimization (what MoltenVK compiles with)
        const char *modeNames[] = {"safe", "fast", "MoltenVK's (relaxed, fast functions, size)"};
        for (int variant = 0; variant < 6; variant++) {
            int fast = variant % 3, old = variant / 3;
            MTLCompileOptions *opts = [MTLCompileOptions new];
            if (old) opts.preprocessorMacros = @{@"OLD_LOWERING": @1};
            opts.mathMode = fast == 2 ? MTLMathModeRelaxed : fast ? MTLMathModeFast : MTLMathModeSafe;
            if (fast == 2) {
                opts.mathFloatingPointFunctions = MTLMathFloatingPointFunctionsFast;
                opts.optimizationLevel = MTLLibraryOptimizationLevelSize;
            }
            NSError *err = nil;
            id<MTLLibrary> lib = [dev newLibraryWithSource:kSource options:opts error:&err];
            if (!lib) { printf("compile failed: %s\n", err.localizedDescription.UTF8String); return 1; }
            id<MTLComputePipelineState> pso = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"k"] error:&err];
            id<MTLBuffer> bad = [dev newBufferWithLength:4 options:MTLResourceStorageModeShared];
            id<MTLBuffer> example = [dev newBufferWithLength:16 options:MTLResourceStorageModeShared];
            memset(bad.contents, 0, 4);
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
            [enc setComputePipelineState:pso];
            [enc setBuffer:bad offset:0 atIndex:0];
            [enc setBuffer:example offset:0 atIndex:1];
            [enc dispatchThreads:MTLSizeMake(1u << 20, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
            [enc endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            uint32_t n = *(uint32_t *)bad.contents, *e = (uint32_t *)example.contents;
            printf("%s lowering, math mode %s: %u of %u inputs wrong", old ? "old" : "new", modeNames[fast], n, 1u << 20);
            if (n) printf(" (a %08x b %08x: got %d, want %d)", e[0], e[1], (int)e[2], (int)e[3]);
            printf("\n");
            failures += n != 0;
        }
        return failures ? 1 : 0;
    }
}
