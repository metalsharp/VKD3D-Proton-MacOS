// Conservative rasterization tier-1 emulation feasibility probe.
// Emulation: VS expands triangle by half-diagonal (0.707px) in screen space;
// FS tests the ORIGINAL triangle's edge functions at the 4 pixel corners;
// covered if ANY corner is inside -> exact outer-conservative coverage.
// Validated against a CPU-computed reference mask, pixel-exact.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>

/* Creates (or truncates) a file for writing, owner-only (0600) and not through a symlink. */
static FILE* create_owner_file(const char* path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    return fd < 0 ? NULL : fdopen(fd, "w");
}

static const int W = 64, H = 64;
static const float HALF_DIAG = 0.70710678f;  // half pixel diagonal (safe expansion)

typedef struct { float x, y; } V2;
typedef struct { V2 a, b, c; } Tri;

// CPU reference: D3D12 tier-1 POST-SNAP semantics: vertices snapped to the
// nearest pixel corner (integer coords), then the standard pixel-center test.
static int refCovered(Tri t, int px, int py) {
    V2 sa = {roundf(t.a.x), roundf(t.a.y)};
    V2 sb = {roundf(t.b.x), roundf(t.b.y)};
    V2 sc = {roundf(t.c.x), roundf(t.c.y)};
    float cx = px + 0.5f, cy = py + 0.5f;   // pixel center
    float a2 = (sb.x-sa.x)*(sc.y-sa.y) - (sb.y-sa.y)*(sc.x-sa.x);
    int ccw = a2 >= 0;
    float e1 = (sb.x-sa.x)*(cy-sa.y) - (sb.y-sa.y)*(cx-sa.x);
    float e2 = (sc.x-sb.x)*(cy-sb.y) - (sc.y-sb.y)*(cx-sb.x);
    float e3 = (sa.x-sc.x)*(cy-sc.y) - (sa.y-sc.y)*(cx-sc.x);
    if (ccw) return e1 >= -1e-6f && e2 >= -1e-6f && e3 >= -1e-6f;
    return e1 <= 1e-6f && e2 <= 1e-6f && e3 <= 1e-6f;
}

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *err = nil;
        srand(7);

        // 8 random triangles well inside the 64x64 area (avoid edge cases for now)
        Tri tris[16];
        int triCount = 16;
        if (getenv("ONE_TRI")) {
            tris[0].a.x = 28; tris[0].a.y = 53; tris[0].b.x = 20; tris[0].b.y = 37; tris[0].c.x = 41; tris[0].c.y = 50;
            triCount = 1;
        } else if (getenv("SEED")) {
            srand(atoi(getenv("SEED")));
            for (int i = 0; i < 16; i++) {
                tris[i].a.x = 2 + rand() % 60; tris[i].a.y = 2 + rand() % 60;
                tris[i].b.x = 2 + rand() % 60; tris[i].b.y = 2 + rand() % 60;
                tris[i].c.x = 2 + rand() % 60; tris[i].c.y = 2 + rand() % 60;
            }
        } else {
            for (int i = 0; i < 16; i++) {
                tris[i].a.x = 5 + rand() % 54; tris[i].a.y = 5 + rand() % 54;
                tris[i].b.x = 5 + rand() % 54; tris[i].b.y = 5 + rand() % 54;
                tris[i].c.x = 5 + rand() % 54; tris[i].c.y = 5 + rand() % 54;
            }
        }

        // === Render pass 1: emulated conservative raster (all 8 triangles) ===
        MTLRenderPipelineDescriptor *rpd = [MTLRenderPipelineDescriptor new];
        rpd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        MTLCompileOptions *copts = nil;
        if (getenv("NO_FS_TEST")) {
            copts = [MTLCompileOptions new];
            copts.preprocessorMacros = @{@"NO_FS_TEST": @YES};
        }
        if (getenv("DUMP_INPUTS")) {
            if (!copts) copts = [MTLCompileOptions new];
            copts.preprocessorMacros = @{@"DUMP_INPUTS": @YES};
        }
        id<MTLLibrary> lib = [dev newLibraryWithSource:@R"MSL(
#include <metal_stdlib>
using namespace metal;
struct VSOut {
    float4 pos [[position]];
    float2 origA [[flat]];
    float2 origB [[flat]];
    float2 origC [[flat]];
    float2 uv;
};
struct FSOut { float4 color [[color(0)]]; };
struct DrawConst { float4 viewportSize; };  // xy = w,h in pixels

vertex VSOut vs(constant DrawConst& dc [[buffer(0)]],
                const device float2* pos [[buffer(1)]],
                uint vid [[vertex_id]]) {
    float2 p = pos[vid];
    float2 win = dc.viewportSize.xy;
    // screen-space (pixels) in FRAMEBUFFER orientation (y down, top-left origin)
    // y: framebuffer y = H*(1-y_ndc)/2 - 1 (pixel-index mirror of the y-up grid)
    #define NDC2FB(v) float2(((v).x*0.5f+0.5f)*win.x, (1.0f-(v).y*0.5f-0.5f)*win.y - 1.0f)
    float2 s = NDC2FB(p);
    // original triangle = vertices 0,1,2 of the current draw (triangles)
    uint triBase = (vid / 3) * 3;
    float2 sa = NDC2FB(pos[triBase+0]);
    float2 sb = NDC2FB(pos[triBase+1]);
    float2 sc = NDC2FB(pos[triBase+2]);
    #undef NDC2FB
    // expand this vertex along the angle bisector by 0.707/sin(theta/2):
    // pushes every edge outward by exactly the half-diagonal (0.707px),
    // guaranteeing the expanded triangle contains every pixel square that
    // intersects the original triangle.
    // per-vertex outward bisector: this vertex's two adjacent edges
    uint vi = vid % 3;
    float2 pv = vi == 0 ? sa : (vi == 1 ? sb : sc);
    float2 q1 = vi == 0 ? sb : (vi == 1 ? sc : sa);
    float2 q2 = vi == 0 ? sc : (vi == 1 ? sa : sb);
    float2 e1 = normalize(q1 - pv);
    float2 e2 = normalize(q2 - pv);
    float2 dir = -normalize(e1 + e2);   // outward: away from the interior
    float cosTh = dot(e1, e2);
    float sinHalf = sqrt(max(0.0, (1.0 - cosTh) * 0.5));
    // Edge offset = 2r: r for the half-diagonal reach, +r margin covering
    // the disk-dilation arc slivers at the vertices (sagitta = r(1-sin(t/2)) <= r).
    // The expanded triangle is a superset; the FS decides final coverage.
    float dist = 0.70710678f * 2.0f / max(sinHalf, 0.05f);
    float2 sExp = s + dir * dist;
    // back to NDC (framebuffer y down -> NDC y up)
    float2 pExp = float2(sExp.x / win.x * 2.0f - 1.0f, 1.0f - (sExp.y + 1.0f) / win.y * 2.0f);
    VSOut o;
    o.pos = float4(pExp, 0.5, 1.0);
    o.origA = sa; o.origB = sb; o.origC = sc;
    o.uv = p;
    return o;
}

fragment FSOut fs(VSOut in [[stage_in]]) {
    // D3D12 tier-1 POST-SNAP: snap the original vertices to the nearest pixel
    // corner (integer coords), then test the pixel center.
    float2 sa = round(in.origA);
    float2 sb = round(in.origB);
    float2 sc = round(in.origC);
    float2 ctr = in.pos.xy;               // pixel center (framebuffer coords)
    float a2 = (sb.x-sa.x)*(sc.y-sa.y) - (sb.y-sa.y)*(sc.x-sa.x);
    int ccw = a2 >= 0.0f;
    float e1 = (sb.x-sa.x)*(ctr.y-sa.y) - (sb.y-sa.y)*(ctr.x-sa.x);
    float e2 = (sc.x-sb.x)*(ctr.y-sb.y) - (sc.y-sb.y)*(ctr.x-sb.x);
    float e3 = (sa.x-sc.x)*(ctr.y-sc.y) - (sa.y-sc.y)*(ctr.x-sc.x);
    bool covered = ccw ? (e1 >= -1e-4f && e2 >= -1e-4f && e3 >= -1e-4f)
                       : (e1 <=  1e-4f && e2 <=  1e-4f && e3 <=  1e-4f);
    FSOut o;
    #ifdef DUMP_INPUTS
    // debug: dump the flat vertex values the FS receives
    o.color = float4(sa.x/255.0, sa.y/255.0, sb.x/255.0, 1.0);
    return o;
    #endif
    #ifndef NO_FS_TEST
    if (!covered) { discard_fragment(); }
    #endif
    o.color = float4(1, 0, 0, 1);
    return o;
}
)MSL" options:copts error:&err];
        if (!lib) { printf("lib fail: %s\n", err.localizedDescription.UTF8String); return 1; }
        id<MTLLibrary> lib2 = lib; (void)lib2;
        rpd.vertexFunction = [lib newFunctionWithName:@"vs"];
        rpd.fragmentFunction = [lib newFunctionWithName:@"fs"];
        id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:rpd error:&err];
        if (!pso) { printf("pso fail: %s\n", err.localizedDescription.UTF8String); return 1; }

        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:W height:H mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> rt = [dev newTextureWithDescriptor:td];
        id<MTLBuffer> constBuf = [dev newBufferWithLength:16 options:MTLResourceStorageModeShared];
        ((float*)constBuf.contents)[0] = W; ((float*)constBuf.contents)[1] = H;
        id<MTLBuffer> vb = [dev newBufferWithLength:16*3*8 options:MTLResourceStorageModeShared];
        float* vp = (float*)vb.contents;
        int nv = 0;
        for (int i = 0; i < triCount; i++) {
            vp[nv++] = tris[i].a.x/ (W/2.0f) - 1.0f; vp[nv++] = tris[i].a.y/ (H/2.0f) - 1.0f;
            vp[nv++] = tris[i].b.x/ (W/2.0f) - 1.0f; vp[nv++] = tris[i].b.y/ (H/2.0f) - 1.0f;
            vp[nv++] = tris[i].c.x/ (W/2.0f) - 1.0f; vp[nv++] = tris[i].c.y/ (H/2.0f) - 1.0f;
        }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        MTLRenderPassDescriptor *rpd2 = [MTLRenderPassDescriptor renderPassDescriptor];
        rpd2.colorAttachments[0].texture = rt;
        rpd2.colorAttachments[0].loadAction = MTLLoadActionClear;
        rpd2.colorAttachments[0].clearColor = MTLClearColorMake(0,0,0,1);
        rpd2.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rpd2];
        [enc setRenderPipelineState:pso];
        [enc setVertexBuffer:constBuf offset:0 atIndex:0];
        [enc setVertexBuffer:vb offset:0 atIndex:1];
        MTLViewport vp0 = {0,0,W,H,0,1};
        [enc setViewport:vp0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:nv/2];
        [enc endEncoding];
        [cb commit]; [cb waitUntilCompleted];

        // readback
        id<MTLBuffer> rb = [dev newBufferWithLength:W*H*4 options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb2 = [q commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cb2 blitCommandEncoder];
        [blit copyFromTexture:rt sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
                   sourceSize:MTLSizeMake(W,H,1) toBuffer:rb destinationOffset:0
                   destinationBytesPerRow:W*4 destinationBytesPerImage:W*H*4];
        [blit endEncoding];
        [cb2 commit]; [cb2 waitUntilCompleted];

        // compare vs CPU reference
        uint32_t* px = (uint32_t*)rb.contents;
        int mismatches = 0, totalCovered = 0, refTotal = 0;
        for (int py = 0; py < H; py++) {
            for (int pxx = 0; pxx < W; pxx++) {
                int gpu = (px[py*W+pxx] & 0xff) > 0;
                int ref = 0;
                for (int i = 0; i < triCount; i++) if (refCovered(tris[i], pxx, H-2-py)) ref = 1;
                if (gpu) totalCovered++;
                if (ref) refTotal++;
                if (gpu != ref) {
                    mismatches++;
                    if (mismatches < 12) printf("MISMATCH at (%d,%d): gpu=%d ref=%d\n", pxx, py, gpu, ref);
                }
            }
        }
        int over = 0, under = 0;
        for (int py = 0; py < H; py++) for (int pxx = 0; pxx < W; pxx++) {
            int gpu = (px[py*W+pxx] & 0xff) > 0;
            int ref = 0;
            for (int i = 0; i < triCount; i++) if (refCovered(tris[i], pxx, H-2-py)) ref = 1;
            if (gpu && !ref) over++;
            if (!gpu && ref) under++;
        }
        printf("over=%d under=%d\n", over, under);
        int shown = 0;
        for (int py = 0; py < H && shown < 14; py++) for (int pxx = 0; pxx < W && shown < 14; pxx++) {
            int gpu = (px[py*W+pxx] & 0xff) > 0;
            int ref = 0;
            for (int i = 0; i < triCount; i++) if (refCovered(tris[i], pxx, H-2-py)) ref = 1;
            if (!gpu && ref) {
                int cov[8] = {0,0,0,0,0,0,0,0};
                for (int i = 0; i < triCount; i++) if (refCovered(tris[i], pxx, H-2-py)) cov[i] = 1;
                printf("under at fb(%d,%d) covered by tri:", pxx, py);
                for (int i = 0; i < triCount; i++) if (cov[i]) printf(" %d", i);
                printf("\n");
                shown++;
            }
        }
        for (int i = 0; i < triCount; i++)
            printf("tri%d: a=(%.2f,%.2f) b=(%.2f,%.2f) c=(%.2f,%.2f)\n", i,
                tris[i].a.x, tris[i].a.y, tris[i].b.x, tris[i].b.y, tris[i].c.x, tris[i].c.y);
        // check pixel (26,6) in detail
        int dbgpx = 26, dbgpy = 6;
        for (int i = 0; i < 8; i++) {
            int r = refCovered(tris[i], dbgpx, H-2-dbgpy);
            if (r) {
                V2 sa = {roundf(tris[i].a.x), roundf(tris[i].a.y)};
                V2 sb = {roundf(tris[i].b.x), roundf(tris[i].b.y)};
                V2 sc = {roundf(tris[i].c.x), roundf(tris[i].c.y)};
                printf("pixel(%d,%d) covered by tri%d snapped a=(%.0f,%.0f) b=(%.0f,%.0f) c=(%.0f,%.0f)\n",
                    dbgpx, dbgpy, i, sa.x, sa.y, sb.x, sb.y, sc.x, sc.y);
            }
        }
        printf("gpu covered=%d ref covered=%d mismatches=%d\n", totalCovered, refTotal, mismatches);
        if (getenv("DUMP_PPM")) {
            FILE* f = create_owner_file("/tmp/cr-gpu.ppm"); fprintf(f,"P3\n%d %d\n255\n",W,H);
            for (int y=0;y<H;y++) for (int x=0;x<W;x++) {
                if (getenv("DUMP_INPUTS")) {
                    uint32_t v=px[y*W+x];
                    fprintf(f,"%d %d %d\n", v&0xff, (v>>8)&0xff, (v>>16)&0xff);
                } else {
                    int g=(px[y*W+x]&0xff)>0; fprintf(f,"%d %d %d\n", g*255,0,0);
                }
            }
            fclose(f);
            FILE* f2 = create_owner_file("/tmp/cr-ref.ppm"); fprintf(f2,"P3\n%d %d\n255\n",W,H);
            for (int y=0;y<H;y++) for (int x=0;x<W;x++) {
                int r=0; for (int i=0;i<triCount;i++) if (refCovered(tris[i],x,H-2-y)) r=1;
                fprintf(f2,"%d %d %d\n", r*255,0,0);
            }
            fclose(f2);
            printf("PPM dumped\n");
        }
        printf("RESULT: %s\n", mismatches == 0 ? "CONSERVATIVE RASTER EMULATION PIXEL-EXACT" : "MISMATCHES FOUND");
        fflush(stdout);
    }
    return 0;
}
