/*
 * d3d9_draw.c — D3D9 rendering checks with readback (GetRenderTargetData), no presentation:
 * clears and draws into an offscreen render target and the back buffer, and fixed-function and
 * shader draws, so a wrong frame can be told apart from a wrong present.
 *
 *   d3d9_draw.exe <test>|list|all
 *
 * Build: x86_64-w64-mingw32-clang -O2 d3d9_draw.c -o d3d9_draw.exe -ld3d9 -ld3dcompiler_47 -luuid -luser32
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>

#define SIZE 64
static IDirect3DDevice9 *dev;
static const char *failure;
static char failbuf[256];
#define FAIL(...) do { snprintf(failbuf, sizeof(failbuf), __VA_ARGS__); failure = failbuf; goto done; } while (0)
#define RELEASE(p) do { if (p) { IUnknown_Release((IUnknown *)(p)); (p) = NULL; } } while (0)

#define ORANGE 0xffff8020u
#define RED    0xffff0000u
#define GREEN  0xff00ff00u
#define BLUE   0xff0000ffu

static int close_to(DWORD a, DWORD b) {
    for (int i = 0; i < 24; i += 8) { int d = (int)((a >> i) & 0xff) - (int)((b >> i) & 0xff); if (d > 2 || d < -2) return 0; }
    return 1;
}

/* The pixel at (x, y) of a render target surface, as A8R8G8B8 with alpha forced (X8R8G8B8 targets) */
static DWORD read_pixel(IDirect3DSurface9 *rt, int x, int y) {
    D3DSURFACE_DESC d; IDirect3DSurface9 *sys = NULL; D3DLOCKED_RECT l; DWORD v = 0xdeadbeef;
    IDirect3DSurface9_GetDesc(rt, &d);
    if (FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, NULL))) return v;
    if (SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(dev, rt, sys)) &&
        SUCCEEDED(IDirect3DSurface9_LockRect(sys, &l, NULL, D3DLOCK_READONLY))) {
        v = *(DWORD *)((BYTE *)l.pBits + y * l.Pitch + x * 4) | 0xff000000u;
        IDirect3DSurface9_UnlockRect(sys);
    }
    RELEASE(sys);
    return v;
}

static IDirect3DSurface9 *offscreen_rt(void) {
    IDirect3DSurface9 *rt = NULL;
    IDirect3DDevice9_CreateRenderTarget(dev, SIZE, SIZE, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
    return rt;
}

/* Clear an offscreen render target */
static int test_clear_rt(void) {
    IDirect3DSurface9 *rt = offscreen_rt(); DWORD v;
    if (!rt) FAIL("CreateRenderTarget");
    IDirect3DDevice9_SetRenderTarget(dev, 0, rt);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, ORANGE, 1.0f, 0);
    v = read_pixel(rt, 32, 32);
    if (!close_to(v, ORANGE)) FAIL("pixel %08lx, want %08x", (unsigned long)v, ORANGE);
done:
    RELEASE(rt);
    return !failure;
}

/* Clear the back buffer, no present */
static int test_clear_backbuffer(void) {
    IDirect3DSurface9 *bb = NULL; DWORD v;
    if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) FAIL("GetBackBuffer");
    IDirect3DDevice9_SetRenderTarget(dev, 0, bb);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, ORANGE, 1.0f, 0);
    v = read_pixel(bb, 32, 32);
    if (!close_to(v, ORANGE)) FAIL("pixel %08lx, want %08x", (unsigned long)v, ORANGE);
done:
    RELEASE(bb);
    return !failure;
}

/* Clear inside BeginScene/EndScene, as games and the smoke test do */
static int test_clear_in_scene(void) {
    IDirect3DSurface9 *rt = offscreen_rt(); DWORD v;
    if (!rt) FAIL("CreateRenderTarget");
    IDirect3DDevice9_SetRenderTarget(dev, 0, rt);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, ORANGE, 1.0f, 0);
    IDirect3DDevice9_EndScene(dev);
    v = read_pixel(rt, 32, 32);
    if (!close_to(v, ORANGE)) FAIL("pixel %08lx, want %08x", (unsigned long)v, ORANGE);
done:
    RELEASE(rt);
    return !failure;
}

/* A pre-transformed (XYZRHW) triangle with vertex colors, fixed function */
static int test_draw_fixed_function(void) {
    struct { float x, y, z, rhw; DWORD c; } tri[3] = {
        { 0, 0, 0.5f, 1, GREEN }, { SIZE * 2, 0, 0.5f, 1, GREEN }, { 0, SIZE * 2, 0.5f, 1, GREEN } };
    IDirect3DSurface9 *rt = offscreen_rt(); DWORD v;
    if (!rt) FAIL("CreateRenderTarget");
    IDirect3DDevice9_SetRenderTarget(dev, 0, rt);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, BLUE, 1.0f, 0);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, tri, sizeof(tri[0]));
    IDirect3DDevice9_EndScene(dev);
    v = read_pixel(rt, 16, 16);
    if (!close_to(v, GREEN)) FAIL("pixel %08lx, want %08x (blue: nothing drawn)", (unsigned long)v, GREEN);
