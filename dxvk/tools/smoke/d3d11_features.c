/*
 * d3d11_features.c — headless D3D11 feature checks with known results, for the rendering paths a
 * deferred/HDR engine (Unity) takes: depth and stencil state changes, HDR targets read back as textures,
 * compute writes, depth read as a texture, arrays and cubes, MSAA resolve, constant buffer offsets,
 * structured buffers, MRT and blending. Each test draws offscreen and checks pixels.
 *
 *   d3d11_features.exe <test>|list|all
 *
 * Build: x86_64-w64-mingw32-clang -O2 d3d11_features.c -o d3d11_features.exe -ld3d11 -ld3dcompiler_47 -luuid
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#define SIZE 64
static const char *failure;
static char failbuf[256];
#define FAIL(...) do { snprintf(failbuf, sizeof(failbuf), __VA_ARGS__); failure = failbuf; goto done; } while (0)
#define CHECK_HR(expr, what) do { HRESULT hr_ = (expr); if (FAILED(hr_)) FAIL("%s (hr %#lx)", what, (unsigned long)hr_); } while (0)
#define RELEASE(p) do { if (p) { IUnknown_Release((IUnknown *)(p)); (p) = NULL; } } while (0)

static ID3D11Device1 *dev;
static ID3D11DeviceContext1 *ctx;

static ID3DBlob *compile(const char *src, const char *target) {
    ID3DBlob *blob = NULL, *err = NULL;
    if (FAILED(D3DCompile(src, strlen(src), NULL, NULL, NULL, "main", target, 0, 0, &blob, &err))) {
        if (err) fprintf(stderr, "%s\n", (char *)ID3D10Blob_GetBufferPointer(err));
        RELEASE(err);
        return NULL;
    }
    RELEASE(err);
    return blob;
}
static ID3D11VertexShader *vs(const char *src) {
    ID3DBlob *b = compile(src, "vs_5_0"); ID3D11VertexShader *s = NULL;
    if (b) ID3D11Device1_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s);
    RELEASE(b); return s;
}
static ID3D11PixelShader *ps(const char *src) {
    ID3DBlob *b = compile(src, "ps_5_0"); ID3D11PixelShader *s = NULL;
    if (b) ID3D11Device1_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s);
    RELEASE(b); return s;
}
static ID3D11ComputeShader *cs(const char *src) {
    ID3DBlob *b = compile(src, "cs_5_0"); ID3D11ComputeShader *s = NULL;
    if (b) ID3D11Device1_CreateComputeShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s);
    RELEASE(b); return s;
}

/* A full-screen triangle at depth z (cbuffer b0.x), color from b0.yzw... kept simple: z from b13 */
static const char vs_full[] =
    "cbuffer c : register(b13) { float4 zc; };\n"
    "float4 main(uint id : SV_VertexID) : SV_Position {\n"
    "    float2 p = float2((id << 1) & 2, id & 2) * 2.0 - 1.0;\n"
    "    return float4(p, zc.x, 1.0);\n"
    "}\n";
static const char ps_color[] =
    "cbuffer c : register(b13) { float4 zc; float4 color; };\n"
    "float4 main() : SV_Target { return color; }\n";

static ID3D11Buffer *cb13;
static void set_zc(float z, float r, float g, float b, float a) {
    float d[8] = { z, 0, 0, 0, r, g, b, a };
    ID3D11DeviceContext1_UpdateSubresource(ctx, (ID3D11Resource *)cb13, 0, NULL, d, 0, 0);
}

static ID3D11Texture2D *tex2d(DXGI_FORMAT fmt, UINT w, UINT h, UINT bind, UINT samples, UINT array, UINT misc) {
    D3D11_TEXTURE2D_DESC d = { w, h, 1, array, fmt, { samples, 0 }, D3D11_USAGE_DEFAULT, bind, 0, misc };
    ID3D11Texture2D *t = NULL;
    ID3D11Device1_CreateTexture2D(dev, &d, NULL, &t);
    return t;
}
static ID3D11RenderTargetView *rtv(ID3D11Texture2D *t) {
    ID3D11RenderTargetView *v = NULL; ID3D11Device1_CreateRenderTargetView(dev, (ID3D11Resource *)t, NULL, &v); return v;
}
static ID3D11ShaderResourceView *srv(ID3D11Resource *t, const D3D11_SHADER_RESOURCE_VIEW_DESC *d) {
    ID3D11ShaderResourceView *v = NULL; ID3D11Device1_CreateShaderResourceView(dev, t, d, &v); return v;
}

