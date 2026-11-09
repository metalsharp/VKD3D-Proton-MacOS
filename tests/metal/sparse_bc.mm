// Which texels one sparse tile of a block-compressed texture covers on Apple GPUs: for each format, map a single
// tile, (tx, ty) in the units of sparseTileSizeWithTextureType, copy data over the whole texture, copy it back and
// print which 4x4 blocks kept it (one character per 16x16 blocks). D3D12's tiles for 8-byte blocks (BC1, BC4) are
// 512x256 texels; CONTROL Resonant rendered its BC1/BC4 materials as noise.
//   clang++ -fobjc-arc -framework Metal -framework Foundation sparse_bc.mm -o sparse_bc
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>

int main(void)
{
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTL4CommandQueue> q4 = [dev newMTL4CommandQueueWithDescriptor:[MTL4CommandQueueDescriptor new] error:nil];
        id<MTLSharedEvent> ev = [dev newSharedEvent];
        uint64_t evv = 0;
        MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
        hd.type = MTLHeapTypePlacement;
        hd.size = 16ull << 20;
        hd.storageMode = MTLStorageModePrivate;
        hd.maxCompatiblePlacementSparsePageSize = MTLSparsePageSize64;
        id<MTLHeap> heap = [dev newHeapWithDescriptor:hd];
        // The heap reaches the GPU only through a residency set (as MoltenVK's MVKDevice::makeResident()).
        id<MTLResidencySet> rset = [dev newResidencySetWithDescriptor:[MTLResidencySetDescriptor new] error:nil];
        [rset addAllocation:heap];
        [rset commit];
        [rset requestResidency];
        [q addResidencySet:rset];

        struct { MTLPixelFormat f; const char *n; unsigned bw, bytes; } fmts[] = {
            { MTLPixelFormatRGBA8Unorm, "RGBA8", 1, 4 }, { MTLPixelFormatRG32Uint, "RG32UI", 1, 8 },
            { MTLPixelFormatBC1_RGBA, "BC1", 4, 8 }, { MTLPixelFormatBC4_RUnorm, "BC4", 4, 8 },
            { MTLPixelFormatBC3_RGBA, "BC3", 4, 16 }, { MTLPixelFormatBC7_RGBAUnorm, "BC7", 4, 16 },
        };
        const unsigned W = 2048, H = 1024;
        int tiles[][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 } };
        for (auto &f : fmts)
        {
            MTLSize ts = [dev sparseTileSizeWithTextureType:MTLTextureType2D pixelFormat:f.f sampleCount:1 sparsePageSize:MTLSparsePageSize64];
            printf("%s: tile %lux%lu texels\n", f.n, ts.width, ts.height);
            unsigned bx = W / f.bw, by = H / f.bw, pitch = bx * f.bytes;
            id<MTLBuffer> src = [dev newBufferWithLength:pitch * by options:MTLResourceStorageModeShared];
            memset(src.contents, 0xA5, src.length);
            id<MTLBuffer> dst = [dev newBufferWithLength:pitch * by options:MTLResourceStorageModeShared];
            for (auto &t : tiles)
            {
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f.f width:W height:H mipmapped:NO];
                // SPARSE_USAGE=write adds MoltenVK's sparse usage (ShaderWrite, tier 1); =view adds PixelFormatView too.
                const char *u = getenv("SPARSE_USAGE");
                td.usage = MTLTextureUsageShaderRead;
                if (u && (!strcmp(u, "write") || !strcmp(u, "view")))
                    td.usage |= MTLTextureUsageShaderWrite;
                if (u && (!strcmp(u, "view") || !strcmp(u, "viewonly")))
                    td.usage |= MTLTextureUsagePixelFormatView;
                td.storageMode = MTLStorageModePrivate;
                td.placementSparsePageSize = MTLSparsePageSize64;
                id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
                MTL4UpdateSparseTextureMappingOperation op = {};
                op.mode = MTLSparseTextureMappingModeMap;
                op.textureRegion = MTLRegionMake2D(t[0], t[1], 1, 1);
                op.heapOffset = 0;
                [q4 updateTextureMappings:tex heap:heap operations:&op count:1];
                [q4 signalEvent:ev value:++evv];
                memset(dst.contents, 0, dst.length);
                id<MTLCommandBuffer> cb = [q commandBuffer];
                [cb encodeWaitForEvent:ev value:evv];
                id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
                [b copyFromBuffer:src sourceOffset:0 sourceBytesPerRow:pitch sourceBytesPerImage:pitch * by
                       sourceSize:MTLSizeMake(W, H, 1) toTexture:tex destinationSlice:0 destinationLevel:0
                destinationOrigin:MTLOriginMake(0, 0, 0)];
                [b copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1)
                          toBuffer:dst destinationOffset:0 destinationBytesPerRow:pitch destinationBytesPerImage:pitch * by];
                [b endEncoding];
                [cb commit];
                [cb waitUntilCompleted];
                const uint8_t *p = (const uint8_t *)dst.contents;
                unsigned minx = ~0u, miny = ~0u, maxx = 0, maxy = 0, kept = 0;
                for (unsigned y = 0; y < by; y++)
                    for (unsigned x = 0; x < bx; x++)
                        if (p[y * pitch + x * f.bytes] == 0xA5)
                        {
                            kept++;
                            minx = std::min(minx, x); maxx = std::max(maxx, x);
                            miny = std::min(miny, y); maxy = std::max(maxy, y);
                        }
                if (t[0] == 0 && t[1] == 0)
                    printf("  sparse tier %d\n", (int)tex.sparseTextureTier);
                if (getenv("SPARSE_MAP") && f.bw == 4)
                {
                    // One character per 8x8 blocks (32x32 texels) of the first 1024x1024 texels: '#' kept, '+' some, '.' none.
                    for (unsigned cy = 0; cy < std::min(by, 256u); cy += 8)
                    {
                        printf("    ");
                        for (unsigned cx = 0; cx < std::min(bx, 256u); cx += 8)
                        {
                            unsigned n = 0;
                            for (unsigned y = cy; y < cy + 8; y++)
                                for (unsigned x = cx; x < cx + 8; x++)
                                    n += p[y * pitch + x * f.bytes] == 0xA5;
                            putchar(n == 64 ? '#' : n ? '+' : '.');
                        }
                        putchar('\n');
                    }
                }
                printf("  tile (%d,%d) mapped: %u blocks kept data (%u bytes), texels x %u-%u y %u-%u\n", t[0], t[1], kept,
                       kept * f.bytes, kept ? minx * f.bw : 0, kept ? (maxx + 1) * f.bw - 1 : 0,
                       kept ? miny * f.bw : 0, kept ? (maxy + 1) * f.bw - 1 : 0);
            }
        }
        return 0;
    }
}