done:
    RELEASE(rt);
    return !failure;
}

/* A textured quad, fixed function: texture modulated by nothing (SELECTARG1 TEXTURE) */
static int test_draw_textured(void) {
    struct { float x, y, z, rhw, u, v; } tri[3] = {
        { 0, 0, 0.5f, 1, 0, 0 }, { SIZE * 2, 0, 0.5f, 1, 2, 0 }, { 0, SIZE * 2, 0.5f, 1, 0, 2 } };
    IDirect3DSurface9 *rt = offscreen_rt(); IDirect3DTexture9 *tex = NULL; D3DLOCKED_RECT l; DWORD v;
    if (!rt) FAIL("CreateRenderTarget");
    if (FAILED(IDirect3DDevice9_CreateTexture(dev, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, NULL))) FAIL("CreateTexture");
    IDirect3DTexture9_LockRect(tex, 0, &l, NULL, 0);
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) ((DWORD *)((BYTE *)l.pBits + y * l.Pitch))[x] = RED;
    IDirect3DTexture9_UnlockRect(tex, 0);
    IDirect3DDevice9_SetRenderTarget(dev, 0, rt);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, BLUE, 1.0f, 0);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)tex);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, tri, sizeof(tri[0]));
    IDirect3DDevice9_EndScene(dev);
    IDirect3DDevice9_SetTexture(dev, 0, NULL);
    v = read_pixel(rt, 16, 16);
    if (!close_to(v, RED)) FAIL("pixel %08lx, want %08x (blue: nothing drawn)", (unsigned long)v, RED);
done:
    RELEASE(tex); RELEASE(rt);
    return !failure;
}

/* StretchRect between render targets (games' post-processing and present paths) */
static int test_stretch_rect(void) {
    IDirect3DSurface9 *a = offscreen_rt(), *b = offscreen_rt(); DWORD v;
    if (!a || !b) FAIL("CreateRenderTarget");
    IDirect3DDevice9_SetRenderTarget(dev, 0, a);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, ORANGE, 1.0f, 0);
    IDirect3DDevice9_SetRenderTarget(dev, 0, b);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, BLUE, 1.0f, 0);
    if (FAILED(IDirect3DDevice9_StretchRect(dev, a, NULL, b, NULL, D3DTEXF_POINT))) FAIL("StretchRect");
    v = read_pixel(b, 32, 32);
    if (!close_to(v, ORANGE)) FAIL("pixel %08lx, want %08x", (unsigned long)v, ORANGE);
done:
    RELEASE(a); RELEASE(b);
    return !failure;
}


/* Render-target feedback: a draw samples the texture it renders to (GTA IV's HDR and exposure passes).
 * D3D9 games get the texture's previous contents; on tile-based GPUs reading it directly gives tile lines. */
static int test_rt_feedback(void) {
    struct { float x, y, z, rhw; DWORD c; float u, v; } tri[3] = {
        { -0.5f, -0.5f, 0.5f, 1, 0xff808080u, 0, 0 }, { SIZE * 2 - 0.5f, -0.5f, 0.5f, 1, 0xff808080u, 2, 0 },
        { -0.5f, SIZE * 2 - 0.5f, 0.5f, 1, 0xff808080u, 0, 2 } };
    IDirect3DTexture9 *tex = NULL; IDirect3DSurface9 *rt = NULL; int bad = 0; DWORD first = 0, v = 0;
    if (FAILED(IDirect3DDevice9_CreateTexture(dev, SIZE, SIZE, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, NULL))) FAIL("CreateTexture");
    IDirect3DTexture9_GetSurfaceLevel(tex, 0, &rt);
    IDirect3DDevice9_SetRenderTarget(dev, 0, rt);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, RED, 1.0f, 0);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)tex);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, tri, sizeof(tri[0]));
    IDirect3DDevice9_EndScene(dev);
    IDirect3DDevice9_SetTexture(dev, 0, NULL);
    /* red sampled, modulated by 0.5 gray: about (0x80, 0, 0) everywhere */
    for (int y = 0; y < SIZE; y += 8) for (int x = 0; x < SIZE; x += 8) {
        v = read_pixel(rt, x, y);
        if (!close_to(v, 0xff800000u)) { if (!bad++) first = v; }
    }
    if (bad) FAIL("%d of 64 sampled pixels wrong, first %08lx, want ff800000", bad, (unsigned long)first);
done:
    RELEASE(rt); RELEASE(tex);
    return !failure;
}


/* Shadow map: depth 0.5 rendered into a D24S8 depth texture, then sampled by a pixel shader with tex2Dproj,
 * which on D3D9 hardware compares (returns 1 where the reference depth passes, 0 where it does not) */
