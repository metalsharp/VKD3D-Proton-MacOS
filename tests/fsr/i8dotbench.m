// i8dotbench: throughput of ways to write a packed signed int8 dot product with a 32-bit accumulator in Metal
// (HLSL dot4add_i8packed, SPIR-V OpSDot with PackedVectorFormat4x8Bit), on this Mac's GPU. Each variant must give
// the exact result; they are checked against the first.
//   clang -fobjc-arc -framework Metal -framework Foundation tests/fsr/i8dotbench.m -o i8dotbench && ./i8dotbench
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <stdio.h>

static NSString *kernelSource(NSString *dotExpr) {
    return [NSString stringWithFormat:@
        "#include <metal_stdlib>\n"
        "using namespace metal;\n"
        "template <typename T> T reduce_add(vec<T, 4> v) { return v.x + v.y + v.z + v.w; }\n"  // SPIRV-Cross's helper
        "kernel void k(device const uint *in [[buffer(0)]], device int *out [[buffer(1)]], uint id [[thread_position_in_grid]]) {\n"
        "    uint a = in[id & 1023u], b = in[(id + 7u) & 1023u];\n"
        "    int acc0 = 0, acc1 = 0, acc2 = 0, acc3 = 0;\n"
        "    for (int i = 0; i < 256; i++) {\n"
        "        uint x = a, y = b;  acc0 += %1$@;\n"
        "        x = a ^ 0x5a5a5a5au; y = b + 0x01010101u; acc1 += %1$@;\n"
        "        x = a + 0x10203040u; y = b ^ 0xa5a5a5a5u; acc2 += %1$@;\n"
        "        x = a * 3u; y = b ^ 0x0f0f0f0fu; acc3 += %1$@;\n"
        "        a = (a << 1) ^ (a >> 3) ^ uint(i); b = (b >> 1) ^ (b << 5) ^ 0x9e3779b9u;\n"
        "    }\n"
        "    out[id] = acc0 + acc1 + acc2 + acc3;\n"
        "}\n", dotExpr];
}

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> queue = [dev newCommandQueue];
        printf("%s\n", dev.name.UTF8String);
        NSArray *names = @[@"int32 multiply (SPIRV-Cross before)", @"int16 multiply", @"float dot (SPIRV-Cross)", @"mul24",
                           @"unorm8 unpack, offset-binary dot", @"half4 unpack, float dot",
                           @"unorm8 unpack, recentred (fma)"];
        NSArray *exprs = @[
            @"int(reduce_add(int4(as_type<char4>(x)) * int4(as_type<char4>(y))))",
            @"reduce_add(int4(short4(as_type<char4>(x)) * short4(as_type<char4>(y))))",
            @"int(dot(float4(as_type<char4>(x)), float4(as_type<char4>(y))))",
            @"reduce_add(mul24(int4(as_type<char4>(x)), int4(as_type<char4>(y))))",
            // Signed bytes as offset binary (x ^ 0x80808080 = s + 128) unpacked by the hardware's unorm8 conversion:
            // dot(s, t) = dot(u, v) - 128 (sum(u) + sum(v)) + 4 * 16384, with u, v in [0, 255], all exact in float.
            @"int(rint(dot(unpack_unorm4x8_to_float(x ^ 0x80808080u) * 255.0f, unpack_unorm4x8_to_float(y ^ 0x80808080u) * 255.0f) - 128.0f * (dot(unpack_unorm4x8_to_float(x ^ 0x80808080u), float4(255.0f)) + dot(unpack_unorm4x8_to_float(y ^ 0x80808080u), float4(255.0f))) + 65536.0f))",
            @"int(dot(float4(half4(as_type<char4>(x))), float4(half4(as_type<char4>(y)))))",
            // unorm8(s + 128) * 255 - 128 = s (to within 2^-16), so the float dot is within 0.02 of the exact integer.
            // The fastest, and exact on its own, but SPIRV-Cross doesn't use the unorm8 forms: in AMD FSR 4's networks
            // they changed the output even where the shader went on to use the integer result (docs/fsr4.md).
            @"int(rint(dot(fma(unpack_unorm4x8_to_float(x ^ 0x80808080u), float4(255.0f), float4(-128.0f)), fma(unpack_unorm4x8_to_float(y ^ 0x80808080u), float4(255.0f), float4(-128.0f)))))",
        ];
        const NSUInteger threads = 1u << 22;  // 4M threads x 256 iterations x 4 dots = 4G dot4 per dispatch
        id<MTLBuffer> in = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
        uint32_t *p = (uint32_t *)in.contents;
        for (int i = 0; i < 1024; i++) p[i] = (uint32_t)(i * 2654435761u) ^ 0x9e3779b9u;
        NSMutableArray *outs = [NSMutableArray array];
        for (NSUInteger v = 0; v < exprs.count; v++) {
            NSError *err = nil;
            MTLCompileOptions *opts = [MTLCompileOptions new];
            id<MTLLibrary> lib = [dev newLibraryWithSource:kernelSource(exprs[v]) options:opts error:&err];
            if (!lib) { printf("%s: compile failed: %s\n", [names[v] UTF8String], err.localizedDescription.UTF8String); return 1; }
            id<MTLComputePipelineState> pso = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"k"] error:&err];
            id<MTLBuffer> out = [dev newBufferWithLength:threads * 4 options:MTLResourceStorageModeShared];
            [outs addObject:out];
            double best = 1e9;
            for (int run = 0; run < 6; run++) {
                id<MTLCommandBuffer> cb = [queue commandBuffer];
                id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
                [enc setComputePipelineState:pso];
                [enc setBuffer:in offset:0 atIndex:0];
                [enc setBuffer:out offset:0 atIndex:1];
                [enc dispatchThreads:MTLSizeMake(threads, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
                [enc endEncoding];
                [cb commit];
                [cb waitUntilCompleted];
                double ms = (cb.GPUEndTime - cb.GPUStartTime) * 1000.0;
                if (run > 0 && ms < best) best = ms;
            }
            double dots = (double)threads * 256 * 4;
            int same = 1;
            if (v > 0) same = memcmp(out.contents, ((id<MTLBuffer>)outs[0]).contents, threads * 4) == 0;
            printf("%-36s %8.2f ms  %7.1f G dot4/s  %s\n", [names[v] UTF8String], best, dots / best / 1e6,
                   same ? (v ? "same result" : "reference") : "DIFFERENT RESULT");
        }
    }
    return 0;
}
