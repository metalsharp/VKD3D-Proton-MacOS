// Metal sparse resources for D3D12 tiled resources (tier 2/3) on Apple GPUs:
// - residency queries (sparse_sample().resident()) on mapped and unmapped tiles
// - placement-sparse buffers mapped through MTL4CommandQueue updateBufferMappings
// - 3D sparse textures (tier 3) and the tile shapes of common formats
//   clang++ -fobjc-arc -framework Metal -framework Foundation sparse.mm -o sparse
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <stdio.h>

static int failures;
#define EXPECT(c, ...) do { if (!(c)) { failures++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static id<MTLComputePipelineState> pipeline(id<MTLDevice> dev, NSString *src, const char *name)
{
    NSError *err = nil;
    MTLCompileOptions *o = [MTLCompileOptions new];
    o.languageVersion = MTLLanguageVersion3_1;
    id<MTLLibrary> lib = [dev newLibraryWithSource:src options:o error:&err];
    if (!lib) { printf("compile: %s\n", err.localizedDescription.UTF8String); exit(2); }
    id<MTLComputePipelineState> p = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@(name)] error:&err];
    if (!p) { printf("pipeline: %s\n", err.localizedDescription.UTF8String); exit(2); }
    return p;
}

int main(void)
{
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTL4CommandQueue> q4 = [dev newMTL4CommandQueueWithDescriptor:[MTL4CommandQueueDescriptor new] error:nil];
        id<MTLSharedEvent> ev = [dev newSharedEvent];
        uint64_t evv = 0;
        printf("sparseTileSizeInBytes(64K)=%lu\n", (unsigned long)[dev sparseTileSizeInBytesForSparsePageSize:MTLSparsePageSize64]);

        // Tile shapes (64 KiB pages, matching D3D12's 64 KiB tiles).
        struct { MTLPixelFormat f; const char *n; } fmts[] = {
            { MTLPixelFormatR8Unorm, "R8" }, { MTLPixelFormatRGBA8Unorm, "RGBA8" }, { MTLPixelFormatRGBA16Float, "RGBA16F" },
            { MTLPixelFormatRGBA32Float, "RGBA32F" }, { MTLPixelFormatBC1_RGBA, "BC1" }, { MTLPixelFormatBC7_RGBAUnorm, "BC7" },
            { MTLPixelFormatDepth32Float, "D32F" },
        };
        for (auto &f : fmts)
        {
            MTLSize s2 = [dev sparseTileSizeWithTextureType:MTLTextureType2D pixelFormat:f.f sampleCount:1 sparsePageSize:MTLSparsePageSize64];
            MTLSize s3 = [dev sparseTileSizeWithTextureType:MTLTextureType3D pixelFormat:f.f sampleCount:1 sparsePageSize:MTLSparsePageSize64];
            printf("tile %-8s 2D %lux%lux%lu  3D %lux%lux%lu\n", f.n, s2.width, s2.height, s2.depth, s3.width, s3.height, s3.depth);
        }

        MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
        hd.type = MTLHeapTypePlacement;
        hd.size = 64ull << 20;
        hd.storageMode = MTLStorageModePrivate;
        hd.maxCompatiblePlacementSparsePageSize = MTLSparsePageSize64;
        id<MTLHeap> heap = [dev newHeapWithDescriptor:hd];

        // 1. Residency: map tile (0,0) only, sample tiles (0,0) and (1,0).
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:512 height:512 mipmapped:YES];
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        td.storageMode = MTLStorageModePrivate;
        td.placementSparsePageSize = MTLSparsePageSize64;
        id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
        printf("2D sparse: mips %lu firstMipmapInTail %lu tailSizeInBytes %lu tier %d\n", tex.mipmapLevelCount,
               tex.firstMipmapInTail, tex.tailSizeInBytes, (int)tex.sparseTextureTier);
        MTL4UpdateSparseTextureMappingOperation op = {};
        op.mode = MTLSparseTextureMappingModeMap;
        op.textureRegion = MTLRegionMake2D(0, 0, 1, 1);
        [q4 updateTextureMappings:tex heap:heap operations:&op count:1];
        [q4 signalEvent:ev value:++evv];

        id<MTLComputePipelineState> fill = pipeline(dev, @"#include <metal_stdlib>\nusing namespace metal;\n"
            "kernel void k(texture2d<float, access::write> t [[texture(0)]], uint2 p [[thread_position_in_grid]]) { t.write(float4(0.5), p); }", "k");
        id<MTLComputePipelineState> probe = pipeline(dev, @"#include <metal_stdlib>\nusing namespace metal;\n"
            "kernel void k(texture2d<float> t [[texture(0)]], device float4 *o [[buffer(0)]], device uint *r [[buffer(1)]]) {\n"
            "  constexpr sampler s(filter::nearest, mip_filter::nearest);\n"
            "  auto a = t.sparse_sample(s, float2(8.5, 8.5) / 512.0, level(0));\n"
            "  auto b = t.sparse_sample(s, float2(200.5, 8.5) / 512.0, level(0));\n"
            "  auto c = t.sparse_read(uint2(8, 8));\n"
            "  auto d = t.sparse_read(uint2(200, 8));\n"
            "  o[0] = a.value(); o[1] = b.value(); o[2] = c.value(); o[3] = d.value();\n"
            "  r[0] = a.resident(); r[1] = b.resident(); r[2] = c.resident(); r[3] = d.resident();\n"
            "}", "k");
        id<MTLBuffer> out = [dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
        id<MTLBuffer> res = [dev newBufferWithLength:64 options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        [cb encodeWaitForEvent:ev value:evv];
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e setComputePipelineState:fill];
        [e setTexture:tex atIndex:0];
        [e dispatchThreads:MTLSizeMake(512, 512, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
        [e memoryBarrierWithScope:MTLBarrierScopeTextures];
        [e setComputePipelineState:probe];
        [e setBuffer:out offset:0 atIndex:0];
        [e setBuffer:res offset:0 atIndex:1];
        [e dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [e endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        float *ov = (float *)out.contents;
        uint32_t *rv = (uint32_t *)res.contents;
        printf("sample mapped %.2f resident %u | unmapped %.2f resident %u | read mapped %.2f resident %u | unmapped %.2f resident %u\n",
               ov[0], rv[0], ov[4], rv[1], ov[8], rv[2], ov[12], rv[3]);
        EXPECT(rv[0] == 1 && ov[0] > 0.49f && ov[0] < 0.51f, "mapped tile sample");
        EXPECT(rv[1] == 0 && ov[4] == 0.0f, "unmapped tile sample must be non-resident and 0");
        EXPECT(rv[2] == 1 && rv[3] == 0, "sparse_read residency");

        // 2. Sparse buffer: map tile 1 of 4, write everywhere, read back through a copy.
        NSUInteger tile = [dev sparseTileSizeInBytesForSparsePageSize:MTLSparsePageSize64];
        id<MTLBuffer> sb = [dev newBufferWithLength:4 * tile options:MTLResourceStorageModePrivate placementSparsePageSize:MTLSparsePageSize64];
        printf("sparse buffer: %s tier %d gpuAddress %s\n", sb ? "ok" : "FAIL", sb ? (int)sb.sparseBufferTier : -1,
               sb && sb.gpuAddress ? "yes" : "no");
        if (sb)
        {
            // Unmap everything first: does a new sparse buffer start out mapped?
            MTL4UpdateSparseBufferMappingOperation uop = {};
            uop.mode = MTLSparseTextureMappingModeUnmap;
            uop.bufferRange = NSMakeRange(0, 4);
            if (getenv("UNMAP_FIRST"))
                [q4 updateBufferMappings:sb heap:nil operations:&uop count:1];
            MTL4UpdateSparseBufferMappingOperation bop = {};
            bop.mode = MTLSparseTextureMappingModeMap;
            bop.bufferRange = NSMakeRange(1, 1);
            bop.heapOffset = 64;
            [q4 updateBufferMappings:sb heap:heap operations:&bop count:1];
            [q4 signalEvent:ev value:++evv];
            id<MTLComputePipelineState> bw = pipeline(dev, @"#include <metal_stdlib>\nusing namespace metal;\n"
                "kernel void k(device uint *b [[buffer(0)]], uint i [[thread_position_in_grid]]) { b[i] = 0xA000u + i; }", "k");
            id<MTLBuffer> rb = [dev newBufferWithLength:4 * tile options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> cb2 = [q commandBuffer];
            [cb2 encodeWaitForEvent:ev value:evv];
            id<MTLComputeCommandEncoder> e2 = [cb2 computeCommandEncoder];
            [e2 setComputePipelineState:bw];
            [e2 setBuffer:sb offset:0 atIndex:0];
            [e2 dispatchThreads:MTLSizeMake(4 * tile / 4, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
            [e2 endEncoding];
            [cb2 commit];
            [cb2 waitUntilCompleted];
            cb2 = [q commandBuffer];
            id<MTLBlitCommandEncoder> b2 = [cb2 blitCommandEncoder];
            [b2 copyFromBuffer:sb sourceOffset:0 toBuffer:rb destinationOffset:0 size:4 * tile];
            [b2 endEncoding];
            [cb2 commit];
            [cb2 waitUntilCompleted];
            printf("sparse buffer: %s\n", cb2.error ? cb2.error.localizedDescription.UTF8String : "no error");
            uint32_t *bv = (uint32_t *)rb.contents;
            size_t w = tile / 4;
            printf("words: tile0 0x%x tile1 0x%x tile2 0x%x\n", bv[5], bv[w + 5], bv[2 * w + 5]);
            // Does the mapping alias the heap? Read heap tile 64 through a placed buffer.
            id<MTLBuffer> alias = [heap newBufferWithLength:tile options:MTLResourceStorageModePrivate offset:64 * tile];
            id<MTLBuffer> rb2 = [dev newBufferWithLength:tile options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> cb3 = [q commandBuffer];
            id<MTLBlitCommandEncoder> b3 = [cb3 blitCommandEncoder];
            [b3 copyFromBuffer:alias sourceOffset:0 toBuffer:rb2 destinationOffset:0 size:tile];
            [b3 endEncoding];
            [cb3 commit];
            [cb3 waitUntilCompleted];
            printf("heap tile 64 through an alias: 0x%x (the sparse buffer's tile 1 wrote 0x%zx)\n",
                   ((uint32_t *)rb2.contents)[5], (size_t)(0xA000u + w + 5));
            EXPECT(bv[w + 5] == 0xA000u + w + 5, "mapped buffer tile write");
            EXPECT(bv[5] == 0 && bv[2 * w + 5] == 0, "unmapped buffer tiles read 0");
        }

        // 3. 3D sparse texture.
        MTLTextureDescriptor *t3 = [MTLTextureDescriptor new];
        t3.textureType = MTLTextureType3D;
        t3.pixelFormat = MTLPixelFormatRGBA8Unorm;
        t3.width = t3.height = t3.depth = 128;
        t3.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        t3.storageMode = MTLStorageModePrivate;
        t3.placementSparsePageSize = MTLSparsePageSize64;
        id<MTLTexture> tex3 = [dev newTextureWithDescriptor:t3];
        printf("3D sparse texture: %s isSparse %d\n", tex3 ? "ok" : "FAIL", tex3 ? tex3.isSparse : 0);
        EXPECT(tex3 && tex3.isSparse, "3D sparse texture");

        printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
        return failures ? 1 : 0;
    }
}