static int test_shadow_map(void) {
    static const char src[] =
        "sampler2D s : register(s0);\n"
        "float4 main(float2 uv : TEXCOORD0) : COLOR { return float4(tex2Dproj(s, float4(uv, 0.4, 1)).r, tex2Dproj(s, float4(uv, 0.6, 1)).r, 0, 1); }\n";
    struct { float x, y, z, rhw; float u, v; } tri[3] = {
        { -0.5f, -0.5f, 0.5f, 1, 0, 0 }, { SIZE * 2 - 0.5f, -0.5f, 0.5f, 1, 2, 0 }, { -0.5f, SIZE * 2 - 0.5f, 0.5f, 1, 0, 2 } };
    IDirect3DTexture9 *dtex = NULL; IDirect3DSurface9 *ds = NULL, *rt = offscreen_rt(), *olddepth = NULL;
    IDirect3DPixelShader9 *ps = NULL; ID3DBlob *blob = NULL, *err = NULL; DWORD v;
    if (FAILED(IDirect3DDevice9_CreateTexture(dev, SIZE, SIZE, 1, D3DUSAGE_DEPTHSTENCIL, D3DFMT_D24S8, D3DPOOL_DEFAULT, &dtex, NULL))) FAIL("depth texture");
    if (FAILED(D3DCompile(src, strlen(src), NULL, NULL, NULL, "main", "ps_2_0", 0, 0, &blob, &err))) FAIL("compile: %s", err ? (char *)ID3D10Blob_GetBufferPointer(err) : "?");
    IDirect3DDevice9_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(blob), &ps);
    IDirect3DTexture9_GetSurfaceLevel(dtex, 0, &ds);
    IDirect3DDevice9_GetDepthStencilSurface(dev, &olddepth);
    /* depth pass: 0.5 everywhere, no color */
    IDirect3DDevice9_SetRenderTarget(dev, 0, rt);
    IDirect3DDevice9_SetDepthStencilSurface(dev, ds);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, BLUE, 1.0f, 0);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZFUNC, D3DCMP_ALWAYS);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, tri, sizeof(tri[0]));
    /* lookup pass */
    IDirect3DDevice9_SetDepthStencilSurface(dev, olddepth);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)dtex);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetPixelShader(dev, ps);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, tri, sizeof(tri[0]));
    IDirect3DDevice9_EndScene(dev);
    IDirect3DDevice9_SetPixelShader(dev, NULL);
    IDirect3DDevice9_SetTexture(dev, 0, NULL);
    v = read_pixel(rt, 16, 16);
    /* D3D9 compares texel <= ref as LESS_EQUAL: 0.4 <= 0.5 passes (r = 1), 0.6 fails (g = 0) */
    if (!close_to(v, 0xffff0000u)) FAIL("pixel %08lx, want ffff0000 (r: compare with 0.4 passes, g: with 0.6 fails)", (unsigned long)v);
done:
    if (olddepth) IDirect3DDevice9_SetDepthStencilSurface(dev, olddepth);
    RELEASE(olddepth); RELEASE(ps); RELEASE(blob); RELEASE(err); RELEASE(ds); RELEASE(dtex); RELEASE(rt);
    return !failure;
}

static const struct { const char *name; int (*run)(void); } tests[] = {
    { "clear_rt", test_clear_rt }, { "clear_backbuffer", test_clear_backbuffer }, { "clear_in_scene", test_clear_in_scene },
    { "draw_fixed_function", test_draw_fixed_function }, { "draw_textured", test_draw_textured }, { "stretch_rect", test_stretch_rect }, { "rt_feedback", test_rt_feedback }, { "shadow_map", test_shadow_map },
};

int main(int argc, char **argv) {
    IDirect3D9 *d3d; D3DPRESENT_PARAMETERS pp = { 0 }; HWND hwnd; int all, failed = 0;
    if (argc < 2) { fprintf(stderr, "usage: d3d9_draw.exe <test>|list|all\n"); return 2; }
    if (!strcmp(argv[1], "list")) { for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) printf("%s\n", tests[i].name); return 0; }
    hwnd = CreateWindowA("STATIC", "d3d9 draw", WS_POPUP, 0, 0, SIZE, SIZE, NULL, NULL, NULL, NULL);   /* never shown */
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = SIZE; pp.BackBufferHeight = SIZE; pp.hDeviceWindow = hwnd;
    pp.EnableAutoDepthStencil = TRUE; pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    if (!d3d || FAILED(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev))) {
        printf("FAIL device\n"); return 1;
    }
    all = !strcmp(argv[1], "all");
    for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        if (!all && strcmp(argv[1], tests[i].name)) continue;
        failure = NULL;
        if (tests[i].run()) printf("PASS %s\n", tests[i].name); else { printf("FAIL %s: %s\n", tests[i].name, failure); failed++; }
        fflush(stdout);
    }
    return failed ? 1 : 0;
}