/* Reads one RGBA8 pixel of an RGBA8 texture */
static DWORD pixel(ID3D11Texture2D *t, int x, int y) {
    D3D11_TEXTURE2D_DESC d; ID3D11Texture2D *s = NULL; D3D11_MAPPED_SUBRESOURCE m; DWORD v = 0xdeadbeef;
    ID3D11Texture2D_GetDesc(t, &d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ID3D11Device1_CreateTexture2D(dev, &d, NULL, &s);
    if (!s) return v;
    ID3D11DeviceContext1_CopyResource(ctx, (ID3D11Resource *)s, (ID3D11Resource *)t);
    if (SUCCEEDED(ID3D11DeviceContext1_Map(ctx, (ID3D11Resource *)s, 0, D3D11_MAP_READ, 0, &m))) {
        v = *(DWORD *)((BYTE *)m.pData + y * m.RowPitch + x * 4);
        ID3D11DeviceContext1_Unmap(ctx, (ID3D11Resource *)s, 0);
    }
    RELEASE(s);
    return v;
}
static int close_to(DWORD a, DWORD b) {
    for (int i = 0; i < 32; i += 8) { int d = (int)((a >> i) & 0xff) - (int)((b >> i) & 0xff); if (d > 2 || d < -2) return 0; }
    return 1;
}
#define RED   0xff0000ffu
#define GREEN 0xff00ff00u
#define BLUE  0xffff0000u
#define BLACK 0xff000000u

static ID3D11VertexShader *g_vs; static ID3D11PixelShader *g_ps;
static D3D11_VIEWPORT vp = { 0, 0, SIZE, SIZE, 0, 1 };

static ID3D11RasterizerState *g_rs;
static void bind_full(void) {
    /* No culling: the full-screen triangle winds counter-clockwise, a back face under the default state */
    ID3D11DeviceContext1_RSSetState(ctx, g_rs);
    ID3D11DeviceContext1_VSSetShader(ctx, g_vs, NULL, 0);
    ID3D11DeviceContext1_PSSetShader(ctx, g_ps, NULL, 0);
    ID3D11DeviceContext1_VSSetConstantBuffers(ctx, 13, 1, &cb13);
    ID3D11DeviceContext1_PSSetConstantBuffers(ctx, 13, 1, &cb13);
    ID3D11DeviceContext1_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext1_RSSetViewports(ctx, 1, &vp);
}

static ID3D11DepthStencilState *dss(BOOL test, BOOL write, D3D11_COMPARISON_FUNC f, BOOL stencil, D3D11_COMPARISON_FUNC sf,
                                    D3D11_STENCIL_OP pass) {
    D3D11_DEPTH_STENCIL_DESC d = { 0 };
    d.DepthEnable = test; d.DepthWriteMask = write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO; d.DepthFunc = f;
    d.StencilEnable = stencil; d.StencilReadMask = d.StencilWriteMask = 0xff;
    d.FrontFace.StencilFunc = d.BackFace.StencilFunc = sf;
    d.FrontFace.StencilPassOp = d.BackFace.StencilPassOp = pass;
    d.FrontFace.StencilFailOp = d.BackFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
    d.FrontFace.StencilDepthFailOp = d.BackFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
    ID3D11DepthStencilState *s = NULL; ID3D11Device1_CreateDepthStencilState(dev, &d, &s); return s;
}

/* Reverse-Z like Unity: clear depth 0, GREATER with writes, then a farther draw fails, then EQUAL without writes */
static int test_depth_reverse_z(void) {
    ID3D11Texture2D *c = tex2d(DXGI_FORMAT_R8G8B8A8_UNORM, SIZE, SIZE, D3D11_BIND_RENDER_TARGET, 1, 1, 0);
    ID3D11Texture2D *z = tex2d(DXGI_FORMAT_D32_FLOAT, SIZE, SIZE, D3D11_BIND_DEPTH_STENCIL, 1, 1, 0);
    ID3D11RenderTargetView *rv = rtv(c); ID3D11DepthStencilView *dv = NULL;
    ID3D11DepthStencilState *greater = dss(TRUE, TRUE, D3D11_COMPARISON_GREATER, FALSE, D3D11_COMPARISON_ALWAYS, D3D11_STENCIL_OP_KEEP);
    ID3D11DepthStencilState *equal = dss(TRUE, FALSE, D3D11_COMPARISON_EQUAL, FALSE, D3D11_COMPARISON_ALWAYS, D3D11_STENCIL_OP_KEEP);
    float black[4] = { 0, 0, 0, 1 }; DWORD p;
    CHECK_HR(ID3D11Device1_CreateDepthStencilView(dev, (ID3D11Resource *)z, NULL, &dv), "DSV");
    ID3D11DeviceContext1_ClearRenderTargetView(ctx, rv, black);
    ID3D11DeviceContext1_ClearDepthStencilView(ctx, dv, D3D11_CLEAR_DEPTH, 0.0f, 0);
    ID3D11DeviceContext1_OMSetRenderTargets(ctx, 1, &rv, dv);
    bind_full();
    ID3D11DeviceContext1_OMSetDepthStencilState(ctx, greater, 0);
    set_zc(0.5f, 1, 0, 0, 1); ID3D11DeviceContext1_Draw(ctx, 3, 0);
    set_zc(0.3f, 0, 1, 0, 1); ID3D11DeviceContext1_Draw(ctx, 3, 0);
    ID3D11DeviceContext1_OMSetDepthStencilState(ctx, equal, 0);
    set_zc(0.5f, 0, 0, 1, 1); ID3D11DeviceContext1_Draw(ctx, 3, 0);
    p = pixel(c, 32, 32);
    if (!close_to(p, BLUE)) FAIL("pixel %08lx, want %08x (red: EQUAL failed, green: GREATER let a farther draw through, black: nothing passed)", (unsigned long)p, BLUE);
done:
    ID3D11DeviceContext1_OMSetRenderTargets(ctx, 0, NULL, NULL);
    RELEASE(greater); RELEASE(equal); RELEASE(dv); RELEASE(rv); RELEASE(z); RELEASE(c);
    return !failure;
}

/* D24S8: stencil written with ref 1 by one draw, a second draw passes only where stencil == 1 */
static int test_stencil(void) {
    ID3D11Texture2D *c = tex2d(DXGI_FORMAT_R8G8B8A8_UNORM, SIZE, SIZE, D3D11_BIND_RENDER_TARGET, 1, 1, 0);
    ID3D11Texture2D *z = tex2d(DXGI_FORMAT_D24_UNORM_S8_UINT, SIZE, SIZE, D3D11_BIND_DEPTH_STENCIL, 1, 1, 0);
    ID3D11RenderTargetView *rv = rtv(c); ID3D11DepthStencilView *dv = NULL;
    ID3D11DepthStencilState *write = dss(FALSE, FALSE, D3D11_COMPARISON_ALWAYS, TRUE, D3D11_COMPARISON_ALWAYS, D3D11_STENCIL_OP_REPLACE);
    ID3D11DepthStencilState *test = dss(FALSE, FALSE, D3D11_COMPARISON_ALWAYS, TRUE, D3D11_COMPARISON_EQUAL, D3D11_STENCIL_OP_KEEP);
    D3D11_VIEWPORT left = { 0, 0, SIZE / 2, SIZE, 0, 1 };
    float black[4] = { 0, 0, 0, 1 }; DWORD l, r;
    CHECK_HR(ID3D11Device1_CreateDepthStencilView(dev, (ID3D11Resource *)z, NULL, &dv), "DSV");
    ID3D11DeviceContext1_ClearRenderTargetView(ctx, rv, black);
    ID3D11DeviceContext1_ClearDepthStencilView(ctx, dv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    ID3D11DeviceContext1_OMSetRenderTargets(ctx, 1, &rv, dv);
    bind_full();
    ID3D11DeviceContext1_RSSetViewports(ctx, 1, &left);
    ID3D11DeviceContext1_OMSetDepthStencilState(ctx, write, 1);
    set_zc(0.5f, 1, 0, 0, 1); ID3D11DeviceContext1_Draw(ctx, 3, 0);
    ID3D11DeviceContext1_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext1_OMSetDepthStencilState(ctx, test, 1);
    set_zc(0.5f, 0, 1, 0, 1); ID3D11DeviceContext1_Draw(ctx, 3, 0);
    l = pixel(c, 16, 32); r = pixel(c, 48, 32);
    if (!close_to(l, GREEN) || !close_to(r, BLACK)) FAIL("left %08lx (want green), right %08lx (want black)", (unsigned long)l, (unsigned long)r);
done:
    ID3D11DeviceContext1_OMSetRenderTargets(ctx, 0, NULL, NULL);
    RELEASE(write); RELEASE(test); RELEASE(dv); RELEASE(rv); RELEASE(z); RELEASE(c);
    return !failure;
}

/* Render a value into an HDR target, sample it in a second pass into RGBA8 (HDR -> tonemap) */
static int hdr_roundtrip(DXGI_FORMAT fmt) {
    static const char ps_tonemap[] =
        "Texture2D<float4> t : register(t0); SamplerState s : register(s0);\n"
        "float4 main(float4 pos : SV_Position) : SV_Target { float4 v = t.Sample(s, pos.xy / 64.0); return float4(v.rgb * 0.25, 1); }\n";
    ID3D11Texture2D *h = tex2d(fmt, SIZE, SIZE, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 1, 1, 0);
    ID3D11Texture2D *c = tex2d(DXGI_FORMAT_R8G8B8A8_UNORM, SIZE, SIZE, D3D11_BIND_RENDER_TARGET, 1, 1, 0);
    ID3D11RenderTargetView *hv = h ? rtv(h) : NULL, *cv = rtv(c); ID3D11ShaderResourceView *sv = h ? srv((ID3D11Resource *)h, NULL) : NULL;
    ID3D11PixelShader *tp = ps(ps_tonemap); ID3D11SamplerState *smp = NULL;
    D3D11_SAMPLER_DESC sd = { D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP,
                              D3D11_TEXTURE_ADDRESS_CLAMP, 0, 1, D3D11_COMPARISON_NEVER, { 0 }, 0, D3D11_FLOAT32_MAX };
    float black[4] = { 0, 0, 0, 1 }; DWORD p;
    if (!h || !hv || !sv || !tp) FAIL("could not create the %d target or shader", fmt);
    ID3D11Device1_CreateSamplerState(dev, &sd, &smp);
    ID3D11DeviceContext1_ClearRenderTargetView(ctx, hv, black);
    ID3D11DeviceContext1_OMSetRenderTargets(ctx, 1, &hv, NULL);
    ID3D11DeviceContext1_OMSetDepthStencilState(ctx, NULL, 0);
    bind_full();
    set_zc(0.5f, 4.0f, 2.0f, 0, 1); ID3D11DeviceContext1_Draw(ctx, 3, 0);   /* (4, 2, 0): HDR values above 1 */
    ID3D11DeviceContext1_OMSetRenderTargets(ctx, 1, &cv, NULL);
    ID3D11DeviceContext1_PSSetShader(ctx, tp, NULL, 0);
    ID3D11DeviceContext1_PSSetShaderResources(ctx, 0, 1, &sv);
    ID3D11DeviceContext1_PSSetSamplers(ctx, 0, 1, &smp);
    ID3D11DeviceContext1_Draw(ctx, 3, 0);
    p = pixel(c, 32, 32);
    if (!close_to(p, 0xff0080ffu)) FAIL("pixel %08lx, want ff0080ff (1.0, 0.5, 0)", (unsigned long)p);
done:
    { ID3D11ShaderResourceView *n = NULL; ID3D11DeviceContext1_PSSetShaderResources(ctx, 0, 1, &n); }
    ID3D11DeviceContext1_OMSetRenderTargets(ctx, 0, NULL, NULL);
    RELEASE(smp); RELEASE(tp); RELEASE(sv); RELEASE(hv); RELEASE(cv); RELEASE(h); RELEASE(c);
    return !failure;
}
static int test_hdr_rgba16f(void) { return hdr_roundtrip(DXGI_FORMAT_R16G16B16A16_FLOAT); }
static int test_hdr_r11g11b10(void) { return hdr_roundtrip(DXGI_FORMAT_R11G11B10_FLOAT); }
static int test_hdr_rgba32f(void) { return hdr_roundtrip(DXGI_FORMAT_R32G32B32A32_FLOAT); }

/* A compute shader writes a texture (typed UAV store), and reads it back with a typed load in a second dispatch */
static int test_compute_write_sample(void) {
    static const char cs_write[] =
        "RWTexture2D<float4> o : register(u0);\n"
        "[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) { o[id.xy] = float4(0, 1, 0, 1); }\n";
    static const char cs_copy[] =
        "RWTexture2D<float4> i : register(u0); RWTexture2D<unorm float4> o : register(u1);\n"
        "[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) { o[id.xy] = i[id.xy]; }\n";
    ID3D11Texture2D *t = tex2d(DXGI_FORMAT_R16G16B16A16_FLOAT, SIZE, SIZE, D3D11_BIND_UNORDERED_ACCESS, 1, 1, 0);
    ID3D11Texture2D *c = tex2d(DXGI_FORMAT_R8G8B8A8_UNORM, SIZE, SIZE, D3D11_BIND_UNORDERED_ACCESS, 1, 1, 0);
    ID3D11UnorderedAccessView *u[2] = { NULL, NULL }; ID3D11ComputeShader *w = cs(cs_write), *cp = cs(cs_copy); DWORD p;
    if (!t || !c || !w || !cp) FAIL("setup");
    CHECK_HR(ID3D11Device1_CreateUnorderedAccessView(dev, (ID3D11Resource *)t, NULL, &u[0]), "UAV");
    CHECK_HR(ID3D11Device1_CreateUnorderedAccessView(dev, (ID3D11Resource *)c, NULL, &u[1]), "UAV");
    ID3D11DeviceContext1_CSSetShader(ctx, w, NULL, 0);
    ID3D11DeviceContext1_CSSetUnorderedAccessViews(ctx, 0, 1, u, NULL);
    ID3D11DeviceContext1_Dispatch(ctx, SIZE / 8, SIZE / 8, 1);
    ID3D11DeviceContext1_CSSetShader(ctx, cp, NULL, 0);
    ID3D11DeviceContext1_CSSetUnorderedAccessViews(ctx, 0, 2, u, NULL);
    ID3D11DeviceContext1_Dispatch(ctx, SIZE / 8, SIZE / 8, 1);
    p = pixel(c, 32, 32);
    if (!close_to(p, GREEN)) FAIL("pixel %08lx, want %08x", (unsigned long)p, GREEN);
done:
    { ID3D11UnorderedAccessView *n[2] = { NULL, NULL }; ID3D11DeviceContext1_CSSetUnorderedAccessViews(ctx, 0, 2, n, NULL); }
    RELEASE(u[0]); RELEASE(u[1]); RELEASE(w); RELEASE(cp); RELEASE(t); RELEASE(c);
    return !failure;
}

/* Depth written by a draw, then read as a texture (R32_TYPELESS: D32 DSV, R32_FLOAT SRV) */
static int depth_sample(DXGI_FORMAT typeless, DXGI_FORMAT dsv_fmt, DXGI_FORMAT srv_fmt) {
    static const char ps_depth[] =
        "Texture2D<float> t : register(t0);\n"
        "float4 main(float4 pos : SV_Position) : SV_Target { return float4(t.Load(int3(pos.xy, 0)), 0, 0, 1); }\n";
    ID3D11Texture2D *z = tex2d(typeless, SIZE, SIZE, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, 1, 1, 0);
    ID3D11Texture2D *c = tex2d(DXGI_FORMAT_R8G8B8A8_UNORM, SIZE, SIZE, D3D11_BIND_RENDER_TARGET, 1, 1, 0);
    ID3D11RenderTargetView *cv = rtv(c); ID3D11DepthStencilView *dv = NULL; ID3D11ShaderResourceView *sv = NULL;
    ID3D11DepthStencilState *always = dss(TRUE, TRUE, D3D11_COMPARISON_ALWAYS, FALSE, D3D11_COMPARISON_ALWAYS, D3D11_STENCIL_OP_KEEP);
    ID3D11PixelShader *dp = ps(ps_depth); DWORD p;
    D3D11_DEPTH_STENCIL_VIEW_DESC dd = { dsv_fmt, D3D11_DSV_DIMENSION_TEXTURE2D, 0, { { 0 } } };
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = { srv_fmt, D3D11_SRV_DIMENSION_TEXTURE2D, { { 0, 1 } } };
    if (!z || !dp) FAIL("setup");
    CHECK_HR(ID3D11Device1_CreateDepthStencilView(dev, (ID3D11Resource *)z, &dd, &dv), "DSV");
    sv = srv((ID3D11Resource *)z, &sd); if (!sv) FAIL("SRV");
    ID3D11DeviceContext1_ClearDepthStencilView(ctx, dv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    ID3D11DeviceContext1_OMSetRenderTargets(ctx, 0, NULL, dv);
    ID3D11DeviceContext1_OMSetDepthStencilState(ctx, always, 0);
    bind_full();
    set_zc(0.5f, 1, 1, 1, 1); ID3D11DeviceContext1_Draw(ctx, 3, 0);
    ID3D11DeviceContext1_OMSetRenderTargets(ctx, 1, &cv, NULL);
    ID3D11DeviceContext1_OMSetDepthStencilState(ctx, NULL, 0);
    ID3D11DeviceContext1_PSSetShader(ctx, dp, NULL, 0);
    ID3D11DeviceContext1_PSSetShaderResources(ctx, 0, 1, &sv);
    ID3D11DeviceContext1_Draw(ctx, 3, 0);
    p = pixel(c, 32, 32);
    if (!close_to(p, 0xff000080u)) FAIL("pixel %08lx, want ff000080 (depth 0.5 in red)", (unsigned long)p);
done:
    { ID3D11ShaderResourceView *n = NULL; ID3D11DeviceContext1_PSSetShaderResources(ctx, 0, 1, &n); }
    ID3D11DeviceContext1_OMSetRenderTargets(ctx, 0, NULL, NULL);
    RELEASE(always); RELEASE(dp); RELEASE(sv); RELEASE(dv); RELEASE(cv); RELEASE(z); RELEASE(c);
    return !failure;
}
static int test_depth_sample_d32(void) { return depth_sample(DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT); }
static int test_depth_sample_d24s8(void) { return depth_sample(DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_R24_UNORM_X8_TYPELESS); }
static int test_depth_sample_d32s8(void) { return depth_sample(DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS); }

/* Initialized textures: a 2D array (slice 2 green), a cube (face 3 blue) */
static int test_texture_array(void) {
    static const char ps_arr[] =
        "Texture2DArray<float4> t : register(t0);\n"
        "float4 main(float4 pos : SV_Position) : SV_Target { return t.Load(int4(0, 0, 2, 0)); }\n";
    DWORD texels[4][4]; D3D11_SUBRESOURCE_DATA init[4];
    D3D11_TEXTURE2D_DESC d = { 2, 2, 1, 4, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    ID3D11Texture2D *t = NULL, *c = tex2d(DXGI_FORMAT_R8G8B8A8_UNORM, SIZE, SIZE, D3D11_BIND_RENDER_TARGET, 1, 1, 0);
    ID3D11ShaderResourceView *sv = NULL; ID3D11RenderTargetView *cv = rtv(c); ID3D11PixelShader *p = ps(ps_arr); DWORD v;
    for (int s = 0; s < 4; s++) { for (int i = 0; i < 4; i++) texels[s][i] = s == 2 ? GREEN : RED; init[s].pSysMem = texels[s]; init[s].SysMemPitch = 8; init[s].SysMemSlicePitch = 16; }
    CHECK_HR(ID3D11Device1_CreateTexture2D(dev, &d, init, &t), "array texture");
    sv = srv((ID3D11Resource *)t, NULL); if (!sv || !p) FAIL("setup");
    ID3D11DeviceContext1_OMSetRenderTargets(ctx, 1, &cv, NULL);
    bind_full(); ID3D11DeviceContext1_PSSetShader(ctx, p, NULL, 0);
    ID3D11DeviceContext1_PSSetShaderResources(ctx, 0, 1, &sv);
    ID3D11DeviceContext1_Draw(ctx, 3, 0);
    v = pixel(c, 32, 32);
    if (!close_to(v, GREEN)) FAIL("pixel %08lx, want green (slice 2)", (unsigned long)v);
done:
    { ID3D11ShaderResourceView *n = NULL; ID3D11DeviceContext1_PSSetShaderResources(ctx, 0, 1, &n); }
    ID3D11DeviceContext1_OMSetRenderTargets(ctx, 0, NULL, NULL);
    RELEASE(p); RELEASE(sv); RELEASE(cv); RELEASE(t); RELEASE(c);
    return !failure;
}
static int test_texture_cube(void) {
    static const char ps_cube[] =
        "TextureCube<float4> t : register(t0); SamplerState s : register(s0);\n"
        "float4 main(float4 pos : SV_Position) : SV_Target { return t.Sample(s, float3(0, -1, 0)); }\n";   /* -Y: face 3 */
    DWORD texels[6][4]; D3D11_SUBRESOURCE_DATA init[6];
    D3D11_TEXTURE2D_DESC d = { 2, 2, 1, 6, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_TEXTURECUBE };
    ID3D11Texture2D *t = NULL, *c = tex2d(DXGI_FORMAT_R8G8B8A8_UNORM, SIZE, SIZE, D3D11_BIND_RENDER_TARGET, 1, 1, 0);
    ID3D11ShaderResourceView *sv = NULL; ID3D11RenderTargetView *cv = rtv(c); ID3D11PixelShader *p = ps(ps_cube); ID3D11SamplerState *smp = NULL; DWORD v;
    D3D11_SAMPLER_DESC sd = { D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP,
                              D3D11_TEXTURE_ADDRESS_CLAMP, 0, 1, D3D11_COMPARISON_NEVER, { 0 }, 0, D3D11_FLOAT32_MAX };
    for (int s = 0; s < 6; s++) { for (int i = 0; i < 4; i++) texels[s][i] = s == 3 ? BLUE : RED; init[s].pSysMem = texels[s]; init[s].SysMemPitch = 8; init[s].SysMemSlicePitch = 16; }
    CHECK_HR(ID3D11Device1_CreateTexture2D(dev, &d, init, &t), "cube texture");
    sv = srv((ID3D11Resource *)t, NULL); if (!sv || !p) FAIL("setup");
    ID3D11Device1_CreateSamplerState(dev, &sd, &smp);
    ID3D11DeviceContext1_OMSetRenderTargets(ctx, 1, &cv, NULL);
    bind_full(); ID3D11DeviceContext1_PSSetShader(ctx, p, NULL, 0);
    ID3D11DeviceContext1_PSSetShaderResources(ctx, 0, 1, &sv); ID3D11DeviceContext1_PSSetSamplers(ctx, 0, 1, &smp);
    ID3D11DeviceContext1_Draw(ctx, 3, 0);
    v = pixel(c, 32, 32);
    if (!close_to(v, BLUE)) FAIL("pixel %08lx, want blue (face -Y)", (unsigned long)v);
done:
    { ID3D11ShaderResourceView *n = NULL; ID3D11DeviceContext1_PSSetShaderResources(ctx, 0, 1, &n); }
    ID3D11DeviceContext1_OMSetRenderTargets(ctx, 0, NULL, NULL);
    RELEASE(smp); RELEASE(p); RELEASE(sv); RELEASE(cv); RELEASE(t); RELEASE(c);
    return !failure;
}

/* 4x MSAA target resolved into a single-sample one */
static int test_msaa_resolve(void) {
    ID3D11Texture2D *m = tex2d(DXGI_FORMAT_R16G16B16A16_FLOAT, SIZE, SIZE, D3D11_BIND_RENDER_TARGET, 4, 1, 0);
    ID3D11Texture2D *r = tex2d(DXGI_FORMAT_R16G16B16A16_FLOAT, SIZE, SIZE, D3D11_BIND_SHADER_RESOURCE, 1, 1, 0);
    ID3D11Texture2D *c = tex2d(DXGI_FORMAT_R8G8B8A8_UNORM, SIZE, SIZE, D3D11_BIND_RENDER_TARGET, 1, 1, 0);
    ID3D11RenderTargetView *mv = m ? rtv(m) : NULL, *cv = rtv(c); ID3D11ShaderResourceView *sv = r ? srv((ID3D11Resource *)r, NULL) : NULL;
    static const char ps_load[] = "Texture2D<float4> t : register(t0);\n"
        "float4 main(float4 pos : SV_Position) : SV_Target { return t.Load(int3(pos.xy, 0)); }\n";
    ID3D11PixelShader *lp = ps(ps_load); float black[4] = { 0, 0, 0, 1 }; DWORD v;
    if (!mv || !sv || !lp) FAIL("setup");
    ID3D11DeviceContext1_ClearRenderTargetView(ctx, mv, black);
    ID3D11DeviceContext1_OMSetRenderTargets(ctx, 1, &mv, NULL);
    bind_full(); set_zc(0.5f, 0, 1, 0, 1); ID3D11DeviceContext1_Draw(ctx, 3, 0);
    ID3D11DeviceContext1_ResolveSubresource(ctx, (ID3D11Resource *)r, 0, (ID3D11Resource *)m, 0, DXGI_FORMAT_R16G16B16A16_FLOAT);
    ID3D11DeviceContext1_OMSetRenderTargets(ctx, 1, &cv, NULL);
    ID3D11DeviceContext1_PSSetShader(ctx, lp, NULL, 0); ID3D11DeviceContext1_PSSetShaderResources(ctx, 0, 1, &sv);
    ID3D11DeviceContext1_Draw(ctx, 3, 0);
    v = pixel(c, 32, 32);
    if (!close_to(v, GREEN)) FAIL("pixel %08lx, want green", (unsigned long)v);
done:
    { ID3D11ShaderResourceView *n = NULL; ID3D11DeviceContext1_PSSetShaderResources(ctx, 0, 1, &n); }
    ID3D11DeviceContext1_OMSetRenderTargets(ctx, 0, NULL, NULL);
    RELEASE(lp); RELEASE(sv); RELEASE(mv); RELEASE(cv); RELEASE(m); RELEASE(r); RELEASE(c);
    return !failure;
}

/* A constant buffer bound at an offset (VSSetConstantBuffers1/PSSetConstantBuffers1, as Unity's per-draw data) */
static int test_cbuffer_offset(void) {
    static const char ps_cb[] = "cbuffer c : register(b0) { float4 color; };\n"
        "float4 main() : SV_Target { return color; }\n";
    float data[64 * 4] = { 0 }; ID3D11Buffer *b = NULL; ID3D11PixelShader *p = ps(ps_cb);
    D3D11_BUFFER_DESC bd = { sizeof(data), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA init = { data, 0, 0 }; UINT first = 32, num = 16; DWORD v;
    ID3D11Texture2D *c = tex2d(DXGI_FORMAT_R8G8B8A8_UNORM, SIZE, SIZE, D3D11_BIND_RENDER_TARGET, 1, 1, 0); ID3D11RenderTargetView *cv = rtv(c);
    data[0] = 1; data[3] = 1;                                  /* red at offset 0 */
    data[32 * 4 + 1] = 1; data[32 * 4 + 3] = 1;                /* green at constant 32 (512 bytes) */
    CHECK_HR(ID3D11Device1_CreateBuffer(dev, &bd, &init, &b), "buffer"); if (!p) FAIL("shader");
    ID3D11DeviceContext1_OMSetRenderTargets(ctx, 1, &cv, NULL);
    bind_full(); ID3D11DeviceContext1_PSSetShader(ctx, p, NULL, 0);
    ID3D11DeviceContext1_PSSetConstantBuffers1(ctx, 0, 1, &b, &first, &num);
    ID3D11DeviceContext1_Draw(ctx, 3, 0);
    v = pixel(c, 32, 32);
    if (!close_to(v, GREEN)) FAIL("pixel %08lx, want green (red: the offset was ignored)", (unsigned long)v);
done:
    ID3D11DeviceContext1_OMSetRenderTargets(ctx, 0, NULL, NULL);
    RELEASE(p); RELEASE(b); RELEASE(cv); RELEASE(c);
    return !failure;
}

/* StructuredBuffer read in a pixel shader */
static int test_structured_buffer(void) {
    static const char ps_sb[] = "struct S { float4 a; float4 color; }; StructuredBuffer<S> b : register(t0);\n"
        "float4 main() : SV_Target { return b[3].color; }\n";
    float data[8 * 8] = { 0 }; ID3D11Buffer *b = NULL; ID3D11ShaderResourceView *sv = NULL; ID3D11PixelShader *p = ps(ps_sb); DWORD v;
    D3D11_BUFFER_DESC bd = { sizeof(data), D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 32 };
    D3D11_SUBRESOURCE_DATA init = { data, 0, 0 };
    ID3D11Texture2D *c = tex2d(DXGI_FORMAT_R8G8B8A8_UNORM, SIZE, SIZE, D3D11_BIND_RENDER_TARGET, 1, 1, 0); ID3D11RenderTargetView *cv = rtv(c);
    data[3 * 8 + 4 + 2] = 1; data[3 * 8 + 4 + 3] = 1;   /* element 3: color blue */
    CHECK_HR(ID3D11Device1_CreateBuffer(dev, &bd, &init, &b), "buffer");
    sv = srv((ID3D11Resource *)b, NULL); if (!sv || !p) FAIL("setup");
    ID3D11DeviceContext1_OMSetRenderTargets(ctx, 1, &cv, NULL);
    bind_full(); ID3D11DeviceContext1_PSSetShader(ctx, p, NULL, 0); ID3D11DeviceContext1_PSSetShaderResources(ctx, 0, 1, &sv);
    ID3D11DeviceContext1_Draw(ctx, 3, 0);
    v = pixel(c, 32, 32);
    if (!close_to(v, BLUE)) FAIL("pixel %08lx, want blue", (unsigned long)v);
done:
    { ID3D11ShaderResourceView *n = NULL; ID3D11DeviceContext1_PSSetShaderResources(ctx, 0, 1, &n); }
    ID3D11DeviceContext1_OMSetRenderTargets(ctx, 0, NULL, NULL);
    RELEASE(p); RELEASE(sv); RELEASE(b); RELEASE(cv); RELEASE(c);
    return !failure;
}

/* Two render targets written by one draw (deferred GBuffer) */
static int test_mrt(void) {
    static const char ps_mrt[] = "struct O { float4 a : SV_Target0; float4 b : SV_Target1; };\n"
        "O main() { O o; o.a = float4(1, 0, 0, 1); o.b = float4(0, 0, 1, 1); return o; }\n";
    ID3D11Texture2D *a = tex2d(DXGI_FORMAT_R8G8B8A8_UNORM, SIZE, SIZE, D3D11_BIND_RENDER_TARGET, 1, 1, 0);
    ID3D11Texture2D *bt = tex2d(DXGI_FORMAT_R8G8B8A8_UNORM, SIZE, SIZE, D3D11_BIND_RENDER_TARGET, 1, 1, 0);
    ID3D11RenderTargetView *v[2] = { rtv(a), rtv(bt) }; ID3D11PixelShader *p = ps(ps_mrt); DWORD x, y;
    if (!p) FAIL("shader");
    ID3D11DeviceContext1_OMSetRenderTargets(ctx, 2, v, NULL);
    bind_full(); ID3D11DeviceContext1_PSSetShader(ctx, p, NULL, 0);
    ID3D11DeviceContext1_Draw(ctx, 3, 0);
    x = pixel(a, 32, 32); y = pixel(bt, 32, 32);
    if (!close_to(x, RED) || !close_to(y, BLUE)) FAIL("target 0 %08lx (want red), target 1 %08lx (want blue)", (unsigned long)x, (unsigned long)y);
done:
    ID3D11DeviceContext1_OMSetRenderTargets(ctx, 0, NULL, NULL);
    RELEASE(p); RELEASE(v[0]); RELEASE(v[1]); RELEASE(a); RELEASE(bt);
    return !failure;
}

/* Alpha blending, and additive blending into an HDR target */
static int test_blend(void) {
    D3D11_BLEND_DESC bd = { 0 }; ID3D11BlendState *bs = NULL; DWORD v; float black[4] = { 0, 0, 1, 1 };
    ID3D11Texture2D *c = tex2d(DXGI_FORMAT_R8G8B8A8_UNORM, SIZE, SIZE, D3D11_BIND_RENDER_TARGET, 1, 1, 0); ID3D11RenderTargetView *cv = rtv(c);
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA; bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA; bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO; bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    CHECK_HR(ID3D11Device1_CreateBlendState(dev, &bd, &bs), "blend state");
    ID3D11DeviceContext1_ClearRenderTargetView(ctx, cv, black);
    ID3D11DeviceContext1_OMSetRenderTargets(ctx, 1, &cv, NULL);
    ID3D11DeviceContext1_OMSetBlendState(ctx, bs, NULL, 0xffffffff);
    bind_full(); set_zc(0.5f, 1, 0, 0, 0.5f); ID3D11DeviceContext1_Draw(ctx, 3, 0);
    v = pixel(c, 32, 32);
    if (!close_to(v, 0x80800080u)) FAIL("pixel %08lx, want 80800080 (half red over blue)", (unsigned long)v);
done:
    ID3D11DeviceContext1_OMSetBlendState(ctx, NULL, NULL, 0xffffffff);
    ID3D11DeviceContext1_OMSetRenderTargets(ctx, 0, NULL, NULL);
    RELEASE(bs); RELEASE(cv); RELEASE(c);
    return !failure;
}

/* Instanced draw with per-instance data from a vertex buffer (Unity's instancing, GPU particles) */
static int test_instancing(void) {
    static const char vs_inst[] =
        "struct I { float2 off : OFFSET; float4 color : COLOR; uint vid : SV_VertexID; };\n"
        "struct O { float4 pos : SV_Position; float4 color : COLOR; };\n"
        "O main(I i) { float2 p = float2((i.vid << 1) & 2, i.vid & 2) * 0.5 - 0.5; O o; o.pos = float4(p * 0.5 + i.off, 0.5, 1); o.color = i.color; return o; }\n";
    static const char ps_inst[] = "float4 main(float4 pos : SV_Position, float4 color : COLOR) : SV_Target { return color; }\n";
    float inst[2][6] = { { -0.5f, 0, 1, 0, 0, 1 }, { 0.5f, 0, 0, 1, 0, 1 } };
    D3D11_INPUT_ELEMENT_DESC il[2] = { { "OFFSET", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
                                       { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8, D3D11_INPUT_PER_INSTANCE_DATA, 1 } };
    D3D11_BUFFER_DESC bd = { sizeof(inst), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA init = { inst, 0, 0 }; UINT stride = 24, offset = 0; DWORD l, r;
    ID3DBlob *vb = compile(vs_inst, "vs_5_0"); ID3D11VertexShader *v = NULL; ID3D11PixelShader *p = ps(ps_inst);
    ID3D11InputLayout *layout = NULL; ID3D11Buffer *buf = NULL; float black[4] = { 0, 0, 0, 1 };
    ID3D11Texture2D *c = tex2d(DXGI_FORMAT_R8G8B8A8_UNORM, SIZE, SIZE, D3D11_BIND_RENDER_TARGET, 1, 1, 0); ID3D11RenderTargetView *cv = rtv(c);
    if (!vb || !p) FAIL("compile");
    CHECK_HR(ID3D11Device1_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vb), ID3D10Blob_GetBufferSize(vb), NULL, &v), "VS");
    CHECK_HR(ID3D11Device1_CreateInputLayout(dev, il, 2, ID3D10Blob_GetBufferPointer(vb), ID3D10Blob_GetBufferSize(vb), &layout), "layout");
    CHECK_HR(ID3D11Device1_CreateBuffer(dev, &bd, &init, &buf), "buffer");
    ID3D11DeviceContext1_ClearRenderTargetView(ctx, cv, black);
    ID3D11DeviceContext1_OMSetRenderTargets(ctx, 1, &cv, NULL);
    bind_full(); ID3D11DeviceContext1_VSSetShader(ctx, v, NULL, 0); ID3D11DeviceContext1_PSSetShader(ctx, p, NULL, 0);
    ID3D11DeviceContext1_IASetInputLayout(ctx, layout); ID3D11DeviceContext1_IASetVertexBuffers(ctx, 0, 1, &buf, &stride, &offset);
    ID3D11DeviceContext1_DrawInstanced(ctx, 3, 2, 0, 0);
    l = pixel(c, 18, 36); r = pixel(c, 50, 36);
    if (!close_to(l, RED) || !close_to(r, GREEN)) FAIL("left %08lx (want red), right %08lx (want green)", (unsigned long)l, (unsigned long)r);
done:
    ID3D11DeviceContext1_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext1_OMSetRenderTargets(ctx, 0, NULL, NULL);
    RELEASE(vb); RELEASE(v); RELEASE(p); RELEASE(layout); RELEASE(buf); RELEASE(cv); RELEASE(c);
    return !failure;
}

/* Sampling a BC1 texture (block-compressed, as nearly all of the game's textures) */
static int test_bc1_sample(void) {
    static const char ps_bc[] = "Texture2D<float4> t : register(t0);\n"
        "float4 main(float4 pos : SV_Position) : SV_Target { return t.Load(int3(1, 1, 0)); }\n";
    /* one 4x4 block: color0 = pure green (565: 0x07e0), color1 = black, all indices 0 */
    unsigned short block[4] = { 0x07e0, 0x0000, 0x0000, 0x0000 };
    D3D11_TEXTURE2D_DESC d = { 4, 4, 1, 1, DXGI_FORMAT_BC1_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    D3D11_SUBRESOURCE_DATA init = { block, 8, 8 }; DWORD v;
    ID3D11Texture2D *t = NULL, *c = tex2d(DXGI_FORMAT_R8G8B8A8_UNORM, SIZE, SIZE, D3D11_BIND_RENDER_TARGET, 1, 1, 0);
    ID3D11ShaderResourceView *sv = NULL; ID3D11RenderTargetView *cv = rtv(c); ID3D11PixelShader *p = ps(ps_bc);
    CHECK_HR(ID3D11Device1_CreateTexture2D(dev, &d, &init, &t), "BC1 texture");
    sv = srv((ID3D11Resource *)t, NULL); if (!sv || !p) FAIL("setup");
    ID3D11DeviceContext1_OMSetRenderTargets(ctx, 1, &cv, NULL);
    bind_full(); ID3D11DeviceContext1_PSSetShader(ctx, p, NULL, 0); ID3D11DeviceContext1_PSSetShaderResources(ctx, 0, 1, &sv);
    ID3D11DeviceContext1_Draw(ctx, 3, 0);
    v = pixel(c, 32, 32);
    if (!close_to(v, GREEN)) FAIL("pixel %08lx, want green", (unsigned long)v);
done:
    { ID3D11ShaderResourceView *n = NULL; ID3D11DeviceContext1_PSSetShaderResources(ctx, 0, 1, &n); }
    ID3D11DeviceContext1_OMSetRenderTargets(ctx, 0, NULL, NULL);
    RELEASE(p); RELEASE(sv); RELEASE(cv); RELEASE(t); RELEASE(c);
    return !failure;
}


static ID3D11SamplerState *sampler(D3D11_FILTER f, D3D11_TEXTURE_ADDRESS_MODE a, D3D11_COMPARISON_FUNC cmp, const float *border,
                                   float min_lod) {
    D3D11_SAMPLER_DESC d = { f, a, a, a, 0, 1, cmp, { 0 }, min_lod, D3D11_FLOAT32_MAX };
    ID3D11SamplerState *s = NULL;
    if (border) memcpy(d.BorderColor, border, sizeof(d.BorderColor));
    ID3D11Device1_CreateSamplerState(dev, &d, &s);
    return s;
}

/* Runs a full-screen pixel shader with one SRV and one sampler into RGBA8 and returns the center pixel */
static DWORD shade(const char *src, ID3D11ShaderResourceView *sv, ID3D11SamplerState *smp) {
    ID3D11Texture2D *c = tex2d(DXGI_FORMAT_R8G8B8A8_UNORM, SIZE, SIZE, D3D11_BIND_RENDER_TARGET, 1, 1, 0);
    ID3D11RenderTargetView *cv = rtv(c); ID3D11PixelShader *p = ps(src); DWORD v = 0xdeadbeef;
    float black[4] = { 0, 0, 0, 0 };
    if (p) {
        ID3D11DeviceContext1_ClearRenderTargetView(ctx, cv, black);
        ID3D11DeviceContext1_OMSetRenderTargets(ctx, 1, &cv, NULL);
        ID3D11DeviceContext1_OMSetDepthStencilState(ctx, NULL, 0);
        bind_full(); ID3D11DeviceContext1_PSSetShader(ctx, p, NULL, 0);
        ID3D11DeviceContext1_PSSetShaderResources(ctx, 0, 1, &sv);
        ID3D11DeviceContext1_PSSetSamplers(ctx, 0, 1, &smp);
        ID3D11DeviceContext1_Draw(ctx, 3, 0);
        v = pixel(c, 32, 32);
        { ID3D11ShaderResourceView *n = NULL; ID3D11DeviceContext1_PSSetShaderResources(ctx, 0, 1, &n); }
        ID3D11DeviceContext1_OMSetRenderTargets(ctx, 0, NULL, NULL);
    }
    RELEASE(p); RELEASE(cv); RELEASE(c);
    return v;
}

/* A depth texture at 0.5 everywhere (drawn), with its SRV */
static ID3D11Texture2D *depth_half(ID3D11ShaderResourceView **sv) {
    ID3D11Texture2D *z = tex2d(DXGI_FORMAT_R32_TYPELESS, SIZE, SIZE, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, 1, 1, 0);
    D3D11_DEPTH_STENCIL_VIEW_DESC dd = { DXGI_FORMAT_D32_FLOAT, D3D11_DSV_DIMENSION_TEXTURE2D, 0, { { 0 } } };
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = { DXGI_FORMAT_R32_FLOAT, D3D11_SRV_DIMENSION_TEXTURE2D, { { 0, 1 } } };
    ID3D11DepthStencilView *dv = NULL;
    ID3D11DepthStencilState *always = dss(TRUE, TRUE, D3D11_COMPARISON_ALWAYS, FALSE, D3D11_COMPARISON_ALWAYS, D3D11_STENCIL_OP_KEEP);
    ID3D11Device1_CreateDepthStencilView(dev, (ID3D11Resource *)z, &dd, &dv);
    ID3D11DeviceContext1_ClearDepthStencilView(ctx, dv, D3D11_CLEAR_DEPTH, 1.0f, 0);
    ID3D11DeviceContext1_OMSetRenderTargets(ctx, 0, NULL, dv);
    ID3D11DeviceContext1_OMSetDepthStencilState(ctx, always, 0);
    bind_full(); set_zc(0.5f, 1, 1, 1, 1); ID3D11DeviceContext1_Draw(ctx, 3, 0);
    ID3D11DeviceContext1_OMSetRenderTargets(ctx, 0, NULL, NULL);
    ID3D11DeviceContext1_OMSetDepthStencilState(ctx, NULL, 0);
    *sv = srv((ID3D11Resource *)z, &sd);
    RELEASE(always); RELEASE(dv);
    return z;
}

/* Shadow-map lookups: SampleCmpLevelZero, and SampleCmp with linear comparison filtering (PCF) */
static int test_sample_cmp(void) {
    static const char src[] =
        "Texture2D<float> t : register(t0); SamplerComparisonState s : register(s0);\n"
        "float4 main(float4 pos : SV_Position) : SV_Target { float2 uv = pos.xy / 64.0;\n"
        "  return float4(t.SampleCmpLevelZero(s, uv, 0.4), t.SampleCmpLevelZero(s, uv, 0.6), t.SampleCmp(s, uv, 0.4), 1); }\n";
    ID3D11ShaderResourceView *sv = NULL; ID3D11Texture2D *z = depth_half(&sv);
    ID3D11SamplerState *smp = sampler(D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_COMPARISON_LESS_EQUAL, NULL, 0);
    DWORD v;
    if (!sv || !smp) FAIL("setup");
    v = shade(src, sv, smp);
    /* ref <= texel: 0.4 <= 0.5 passes (1), 0.6 fails (0) */
    if (!close_to(v, 0xffff00ffu)) FAIL("pixel %08lx, want ffff00ff (r: cmp 0.4 = 1, g: cmp 0.6 = 0, b: PCF cmp 0.4 = 1)", (unsigned long)v);
done:
    RELEASE(smp); RELEASE(sv); RELEASE(z);
    return !failure;
}

/* GatherCmpRed (PCF in one fetch) on the same depth texture */
static int test_gather_cmp(void) {
    static const char src[] =
        "Texture2D<float> t : register(t0); SamplerComparisonState s : register(s0);\n"
        "float4 main(float4 pos : SV_Position) : SV_Target { float4 g = t.GatherCmpRed(s, pos.xy / 64.0, 0.4); return float4(g.x, g.w, 0, 1); }\n";
    ID3D11ShaderResourceView *sv = NULL; ID3D11Texture2D *z = depth_half(&sv);
    ID3D11SamplerState *smp = sampler(D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_COMPARISON_LESS_EQUAL, NULL, 0);
    DWORD v;
    if (!sv || !smp) FAIL("setup");
    v = shade(src, sv, smp);
    if (!close_to(v, 0xff00ffffu)) FAIL("pixel %08lx, want ff00ffff", (unsigned long)v);
done:
    RELEASE(smp); RELEASE(sv); RELEASE(z);
    return !failure;
}

/* GatherRed returns the four texels in D3D's order: (0,1), (1,1), (1,0), (0,0) */
static int test_gather(void) {
    static const char src[] =
        "Texture2D<float> t : register(t0); SamplerState s : register(s0);\n"
        "float4 main() : SV_Target { return t.GatherRed(s, float2(0.5, 0.5)); }\n";
    float texels[4] = { 0.2f, 0.4f, 0.6f, 0.8f };   /* (0,0) (1,0) / (0,1) (1,1) */
    D3D11_TEXTURE2D_DESC d = { 2, 2, 1, 1, DXGI_FORMAT_R32_FLOAT, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    D3D11_SUBRESOURCE_DATA init = { texels, 8, 16 }; ID3D11Texture2D *t = NULL; ID3D11ShaderResourceView *sv = NULL;
    ID3D11SamplerState *smp = sampler(D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_COMPARISON_NEVER, NULL, 0);
    DWORD v, want;
    CHECK_HR(ID3D11Device1_CreateTexture2D(dev, &d, &init, &t), "texture");
    sv = srv((ID3D11Resource *)t, NULL);
    v = shade(src, sv, smp);
    /* x = (0,1) 0.6, y = (1,1) 0.8, z = (1,0) 0.4, w = (0,0) 0.2 */
    want = 0x3366cc99;   /* as RGBA8 bytes: r 0x99, g 0xcc, b 0x66, a 0x33 */
    if (!close_to(v, want)) FAIL("pixel %08lx, want %08lx (r 0.6, g 0.8, b 0.4, a 0.2)", (unsigned long)v, (unsigned long)want);
done:
    RELEASE(smp); RELEASE(sv); RELEASE(t);
    return !failure;
}

/* Border addressing: white (shadow maps outside their range) and opaque black */
static int border(const float *color, DWORD want) {
    static const char src[] =
        "Texture2D<float4> t : register(t0); SamplerState s : register(s0);\n"
        "float4 main() : SV_Target { return t.SampleLevel(s, float2(2.5, 2.5), 0); }\n";
    DWORD texels[4] = { RED, RED, RED, RED };
    D3D11_TEXTURE2D_DESC d = { 2, 2, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    D3D11_SUBRESOURCE_DATA init = { texels, 8, 16 }; ID3D11Texture2D *t = NULL; ID3D11ShaderResourceView *sv = NULL;
    ID3D11SamplerState *smp = sampler(D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_BORDER, D3D11_COMPARISON_NEVER, color, 0);
    DWORD v;
    CHECK_HR(ID3D11Device1_CreateTexture2D(dev, &d, &init, &t), "texture");
    sv = srv((ID3D11Resource *)t, NULL);
    v = shade(src, sv, smp);
    if (!close_to(v, want)) FAIL("pixel %08lx, want %08lx", (unsigned long)v, (unsigned long)want);
done:
    RELEASE(smp); RELEASE(sv); RELEASE(t);
    return !failure;
}
static int test_border_white(void) { static const float w[4] = { 1, 1, 1, 1 }; return border(w, 0xffffffffu); }
static int test_border_black(void) { static const float b[4] = { 0, 0, 0, 1 }; return border(b, 0xff000000u); }

/* Mip selection: SampleLevel(1), and a sampler MinLOD of 1 */
static int test_mip_level(void) {
    static const char src_level[] =
        "Texture2D<float4> t : register(t0); SamplerState s : register(s0);\n"
        "float4 main() : SV_Target { return t.SampleLevel(s, float2(0.5, 0.5), 1); }\n";
    static const char src_minlod[] =
        "Texture2D<float4> t : register(t0); SamplerState s : register(s0);\n"
        "float4 main() : SV_Target { return t.SampleLevel(s, float2(0.5, 0.5), 0); }\n";
    DWORD l0[16], l1[4]; D3D11_SUBRESOURCE_DATA init[2] = { { l0, 16, 64 }, { l1, 8, 16 } };
    D3D11_TEXTURE2D_DESC d = { 4, 4, 2, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    ID3D11Texture2D *t = NULL; ID3D11ShaderResourceView *sv = NULL; DWORD a, b;
    ID3D11SamplerState *pt = sampler(D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_COMPARISON_NEVER, NULL, 0);
    ID3D11SamplerState *minlod = sampler(D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_COMPARISON_NEVER, NULL, 1.0f);
    for (int i = 0; i < 16; i++) l0[i] = RED;
    for (int i = 0; i < 4; i++) l1[i] = GREEN;
    CHECK_HR(ID3D11Device1_CreateTexture2D(dev, &d, init, &t), "texture");
    sv = srv((ID3D11Resource *)t, NULL);
    a = shade(src_level, sv, pt); b = shade(src_minlod, sv, minlod);
    if (!close_to(a, GREEN) || !close_to(b, GREEN)) FAIL("SampleLevel(1) %08lx, MinLOD 1 %08lx, want green for both", (unsigned long)a, (unsigned long)b);
done:
    RELEASE(pt); RELEASE(minlod); RELEASE(sv); RELEASE(t);
    return !failure;
}

/* TextureCubeArray (reflection probes): array element 1, face +X */
static int test_cube_array(void) {
    static const char src[] =
        "TextureCubeArray<float4> t : register(t0); SamplerState s : register(s0);\n"
        "float4 main() : SV_Target { return t.SampleLevel(s, float4(1, 0, 0, 1), 0); }\n";
    DWORD texels[12][4]; D3D11_SUBRESOURCE_DATA init[12];
    D3D11_TEXTURE2D_DESC d = { 2, 2, 1, 12, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_TEXTURECUBE };
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = { DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_SRV_DIMENSION_TEXTURECUBEARRAY };
    ID3D11Texture2D *t = NULL; ID3D11ShaderResourceView *sv = NULL; DWORD v;
    ID3D11SamplerState *pt = sampler(D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_COMPARISON_NEVER, NULL, 0);
    for (int s = 0; s < 12; s++) { for (int i = 0; i < 4; i++) texels[s][i] = s == 6 ? GREEN : RED; init[s].pSysMem = texels[s]; init[s].SysMemPitch = 8; init[s].SysMemSlicePitch = 16; }
    sd.TextureCubeArray.MostDetailedMip = 0; sd.TextureCubeArray.MipLevels = 1; sd.TextureCubeArray.First2DArrayFace = 0; sd.TextureCubeArray.NumCubes = 2;
    CHECK_HR(ID3D11Device1_CreateTexture2D(dev, &d, init, &t), "texture");
    sv = srv((ID3D11Resource *)t, &sd); if (!sv) FAIL("SRV");
    v = shade(src, sv, pt);
    if (!close_to(v, GREEN)) FAIL("pixel %08lx, want green (cube 1, face +X)", (unsigned long)v);
done:
    RELEASE(pt); RELEASE(sv); RELEASE(t);
    return !failure;
}

/* SV_IsFrontFace on a counter-clockwise (back-facing) triangle with culling off: two-sided foliage */
static int test_front_face(void) {
    static const char src[] = "float4 main(bool front : SV_IsFrontFace) : SV_Target { return front ? float4(1, 0, 0, 1) : float4(0, 1, 0, 1); }\n";
    DWORD v = shade(src, NULL, NULL);
    /* The full-screen triangle winds counter-clockwise: a back face under D3D's default FrontCounterClockwise = FALSE */
    if (!close_to(v, GREEN)) FAIL("pixel %08lx, want green (back face)", (unsigned long)v);
    return 1;
done:
    return 0;
}

/* Alpha test with clip() (cutout foliage) */
static int test_alpha_clip(void) {
    static const char src[] =
        "float4 main(float4 pos : SV_Position) : SV_Target { clip(pos.x < 32 ? -1 : 1); return float4(0, 1, 0, 1); }\n";
    ID3D11Texture2D *c = tex2d(DXGI_FORMAT_R8G8B8A8_UNORM, SIZE, SIZE, D3D11_BIND_RENDER_TARGET, 1, 1, 0);
    ID3D11RenderTargetView *cv = rtv(c); ID3D11PixelShader *p = ps(src); float blue[4] = { 0, 0, 1, 1 }; DWORD l, r;
    if (!p) FAIL("shader");
    ID3D11DeviceContext1_ClearRenderTargetView(ctx, cv, blue);
    ID3D11DeviceContext1_OMSetRenderTargets(ctx, 1, &cv, NULL);
    bind_full(); ID3D11DeviceContext1_PSSetShader(ctx, p, NULL, 0);
    ID3D11DeviceContext1_Draw(ctx, 3, 0);
    l = pixel(c, 16, 32); r = pixel(c, 48, 32);
    if (!close_to(l, BLUE) || !close_to(r, GREEN)) FAIL("left %08lx (want blue: clipped), right %08lx (want green)", (unsigned long)l, (unsigned long)r);
done:
    ID3D11DeviceContext1_OMSetRenderTargets(ctx, 0, NULL, NULL);
    RELEASE(p); RELEASE(cv); RELEASE(c);
    return !failure;
}

/* Alpha to coverage into 4x MSAA: alpha 0.5 covers about half the samples */
static int test_alpha_to_coverage(void) {
    static const char src[] = "float4 main() : SV_Target { return float4(0, 1, 0, 0.5); }\n";
    D3D11_BLEND_DESC bd = { 0 }; ID3D11BlendState *bs = NULL;
    ID3D11Texture2D *m = tex2d(DXGI_FORMAT_R8G8B8A8_UNORM, SIZE, SIZE, D3D11_BIND_RENDER_TARGET, 4, 1, 0);
    ID3D11Texture2D *r = tex2d(DXGI_FORMAT_R8G8B8A8_UNORM, SIZE, SIZE, 0, 1, 1, 0);
    ID3D11RenderTargetView *mv = m ? rtv(m) : NULL; ID3D11PixelShader *p = ps(src); float black[4] = { 0, 0, 0, 1 }; DWORD v; int g;
    bd.AlphaToCoverageEnable = TRUE; bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (!mv || !p) FAIL("setup");
    CHECK_HR(ID3D11Device1_CreateBlendState(dev, &bd, &bs), "blend state");
    ID3D11DeviceContext1_ClearRenderTargetView(ctx, mv, black);
    ID3D11DeviceContext1_OMSetRenderTargets(ctx, 1, &mv, NULL);
    ID3D11DeviceContext1_OMSetBlendState(ctx, bs, NULL, 0xffffffff);
    bind_full(); ID3D11DeviceContext1_PSSetShader(ctx, p, NULL, 0);
    ID3D11DeviceContext1_Draw(ctx, 3, 0);
    ID3D11DeviceContext1_ResolveSubresource(ctx, (ID3D11Resource *)r, 0, (ID3D11Resource *)m, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
    v = pixel(r, 32, 32); g = (v >> 8) & 0xff;
    if (g < 0x60 || g > 0xa0) FAIL("resolved pixel %08lx: green %d, want about half (0x80) of the samples covered", (unsigned long)v, g);
done:
    ID3D11DeviceContext1_OMSetBlendState(ctx, NULL, NULL, 0xffffffff);
    ID3D11DeviceContext1_OMSetRenderTargets(ctx, 0, NULL, NULL);
    RELEASE(bs); RELEASE(p); RELEASE(mv); RELEASE(m); RELEASE(r);
    return !failure;
}

/* Texture3D (volumetric lighting, LUTs) sampled at slice 2 of 4 */
static int test_texture3d(void) {
    static const char src[] =
        "Texture3D<float4> t : register(t0); SamplerState s : register(s0);\n"
        "float4 main() : SV_Target { return t.SampleLevel(s, float3(0.5, 0.5, 0.625), 0); }\n";
    DWORD texels[4][4]; D3D11_TEXTURE3D_DESC d = { 2, 2, 4, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    D3D11_SUBRESOURCE_DATA init = { texels, 8, 16 }; ID3D11Texture3D *t = NULL; ID3D11ShaderResourceView *sv = NULL; DWORD v;
    ID3D11SamplerState *pt = sampler(D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_COMPARISON_NEVER, NULL, 0);
    for (int z = 0; z < 4; z++) for (int i = 0; i < 4; i++) texels[z][i] = z == 2 ? BLUE : RED;
    CHECK_HR(ID3D11Device1_CreateTexture3D(dev, &d, &init, &t), "texture");
    sv = srv((ID3D11Resource *)t, NULL);
    v = shade(src, sv, pt);
    if (!close_to(v, BLUE)) FAIL("pixel %08lx, want blue (slice 2)", (unsigned long)v);
done:
    RELEASE(pt); RELEASE(sv); RELEASE(t);
    return !failure;
}


/* Reads n uints of a buffer back */
static void read_buffer(ID3D11Buffer *b, unsigned *out, unsigned n) {
    D3D11_BUFFER_DESC d; ID3D11Buffer *s = NULL; D3D11_MAPPED_SUBRESOURCE m;
    ID3D11Buffer_GetDesc(b, &d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0; d.StructureByteStride = 0;
    memset(out, 0xee, n * 4);
    ID3D11Device1_CreateBuffer(dev, &d, NULL, &s);
    if (!s) return;
    ID3D11DeviceContext1_CopyResource(ctx, (ID3D11Resource *)s, (ID3D11Resource *)b);
    if (SUCCEEDED(ID3D11DeviceContext1_Map(ctx, (ID3D11Resource *)s, 0, D3D11_MAP_READ, 0, &m))) {
        memcpy(out, m.pData, n * 4); ID3D11DeviceContext1_Unmap(ctx, (ID3D11Resource *)s, 0);
    }
    RELEASE(s);
}
static ID3D11Buffer *buffer(UINT size, UINT bind, UINT misc, UINT stride, const void *data) {
    D3D11_BUFFER_DESC d = { size, D3D11_USAGE_DEFAULT, bind, 0, misc, stride }; D3D11_SUBRESOURCE_DATA s = { data, 0, 0 };
    ID3D11Buffer *b = NULL; ID3D11Device1_CreateBuffer(dev, &d, data ? &s : NULL, &b); return b;
}
static ID3D11UnorderedAccessView *uav_buf(ID3D11Buffer *b, DXGI_FORMAT f, UINT n, UINT flags) {
    D3D11_UNORDERED_ACCESS_VIEW_DESC d = { f, D3D11_UAV_DIMENSION_BUFFER }; ID3D11UnorderedAccessView *u = NULL;
    d.Buffer.FirstElement = 0; d.Buffer.NumElements = n; d.Buffer.Flags = flags;
    ID3D11Device1_CreateUnorderedAccessView(dev, (ID3D11Resource *)b, &d, &u); return u;
}
static void unbind_cs(void) {
    ID3D11UnorderedAccessView *n[4] = { 0 }; ID3D11ShaderResourceView *ns[2] = { 0 };
    ID3D11DeviceContext1_CSSetUnorderedAccessViews(ctx, 0, 4, n, NULL);
    ID3D11DeviceContext1_CSSetShaderResources(ctx, 0, 2, ns);
}

/* AppendStructuredBuffer from 64 threads (half of them append), CopyStructureCount, then a second
 * dispatch reads the count back with a counter reset to 0 */
static int test_append_counter(void) {
    static const char src[] =
        "AppendStructuredBuffer<uint> a : register(u0);\n"
        "[numthreads(64, 1, 1)] void main(uint i : SV_DispatchThreadID) { if (i & 1) a.Append(i); }\n";
    static const char src2[] =
        "RWStructuredBuffer<uint> c : register(u0);\n"
        "[numthreads(64, 1, 1)] void main(uint i : SV_DispatchThreadID) { c.IncrementCounter(); }\n";
    ID3D11Buffer *ab = buffer(64 * 4, D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4, NULL);
    ID3D11Buffer *cb = buffer(64 * 4, D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4, NULL);
    ID3D11Buffer *count = buffer(16, D3D11_BIND_UNORDERED_ACCESS, 0, 0, NULL);
    ID3D11UnorderedAccessView *au = uav_buf(ab, DXGI_FORMAT_UNKNOWN, 64, D3D11_BUFFER_UAV_FLAG_APPEND);
    ID3D11UnorderedAccessView *cu = uav_buf(cb, DXGI_FORMAT_UNKNOWN, 64, D3D11_BUFFER_UAV_FLAG_COUNTER);
    ID3D11ComputeShader *c1 = cs(src), *c2 = cs(src2); UINT zero = 0, initial = 5; unsigned out[4];
    if (!au || !cu || !c1 || !c2) FAIL("setup");
    ID3D11DeviceContext1_CSSetShader(ctx, c1, NULL, 0);
    ID3D11DeviceContext1_CSSetUnorderedAccessViews(ctx, 0, 1, &au, &zero);
    ID3D11DeviceContext1_Dispatch(ctx, 1, 1, 1);
    ID3D11DeviceContext1_CopyStructureCount(ctx, count, 0, au);
    ID3D11DeviceContext1_CSSetShader(ctx, c2, NULL, 0);
    ID3D11DeviceContext1_CSSetUnorderedAccessViews(ctx, 0, 1, &cu, &initial);
    ID3D11DeviceContext1_Dispatch(ctx, 1, 1, 1);
    unbind_cs();
    ID3D11DeviceContext1_CopyStructureCount(ctx, count, 4, cu);
    read_buffer(count, out, 2);
    if (out[0] != 32 || out[1] != 69) FAIL("append count %u (want 32), counter %u (want 5 + 64 = 69)", out[0], out[1]);
done:
    unbind_cs();
    RELEASE(c1); RELEASE(c2); RELEASE(au); RELEASE(cu); RELEASE(ab); RELEASE(cb); RELEASE(count);
    return !failure;
}

/* Atomics: InterlockedAdd on a raw buffer from 256 threads, InterlockedOr/Max in groupshared memory with a barrier */
static int test_atomics(void) {
    static const char src[] =
        "RWByteAddressBuffer o : register(u0); groupshared uint g_or; groupshared uint g_max;\n"
        "[numthreads(64, 1, 1)] void main(uint i : SV_GroupIndex, uint3 gid : SV_GroupID) {\n"
        "  if (i == 0) { g_or = 0; g_max = 0; } GroupMemoryBarrierWithGroupSync();\n"
        "  InterlockedOr(g_or, 1u << (i & 31)); InterlockedMax(g_max, i + gid.x * 100); GroupMemoryBarrierWithGroupSync();\n"
        "  uint prev; o.InterlockedAdd(0, 1, prev);\n"
        "  if (i == 0) { o.Store(4 + gid.x * 8, g_or); o.Store(8 + gid.x * 8, g_max); } }\n";
    unsigned zero[16] = { 0 }, out[8];
    ID3D11Buffer *b = buffer(64, D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0, zero);
    ID3D11UnorderedAccessView *u = uav_buf(b, DXGI_FORMAT_R32_TYPELESS, 16, D3D11_BUFFER_UAV_FLAG_RAW);
    ID3D11ComputeShader *c = cs(src);
    if (!u || !c) FAIL("setup");
    ID3D11DeviceContext1_CSSetShader(ctx, c, NULL, 0);
    ID3D11DeviceContext1_CSSetUnorderedAccessViews(ctx, 0, 1, &u, NULL);
    ID3D11DeviceContext1_Dispatch(ctx, 4, 1, 1);
    unbind_cs();
    read_buffer(b, out, 5);
    if (out[0] != 256 || out[1] != 0xffffffff || out[2] != 63 || out[4] != 163)
        FAIL("add %u (want 256), or %08x (want ffffffff), max %u (want 63), group 1 max %u (want 163)", out[0], out[1], out[2], out[4]);
done:
    unbind_cs();
    RELEASE(c); RELEASE(u); RELEASE(b);
    return !failure;
}

/* Indirect: a compute shader writes DrawInstancedIndirect arguments, the draw uses them (GPU-driven foliage) */
static int test_draw_indirect(void) {
    static const char src[] =
        "RWByteAddressBuffer a : register(u0);\n"
        "[numthreads(1, 1, 1)] void main() { a.Store4(0, uint4(3, 1, 0, 0)); }\n";   /* 3 vertices, 1 instance */
    ID3D11Buffer *args = buffer(16, D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0, NULL);
    ID3D11UnorderedAccessView *u = uav_buf(args, DXGI_FORMAT_R32_TYPELESS, 4, D3D11_BUFFER_UAV_FLAG_RAW);
    ID3D11ComputeShader *c = cs(src);
    ID3D11Texture2D *t = tex2d(DXGI_FORMAT_R8G8B8A8_UNORM, SIZE, SIZE, D3D11_BIND_RENDER_TARGET, 1, 1, 0);
    ID3D11RenderTargetView *tv = rtv(t); float black[4] = { 0, 0, 0, 1 }; DWORD v;
    if (!u || !c) FAIL("setup");
    ID3D11DeviceContext1_CSSetShader(ctx, c, NULL, 0);
    ID3D11DeviceContext1_CSSetUnorderedAccessViews(ctx, 0, 1, &u, NULL);
    ID3D11DeviceContext1_Dispatch(ctx, 1, 1, 1);
    unbind_cs();
    ID3D11DeviceContext1_ClearRenderTargetView(ctx, tv, black);
    ID3D11DeviceContext1_OMSetRenderTargets(ctx, 1, &tv, NULL);
    bind_full(); set_zc(0.5f, 0, 1, 0, 1);
    ID3D11DeviceContext1_DrawInstancedIndirect(ctx, args, 0);
    v = pixel(t, 32, 32);
    if (!close_to(v, GREEN)) FAIL("pixel %08lx, want green", (unsigned long)v);
done:
    ID3D11DeviceContext1_OMSetRenderTargets(ctx, 0, NULL, NULL);
    RELEASE(c); RELEASE(u); RELEASE(args); RELEASE(tv); RELEASE(t);
    return !failure;
}

/* DispatchIndirect with arguments written by a previous dispatch */
static int test_dispatch_indirect(void) {
    static const char src_args[] =
        "RWByteAddressBuffer a : register(u0);\n"
        "[numthreads(1, 1, 1)] void main() { a.Store3(0, uint3(4, 2, 1)); }\n";
    static const char src_count[] =
        "RWByteAddressBuffer o : register(u1);\n"
        "[numthreads(8, 1, 1)] void main() { uint p; o.InterlockedAdd(0, 1, p); }\n";
    ID3D11Buffer *args = buffer(16, D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0, NULL);
    unsigned zero[4] = { 0 }, out[1];
    ID3D11Buffer *o = buffer(16, D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0, zero);
    ID3D11UnorderedAccessView *u[2] = { uav_buf(args, DXGI_FORMAT_R32_TYPELESS, 4, D3D11_BUFFER_UAV_FLAG_RAW),
                                        uav_buf(o, DXGI_FORMAT_R32_TYPELESS, 4, D3D11_BUFFER_UAV_FLAG_RAW) };
    ID3D11ComputeShader *c1 = cs(src_args), *c2 = cs(src_count);
    if (!u[0] || !u[1] || !c1 || !c2) FAIL("setup");
    ID3D11DeviceContext1_CSSetShader(ctx, c1, NULL, 0);
    ID3D11DeviceContext1_CSSetUnorderedAccessViews(ctx, 0, 1, &u[0], NULL);
    ID3D11DeviceContext1_Dispatch(ctx, 1, 1, 1);
    unbind_cs();
    ID3D11DeviceContext1_CSSetShader(ctx, c2, NULL, 0);
    ID3D11DeviceContext1_CSSetUnorderedAccessViews(ctx, 1, 1, &u[1], NULL);
    ID3D11DeviceContext1_DispatchIndirect(ctx, args, 0);
    unbind_cs();
    read_buffer(o, out, 1);
    if (out[0] != 64) FAIL("threads run %u, want 4 x 2 x 1 groups of 8 = 64", out[0]);
done:
    unbind_cs();
    RELEASE(c1); RELEASE(c2); RELEASE(u[0]); RELEASE(u[1]); RELEASE(args); RELEASE(o);
    return !failure;
}

/* Structured buffers with 12-byte (float3) and 20-byte strides, read and written in a compute shader */
static int test_structured_stride(void) {
    static const char src[] =
        "struct A { float3 v; }; struct B { float3 p; float s; uint k; };\n"
        "StructuredBuffer<A> a : register(t0); StructuredBuffer<B> b : register(t1); RWStructuredBuffer<float3> o : register(u0);\n"
        "[numthreads(1, 1, 1)] void main() { o[0] = a[2].v; o[1] = float3(b[1].p.y, b[1].s, (float)b[1].k); o[2] = a[1].v + a[3].v; }\n";
    float av[4][3] = { { 1, 2, 3 }, { 4, 5, 6 }, { 7, 8, 9 }, { 10, 11, 12 } };
    struct { float p[3]; float s; unsigned k; } bv[2] = { { { 0, 0, 0 }, 0, 0 }, { { 1, 21, 3 }, 22, 23 } };
    ID3D11Buffer *ab = buffer(sizeof(av), D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 12, av);
    ID3D11Buffer *bb = buffer(sizeof(bv), D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 20, bv);
    ID3D11Buffer *ob = buffer(3 * 12, D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 12, NULL);
    ID3D11ShaderResourceView *sv[2] = { ab ? srv((ID3D11Resource *)ab, NULL) : NULL, bb ? srv((ID3D11Resource *)bb, NULL) : NULL };
    ID3D11UnorderedAccessView *u = ob ? uav_buf(ob, DXGI_FORMAT_UNKNOWN, 3, 0) : NULL;
    ID3D11ComputeShader *c = cs(src); float out[9];
    if (!sv[0] || !sv[1] || !u || !c) FAIL("setup");
    ID3D11DeviceContext1_CSSetShader(ctx, c, NULL, 0);
    ID3D11DeviceContext1_CSSetShaderResources(ctx, 0, 2, sv);
    ID3D11DeviceContext1_CSSetUnorderedAccessViews(ctx, 0, 1, &u, NULL);
    ID3D11DeviceContext1_Dispatch(ctx, 1, 1, 1);
    unbind_cs();
    read_buffer(ob, (unsigned *)out, 9);
    if (out[0] != 7 || out[1] != 8 || out[2] != 9 || out[3] != 21 || out[4] != 22 || out[5] != 23 || out[6] != 14 || out[7] != 16 || out[8] != 18)
        FAIL("got (%g %g %g) (%g %g %g) (%g %g %g), want (7 8 9) (21 22 23) (14 16 18)",
             out[0], out[1], out[2], out[3], out[4], out[5], out[6], out[7], out[8]);
done:
    unbind_cs();
    RELEASE(c); RELEASE(u); RELEASE(sv[0]); RELEASE(sv[1]); RELEASE(ab); RELEASE(bb); RELEASE(ob);
    return !failure;
}

static const struct { const char *name; int (*run)(void); } tests[] = {
    { "depth_reverse_z", test_depth_reverse_z }, { "stencil", test_stencil },
    { "hdr_rgba16f", test_hdr_rgba16f }, { "hdr_r11g11b10", test_hdr_r11g11b10 }, { "hdr_rgba32f", test_hdr_rgba32f },
    { "compute_write_sample", test_compute_write_sample },
    { "depth_sample_d32", test_depth_sample_d32 }, { "depth_sample_d24s8", test_depth_sample_d24s8 }, { "depth_sample_d32s8", test_depth_sample_d32s8 },
    { "texture_array", test_texture_array }, { "texture_cube", test_texture_cube }, { "msaa_resolve", test_msaa_resolve },
    { "cbuffer_offset", test_cbuffer_offset }, { "structured_buffer", test_structured_buffer }, { "mrt", test_mrt },
    { "blend", test_blend }, { "instancing", test_instancing }, { "bc1_sample", test_bc1_sample },
    { "sample_cmp", test_sample_cmp }, { "gather_cmp", test_gather_cmp }, { "gather", test_gather },
    { "border_white", test_border_white }, { "border_black", test_border_black }, { "mip_level", test_mip_level },
    { "cube_array", test_cube_array }, { "front_face", test_front_face }, { "alpha_clip", test_alpha_clip },
    { "alpha_to_coverage", test_alpha_to_coverage }, { "texture3d", test_texture3d },
    { "append_counter", test_append_counter }, { "atomics", test_atomics }, { "draw_indirect", test_draw_indirect },
    { "dispatch_indirect", test_dispatch_indirect }, { "structured_stride", test_structured_stride },
};

int main(int argc, char **argv) {
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_1; ID3D11Device *d0 = NULL; ID3D11DeviceContext *c0 = NULL;
    D3D11_BUFFER_DESC bd = { 32, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    int all, failed = 0;
    if (argc < 2) { fprintf(stderr, "usage: d3d11_features.exe <test>|list|all\n"); return 2; }
    if (!strcmp(argv[1], "list")) { for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) printf("%s\n", tests[i].name); return 0; }
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &level, 1, D3D11_SDK_VERSION, &d0, NULL, &c0))) { printf("FAIL device\n"); return 1; }
    ID3D11Device_QueryInterface(d0, &IID_ID3D11Device1, (void **)&dev);
    ID3D11DeviceContext_QueryInterface(c0, &IID_ID3D11DeviceContext1, (void **)&ctx);
    ID3D11Device1_CreateBuffer(dev, &bd, NULL, &cb13);
    g_vs = vs(vs_full); g_ps = ps(ps_color);
    {
        D3D11_RASTERIZER_DESC rd = { D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0.0f, 0.0f, TRUE, FALSE, FALSE, FALSE };
        ID3D11Device1_CreateRasterizerState(dev, &rd, &g_rs);
    }
    all = !strcmp(argv[1], "all");
    for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        if (!all && strcmp(argv[1], tests[i].name)) continue;
        failure = NULL;
        if (tests[i].run()) printf("PASS %s\n", tests[i].name); else { printf("FAIL %s: %s\n", tests[i].name, failure); failed++; }
        fflush(stdout);
        if (!all) return failed ? 1 : 0;
    }
    return failed ? 1 : 0;
}
