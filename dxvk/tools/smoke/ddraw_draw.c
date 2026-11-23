/*
 * ddraw_draw.c — D3D7/6/5/3 rendering checks through d7vk's ddraw.dll, with readback (Lock), no presentation.
 * Each test draws into an offscreen 3D surface and reads pixels back, so a wrong frame can be told apart
 * from a wrong present. d7vk proxies DirectDraw to the real ddraw, which it loads as ddraw_.dll.
 *
 *   ddraw_draw.exe <test>|list|all
 *
 * Build (32-bit, as the games are):
 *   i686-w64-mingw32-clang -O2 ddraw_draw.c -o ddraw_draw.exe -lddraw -ldxguid -luuid -luser32
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>
#include <stdio.h>
#include <string.h>

#define SIZE 64
static HWND hwnd;
static const char *failure;
static char failbuf[256];
#define FAIL(...) do { snprintf(failbuf, sizeof(failbuf), __VA_ARGS__); failure = failbuf; goto done; } while (0)
#define CHECK(hr, what) do { HRESULT h_ = (hr); if (FAILED(h_)) FAIL("%s: %08lx", what, (unsigned long)h_); } while (0)
#define RELEASE(p) do { if (p) { IUnknown_Release((IUnknown *)(p)); (p) = NULL; } } while (0)

#define RED    0xffff0000u
#define GREEN  0xff00ff00u
#define BLUE   0xff0000ffu
#define BLACK  0xff000000u

static int close_to(DWORD a, DWORD b, int tol) {
    for (int i = 0; i < 24; i += 8) { int d = (int)((a >> i) & 0xff) - (int)((b >> i) & 0xff); if (d > tol || d < -tol) return 0; }
    return 1;
}

static void set_rgb_format(DDPIXELFORMAT *pf, int bits) {
    memset(pf, 0, sizeof(*pf));
    pf->dwSize = sizeof(*pf);
    pf->dwFlags = DDPF_RGB;
    pf->dwRGBBitCount = bits;
    if (bits == 16) { pf->dwRBitMask = 0xf800; pf->dwGBitMask = 0x07e0; pf->dwBBitMask = 0x001f; }
    else            { pf->dwRBitMask = 0xff0000; pf->dwGBitMask = 0x00ff00; pf->dwBBitMask = 0x0000ff; }
}

static void set_z_format(DDPIXELFORMAT *pf, int bits) {
    memset(pf, 0, sizeof(*pf));
    pf->dwSize = sizeof(*pf);
    pf->dwFlags = DDPF_ZBUFFER;
    pf->dwZBufferBitDepth = bits;
    pf->dwZBitMask = bits == 16 ? 0xffff : 0xffffff;
}

/* Expands a locked pixel to X8R8G8B8 */
static DWORD expand(const BYTE *p, int bits) {
    if (bits == 16) {
        WORD w = *(const WORD *)p;
        DWORD r = (w >> 11) & 0x1f, g = (w >> 5) & 0x3f, b = w & 0x1f;
        return 0xff000000u | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
    }
    return *(const DWORD *)p | 0xff000000u;
}

/* The pixel at (x, y) of any surface version, through its IDirectDrawSurface (v1) interface */
static DWORD read_pixel(IUnknown *surf, int x, int y) {
    IDirectDrawSurface *s1 = NULL; DDSURFACEDESC d = { sizeof(d) }; DWORD v = 0xdeadbeef;
    if (FAILED(IUnknown_QueryInterface(surf, &IID_IDirectDrawSurface, (void **)&s1))) return v;
    if (SUCCEEDED(IDirectDrawSurface_Lock(s1, NULL, &d, DDLOCK_WAIT | DDLOCK_READONLY, NULL))) {
        v = expand((BYTE *)d.lpSurface + y * d.lPitch + x * (d.ddpfPixelFormat.dwRGBBitCount / 8), d.ddpfPixelFormat.dwRGBBitCount);
        IDirectDrawSurface_Unlock(s1, NULL);
    }
    RELEASE(s1);
    return v;
}

static D3DTLVERTEX tlv(float x, float y, float z, DWORD c, float u, float v) {
    D3DTLVERTEX t; memset(&t, 0, sizeof(t));
    t.sx = x; t.sy = y; t.sz = z; t.rhw = 1.0f; t.color = c; t.specular = 0xff000000u; t.tu = u; t.tv = v;
    return t;
}

/* A triangle that covers the whole SIZE x SIZE target */
static void fullscreen_tri(D3DTLVERTEX *v, DWORD c, float z) {
    v[0] = tlv(0.0f, 0.0f, z, c, 0.0f, 0.0f);
    v[1] = tlv(2.0f * SIZE, 0.0f, z, c, 2.0f, 0.0f);
    v[2] = tlv(0.0f, 2.0f * SIZE, z, c, 0.0f, 2.0f);
}

/* ---------------------------------------------------------------- D3D7 */

struct d7 { IDirectDraw7 *dd; IDirect3D7 *d3d; IDirectDrawSurface7 *rt, *z; IDirect3DDevice7 *dev; };

static void d7_release(struct d7 *c) {
    RELEASE(c->dev); RELEASE(c->z); RELEASE(c->rt); RELEASE(c->d3d); RELEASE(c->dd);
}

static int d7_create(struct d7 *c, int bits, int zbits) {
    DDSURFACEDESC2 sd; D3DVIEWPORT7 vp = { 0, 0, SIZE, SIZE, 0.0f, 1.0f };
    memset(c, 0, sizeof(*c));
    CHECK(DirectDrawCreateEx(NULL, (void **)&c->dd, &IID_IDirectDraw7, NULL), "DirectDrawCreateEx");
    CHECK(IDirectDraw7_SetCooperativeLevel(c->dd, hwnd, DDSCL_NORMAL), "SetCooperativeLevel");
    CHECK(IDirectDraw7_QueryInterface(c->dd, &IID_IDirect3D7, (void **)&c->d3d), "QI IDirect3D7");

    memset(&sd, 0, sizeof(sd)); sd.dwSize = sizeof(sd);
    sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    sd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY;
    sd.dwWidth = SIZE; sd.dwHeight = SIZE;
    set_rgb_format(&sd.ddpfPixelFormat, bits);
    CHECK(IDirectDraw7_CreateSurface(c->dd, &sd, &c->rt, NULL), "CreateSurface rt");

    if (zbits) {
        memset(&sd, 0, sizeof(sd)); sd.dwSize = sizeof(sd);
        sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
        sd.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | DDSCAPS_VIDEOMEMORY;
        sd.dwWidth = SIZE; sd.dwHeight = SIZE;
        set_z_format(&sd.ddpfPixelFormat, zbits);
        CHECK(IDirectDraw7_CreateSurface(c->dd, &sd, &c->z, NULL), "CreateSurface z");
        CHECK(IDirectDrawSurface7_AddAttachedSurface(c->rt, c->z), "AddAttachedSurface z");
    }

    CHECK(IDirect3D7_CreateDevice(c->d3d, &IID_IDirect3DHALDevice, c->rt, &c->dev), "CreateDevice");
    CHECK(IDirect3DDevice7_SetViewport(c->dev, &vp), "SetViewport");
    IDirect3DDevice7_SetRenderState(c->dev, D3DRENDERSTATE_ZENABLE, zbits ? D3DZB_TRUE : D3DZB_FALSE);
    IDirect3DDevice7_SetRenderState(c->dev, D3DRENDERSTATE_LIGHTING, FALSE);
    IDirect3DDevice7_SetRenderState(c->dev, D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
done:
    return !failure;
}

static int d7_draw(struct d7 *c, DWORD color) {
    D3DTLVERTEX v[3]; fullscreen_tri(v, color, 0.5f);
    CHECK(IDirect3DDevice7_BeginScene(c->dev), "BeginScene");
    CHECK(IDirect3DDevice7_DrawPrimitive(c->dev, D3DPT_TRIANGLELIST, D3DFVF_TLVERTEX, v, 3, 0), "DrawPrimitive");
    CHECK(IDirect3DDevice7_EndScene(c->dev), "EndScene");
done:
    return !failure;
}

/* Clear a 32-bit render target */
static int test_d3d7_clear(void) {
    struct d7 c; DWORD v;
    if (!d7_create(&c, 32, 0)) goto done;
    CHECK(IDirect3DDevice7_Clear(c.dev, 0, NULL, D3DCLEAR_TARGET, RED, 1.0f, 0), "Clear");
    v = read_pixel((IUnknown *)c.rt, 32, 32);
    if (!close_to(v, RED, 2)) FAIL("pixel %08lx, want %08x", (unsigned long)v, RED);
done:
    d7_release(&c);
    return !failure;
}

/* Pre-transformed (TL) vertices, the usual D3D7 path for UI and 2D */
static int test_d3d7_draw_tl(void) {
    struct d7 c; DWORD v;
    if (!d7_create(&c, 32, 16)) goto done;
    CHECK(IDirect3DDevice7_Clear(c.dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, RED, 1.0f, 0), "Clear");
    if (!d7_draw(&c, GREEN)) goto done;
    v = read_pixel((IUnknown *)c.rt, 16, 16);
    if (!close_to(v, GREEN, 2)) FAIL("pixel %08lx, want %08x", (unsigned long)v, GREEN);
done:
    d7_release(&c);
    return !failure;
}

/* A 16-bit (R5G6B5) render target, the common D3D3-7 back buffer format */
static int test_d3d7_rgb565(void) {
    struct d7 c; DWORD v;
    if (!d7_create(&c, 16, 16)) goto done;
    CHECK(IDirect3DDevice7_Clear(c.dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, RED, 1.0f, 0), "Clear");
    v = read_pixel((IUnknown *)c.rt, 8, 8);
    if (!close_to(v, RED, 8)) FAIL("after clear: pixel %08lx, want %08x", (unsigned long)v, RED);
    if (!d7_draw(&c, BLUE)) goto done;
    v = read_pixel((IUnknown *)c.rt, 16, 16);
    if (!close_to(v, BLUE, 8)) FAIL("after draw: pixel %08lx, want %08x", (unsigned long)v, BLUE);
done:
    d7_release(&c);
    return !failure;
}

/* Lock a 16-bit Z buffer after clearing it to 0.5: D3D7 lets games read depth */
static int test_d3d7_zbuffer_lock(void) {
    struct d7 c; DDSURFACEDESC2 d; WORD z;
    memset(&d, 0, sizeof(d)); d.dwSize = sizeof(d);
    if (!d7_create(&c, 32, 16)) goto done;
    CHECK(IDirect3DDevice7_Clear(c.dev, 0, NULL, D3DCLEAR_ZBUFFER, 0, 0.5f, 0), "Clear");
    CHECK(IDirectDrawSurface7_Lock(c.z, NULL, &d, DDLOCK_WAIT | DDLOCK_READONLY, NULL), "Lock z");
    z = *(WORD *)((BYTE *)d.lpSurface + 32 * d.lPitch + 32 * 2);
    IDirectDrawSurface7_Unlock(c.z, NULL);
    if (z < 0x7f00 || z > 0x8100) FAIL("depth %04x, want about 7fff", z);
done:
    d7_release(&c);
    return !failure;
}

/* Source color keying on a texture (DDCKEY_SRCBLT + COLORKEYENABLE): keyed texels are not drawn */
static int test_d3d7_colorkey(void) {
    struct d7 c; IDirectDrawSurface7 *tex = NULL; DDSURFACEDESC2 sd; DDCOLORKEY ck = { 0x0000ff, 0x0000ff };
    D3DTLVERTEX q[4]; DWORD l, r;
    if (!d7_create(&c, 32, 0)) goto done;

    memset(&sd, 0, sizeof(sd)); sd.dwSize = sizeof(sd);
    sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    sd.ddsCaps.dwCaps = DDSCAPS_TEXTURE; sd.ddsCaps.dwCaps2 = DDSCAPS2_TEXTUREMANAGE;
    sd.dwWidth = 4; sd.dwHeight = 4;
    set_rgb_format(&sd.ddpfPixelFormat, 32);
    CHECK(IDirectDraw7_CreateSurface(c.dd, &sd, &tex, NULL), "CreateSurface texture");
    memset(&sd, 0, sizeof(sd)); sd.dwSize = sizeof(sd);
    CHECK(IDirectDrawSurface7_Lock(tex, NULL, &sd, DDLOCK_WAIT | DDLOCK_WRITEONLY, NULL), "Lock texture");
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)   /* left half: the key (blue), right half: green */
            ((DWORD *)((BYTE *)sd.lpSurface + y * sd.lPitch))[x] = x < 2 ? 0x0000ff : 0x00ff00;
    IDirectDrawSurface7_Unlock(tex, NULL);
    CHECK(IDirectDrawSurface7_SetColorKey(tex, DDCKEY_SRCBLT, &ck), "SetColorKey");

    CHECK(IDirect3DDevice7_Clear(c.dev, 0, NULL, D3DCLEAR_TARGET, RED, 1.0f, 0), "Clear");
    IDirect3DDevice7_SetTexture(c.dev, 0, tex);
    IDirect3DDevice7_SetTextureStageState(c.dev, 0, D3DTSS_MINFILTER, D3DTFN_POINT);
    IDirect3DDevice7_SetTextureStageState(c.dev, 0, D3DTSS_MAGFILTER, D3DTFG_POINT);
    IDirect3DDevice7_SetTextureStageState(c.dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    IDirect3DDevice7_SetTextureStageState(c.dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice7_SetRenderState(c.dev, D3DRENDERSTATE_COLORKEYENABLE, TRUE);
    q[0] = tlv(0, 0, 0.5f, 0xffffffff, 0, 0);      q[1] = tlv(SIZE, 0, 0.5f, 0xffffffff, 1, 0);
    q[2] = tlv(0, SIZE, 0.5f, 0xffffffff, 0, 1);   q[3] = tlv(SIZE, SIZE, 0.5f, 0xffffffff, 1, 1);
    CHECK(IDirect3DDevice7_BeginScene(c.dev), "BeginScene");
    CHECK(IDirect3DDevice7_DrawPrimitive(c.dev, D3DPT_TRIANGLESTRIP, D3DFVF_TLVERTEX, q, 4, 0), "DrawPrimitive");
    CHECK(IDirect3DDevice7_EndScene(c.dev), "EndScene");
    IDirect3DDevice7_SetTexture(c.dev, 0, NULL);
    l = read_pixel((IUnknown *)c.rt, 8, 32);
    r = read_pixel((IUnknown *)c.rt, 56, 32);
    if (!close_to(l, RED, 2))   FAIL("keyed texel drew %08lx, want the background %08x", (unsigned long)l, RED);
    if (!close_to(r, GREEN, 2)) FAIL("unkeyed texel %08lx, want %08x", (unsigned long)r, GREEN);
done:
    RELEASE(tex);
    d7_release(&c);
    return !failure;
}

/* ---------------------------------------------------------------- D3D6 */

static int test_d3d6_draw(void) {
    IDirectDraw *dd1 = NULL; IDirectDraw4 *dd = NULL; IDirect3D3 *d3d = NULL; IDirectDrawSurface4 *rt = NULL;
    IDirect3DDevice3 *dev = NULL; IDirect3DViewport3 *vp = NULL; DDSURFACEDESC2 sd;
    D3DVIEWPORT2 v = { sizeof(v), 0, 0, SIZE, SIZE, -1.0f, 1.0f, 2.0f, 2.0f, 0.0f, 1.0f };
    D3DRECT rect = { 0, 0, SIZE, SIZE }; D3DTLVERTEX tri[3]; DWORD px;
    CHECK(DirectDrawCreate(NULL, &dd1, NULL), "DirectDrawCreate");
    CHECK(IDirectDraw_QueryInterface(dd1, &IID_IDirectDraw4, (void **)&dd), "QI IDirectDraw4");
    CHECK(IDirectDraw4_SetCooperativeLevel(dd, hwnd, DDSCL_NORMAL), "SetCooperativeLevel");
    CHECK(IDirectDraw4_QueryInterface(dd, &IID_IDirect3D3, (void **)&d3d), "QI IDirect3D3");
    memset(&sd, 0, sizeof(sd)); sd.dwSize = sizeof(sd);
    sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    sd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY;
    sd.dwWidth = SIZE; sd.dwHeight = SIZE; set_rgb_format(&sd.ddpfPixelFormat, 16);
    CHECK(IDirectDraw4_CreateSurface(dd, &sd, &rt, NULL), "CreateSurface rt");
    CHECK(IDirect3D3_CreateDevice(d3d, &IID_IDirect3DHALDevice, rt, &dev, NULL), "CreateDevice");
    CHECK(IDirect3D3_CreateViewport(d3d, &vp, NULL), "CreateViewport");
    CHECK(IDirect3DDevice3_AddViewport(dev, vp), "AddViewport");
    CHECK(IDirect3DViewport3_SetViewport2(vp, &v), "SetViewport2");
    CHECK(IDirect3DDevice3_SetCurrentViewport(dev, vp), "SetCurrentViewport");
    CHECK(IDirect3DViewport3_Clear2(vp, 1, &rect, D3DCLEAR_TARGET, RED, 1.0f, 0), "Clear2");
    IDirect3DDevice3_SetRenderState(dev, D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
    fullscreen_tri(tri, GREEN, 0.5f);
    CHECK(IDirect3DDevice3_BeginScene(dev), "BeginScene");
    CHECK(IDirect3DDevice3_DrawPrimitive(dev, D3DPT_TRIANGLELIST, D3DFVF_TLVERTEX, tri, 3, 0), "DrawPrimitive");
    CHECK(IDirect3DDevice3_EndScene(dev), "EndScene");
    px = read_pixel((IUnknown *)rt, 16, 16);
    if (!close_to(px, GREEN, 8)) FAIL("pixel %08lx, want %08x", (unsigned long)px, GREEN);
done:
    RELEASE(vp); RELEASE(dev); RELEASE(rt); RELEASE(d3d); RELEASE(dd); RELEASE(dd1);
    return !failure;
}

/* ---------------------------------------------------------------- D3D5 */

/* Creates a 16-bit offscreen 3D surface on a v1 DirectDraw object and fills it with fill (565) */
static int create_rt1(IDirectDraw *dd, IDirectDrawSurface **rt, WORD fill) {
    DDSURFACEDESC sd; DDBLTFX fx;
    memset(&sd, 0, sizeof(sd)); sd.dwSize = sizeof(sd);
    sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    sd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY;
    sd.dwWidth = SIZE; sd.dwHeight = SIZE; set_rgb_format(&sd.ddpfPixelFormat, 16);
    CHECK(IDirectDraw_CreateSurface(dd, &sd, rt, NULL), "CreateSurface rt");
    memset(&fx, 0, sizeof(fx)); fx.dwSize = sizeof(fx); fx.dwFillColor = fill;
    CHECK(IDirectDrawSurface_Blt(*rt, NULL, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx), "Blt colorfill");
done:
    return !failure;
}

static int test_d3d5_draw(void) {
    IDirectDraw *dd = NULL; IDirect3D2 *d3d = NULL; IDirectDrawSurface *rt = NULL;
    IDirect3DDevice2 *dev = NULL; IDirect3DViewport2 *vp = NULL;
    D3DVIEWPORT2 v = { sizeof(v), 0, 0, SIZE, SIZE, -1.0f, 1.0f, 2.0f, 2.0f, 0.0f, 1.0f };
    D3DTLVERTEX tri[3]; DWORD px;
    CHECK(DirectDrawCreate(NULL, &dd, NULL), "DirectDrawCreate");
    CHECK(IDirectDraw_SetCooperativeLevel(dd, hwnd, DDSCL_NORMAL), "SetCooperativeLevel");
    CHECK(IDirectDraw_QueryInterface(dd, &IID_IDirect3D2, (void **)&d3d), "QI IDirect3D2");
    if (!create_rt1(dd, &rt, 0xf800)) goto done;   /* red */
    CHECK(IDirect3D2_CreateDevice(d3d, &IID_IDirect3DHALDevice, rt, &dev), "CreateDevice");
    CHECK(IDirect3D2_CreateViewport(d3d, &vp, NULL), "CreateViewport");
    CHECK(IDirect3DDevice2_AddViewport(dev, vp), "AddViewport");
    CHECK(IDirect3DViewport2_SetViewport2(vp, &v), "SetViewport2");
    CHECK(IDirect3DDevice2_SetCurrentViewport(dev, vp), "SetCurrentViewport");
    IDirect3DDevice2_SetRenderState(dev, D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
    fullscreen_tri(tri, BLUE, 0.5f);
    CHECK(IDirect3DDevice2_BeginScene(dev), "BeginScene");
    CHECK(IDirect3DDevice2_DrawPrimitive(dev, D3DPT_TRIANGLELIST, D3DVT_TLVERTEX, tri, 3, D3DDP_WAIT), "DrawPrimitive");
    CHECK(IDirect3DDevice2_EndScene(dev), "EndScene");
    px = read_pixel((IUnknown *)rt, 16, 16);
    if (!close_to(px, BLUE, 8)) FAIL("pixel %08lx, want %08x", (unsigned long)px, BLUE);
done:
    RELEASE(vp); RELEASE(dev); RELEASE(rt); RELEASE(d3d); RELEASE(dd);
    return !failure;
}

/* ---------------------------------------------------------------- D3D3 (execute buffers) */

static int test_d3d3_execute_buffer(void) {
    IDirectDraw *dd = NULL; IDirect3D *d3d = NULL; IDirectDrawSurface *rt = NULL;
    IDirect3DDevice *dev = NULL; IDirect3DViewport *vp = NULL; IDirect3DExecuteBuffer *eb = NULL;
    D3DVIEWPORT v; D3DEXECUTEBUFFERDESC ebd; D3DEXECUTEDATA ed; DWORD px;
    BYTE *p, *ins;
    CHECK(DirectDrawCreate(NULL, &dd, NULL), "DirectDrawCreate");
    CHECK(IDirectDraw_SetCooperativeLevel(dd, hwnd, DDSCL_NORMAL), "SetCooperativeLevel");
    CHECK(IDirectDraw_QueryInterface(dd, &IID_IDirect3D, (void **)&d3d), "QI IDirect3D");
    if (!create_rt1(dd, &rt, 0xf800)) goto done;   /* red */
    CHECK(IDirectDrawSurface_QueryInterface(rt, &IID_IDirect3DHALDevice, (void **)&dev), "QI IDirect3DHALDevice");
    CHECK(IDirect3D_CreateViewport(d3d, &vp, NULL), "CreateViewport");
    CHECK(IDirect3DDevice_AddViewport(dev, vp), "AddViewport");
    memset(&v, 0, sizeof(v)); v.dwSize = sizeof(v);
    v.dwWidth = SIZE; v.dwHeight = SIZE; v.dvScaleX = SIZE / 2.0f; v.dvScaleY = SIZE / 2.0f;
    v.dvMaxX = 1.0f; v.dvMaxY = 1.0f; v.dvMinZ = 0.0f; v.dvMaxZ = 1.0f;
    CHECK(IDirect3DViewport_SetViewport(vp, &v), "SetViewport");

    memset(&ebd, 0, sizeof(ebd)); ebd.dwSize = sizeof(ebd);
    ebd.dwFlags = D3DDEB_BUFSIZE; ebd.dwBufferSize = 1024;
    CHECK(IDirect3DDevice_CreateExecuteBuffer(dev, &ebd, &eb, NULL), "CreateExecuteBuffer");
    memset(&ebd, 0, sizeof(ebd)); ebd.dwSize = sizeof(ebd);
    CHECK(IDirect3DExecuteBuffer_Lock(eb, &ebd), "Lock execute buffer");
    p = ebd.lpData;
    fullscreen_tri((D3DTLVERTEX *)p, GREEN, 0.5f);
    ins = p + 3 * sizeof(D3DTLVERTEX);
    {
        BYTE *q = ins;
        D3DINSTRUCTION *i; D3DPROCESSVERTICES *pv; D3DSTATE *st; D3DTRIANGLE *t;
        i = (D3DINSTRUCTION *)q; i->bOpcode = D3DOP_STATERENDER; i->bSize = sizeof(D3DSTATE); i->wCount = 1; q += sizeof(*i);
        st = (D3DSTATE *)q; st->drstRenderStateType = D3DRENDERSTATE_CULLMODE; st->dwArg[0] = D3DCULL_NONE; q += sizeof(*st);
        i = (D3DINSTRUCTION *)q; i->bOpcode = D3DOP_PROCESSVERTICES; i->bSize = sizeof(D3DPROCESSVERTICES); i->wCount = 1; q += sizeof(*i);
        pv = (D3DPROCESSVERTICES *)q; memset(pv, 0, sizeof(*pv));
        pv->dwFlags = D3DPROCESSVERTICES_COPY; pv->wStart = 0; pv->wDest = 0; pv->dwCount = 3; q += sizeof(*pv);
        i = (D3DINSTRUCTION *)q; i->bOpcode = D3DOP_TRIANGLE; i->bSize = sizeof(D3DTRIANGLE); i->wCount = 1; q += sizeof(*i);
        t = (D3DTRIANGLE *)q; t->v1 = 0; t->v2 = 1; t->v3 = 2; t->wFlags = D3DTRIFLAG_EDGEENABLETRIANGLE; q += sizeof(*t);
        i = (D3DINSTRUCTION *)q; i->bOpcode = D3DOP_EXIT; i->bSize = 0; i->wCount = 0; q += sizeof(*i);
        memset(&ed, 0, sizeof(ed)); ed.dwSize = sizeof(ed);
        ed.dwVertexOffset = 0; ed.dwVertexCount = 3;
        ed.dwInstructionOffset = (DWORD)(ins - p); ed.dwInstructionLength = (DWORD)(q - ins);
    }
    CHECK(IDirect3DExecuteBuffer_Unlock(eb), "Unlock execute buffer");
    CHECK(IDirect3DExecuteBuffer_SetExecuteData(eb, &ed), "SetExecuteData");
    CHECK(IDirect3DDevice_BeginScene(dev), "BeginScene");
    CHECK(IDirect3DDevice_Execute(dev, eb, vp, D3DEXECUTE_UNCLIPPED), "Execute");
    CHECK(IDirect3DDevice_EndScene(dev), "EndScene");
    px = read_pixel((IUnknown *)rt, 16, 16);
    if (!close_to(px, GREEN, 8)) FAIL("pixel %08lx, want %08x", (unsigned long)px, GREEN);
done:
    RELEASE(eb); RELEASE(vp); RELEASE(dev); RELEASE(rt); RELEASE(d3d); RELEASE(dd);
    return !failure;
}

static const struct { const char *name; int (*run)(void); } tests[] = {
    { "d3d7_clear", test_d3d7_clear }, { "d3d7_draw_tl", test_d3d7_draw_tl }, { "d3d7_rgb565", test_d3d7_rgb565 },
    { "d3d7_zbuffer_lock", test_d3d7_zbuffer_lock }, { "d3d7_colorkey", test_d3d7_colorkey },
    { "d3d6_draw", test_d3d6_draw }, { "d3d5_draw", test_d3d5_draw }, { "d3d3_execute_buffer", test_d3d3_execute_buffer },
};

int main(int argc, char **argv) {
    int all, failed = 0;
    if (argc < 2) { fprintf(stderr, "usage: ddraw_draw.exe <test>|list|all\n"); return 2; }
    if (!strcmp(argv[1], "list")) { for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) printf("%s\n", tests[i].name); return 0; }
    hwnd = CreateWindowA("STATIC", "ddraw draw", WS_POPUP, 0, 0, SIZE, SIZE, NULL, NULL, NULL, NULL);   /* never shown */
    all = !strcmp(argv[1], "all");
    for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        if (!all && strcmp(argv[1], tests[i].name)) continue;
        failure = NULL;
        if (tests[i].run()) printf("PASS %s\n", tests[i].name); else { printf("FAIL %s: %s\n", tests[i].name, failure); failed++; }
        fflush(stdout);
    }
    return failed ? 1 : 0;
}
