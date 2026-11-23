/*
 * d3d11_draw.c — headless D3D11 rendering checks for DXVK on MoltenVK
 *
 * Unlike the Tier-1 smokes, these draw into an offscreen render target and read the pixels back, so they
 * test rendering, not only device creation. No window or swap chain is created.
 *
 *   d3d11_draw.exe <test>       run one test; prints "PASS <test>" or "FAIL <test>: <why>", exit 0 or 1
 *   d3d11_draw.exe list         prints the test names
 *
 * Tests: draw, depth_clip, pipeline_stats, stream_output, geometry_shader, d3d11on12
 * Shaders are compiled at run time with d3dcompiler_47.
 *
 * Build: x86_64-w64-mingw32-clang -O2 d3d11_draw.c -o d3d11_draw.exe -ld3d11 -ld3d12 -ldxgi -ld3dcompiler_47 -luuid
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3d11on12.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>

#define SIZE 64

static const char *failure;
#define CHECK(cond, why) do { if (!(cond)) { failure = (why); goto done; } } while (0)
#define CHECK_HR(expr, why) do { HRESULT hr_ = (expr); if (FAILED(hr_)) { static char m_[256]; \
    snprintf(m_, sizeof(m_), "%s (hr %#lx)", (why), (unsigned long)hr_); failure = m_; goto done; } } while (0)

/* A quad over the left half of the target at depth z (clip space, w = 1), as two triangles from SV_VertexID */
static const char vs_quad_source[] =
    "cbuffer c : register(b0) { float z; float3 pad; };\n"
    "float4 main(uint id : SV_VertexID) : SV_Position {\n"
    "    float2 p[6] = { float2(-1, -1), float2(-1, 1), float2(0, 1), float2(-1, -1), float2(0, 1), float2(0, -1) };\n"
    "    return float4(p[id], z, 1.0);\n"
    "}\n";
static const char ps_source[] =
    "float4 main() : SV_Target { return float4(1, 0, 0, 1); }\n";
/* Stream output: the vertex shader's position goes to the buffer */
static const char vs_so_source[] =
    "float4 main(uint id : SV_VertexID) : SV_Position {\n"
    "    return float4(float(id), float(id) * 2.0, 0.5, 1.0);\n"
    "}\n";
/* A geometry shader turning one point into a quad over the right half */
static const char vs_point_source[] =
    "float4 main(uint id : SV_VertexID) : SV_Position { return float4(0, 0, 0.5, 1); }\n";
static const char gs_source[] =
    "struct o { float4 pos : SV_Position; };\n"
    "[maxvertexcount(4)] void main(point float4 p[1] : SV_Position, inout TriangleStream<o> s) {\n"
    "    o v; v.pos = float4(0, -1, 0.5, 1); s.Append(v); v.pos = float4(0, 1, 0.5, 1); s.Append(v);\n"
    "    v.pos = float4(1, -1, 0.5, 1); s.Append(v); v.pos = float4(1, 1, 0.5, 1); s.Append(v);\n"
    "}\n";

static ID3DBlob *compile(const char *source, const char *target) {
    ID3DBlob *blob = NULL, *errors = NULL;
    if (FAILED(D3DCompile(source, strlen(source), NULL, NULL, NULL, "main", target, 0, 0, &blob, &errors))) {
        if (errors) { fprintf(stderr, "%s\n", (const char *)ID3D10Blob_GetBufferPointer(errors)); ID3D10Blob_Release(errors); }
        return NULL;
    }
    if (errors) ID3D10Blob_Release(errors);
    return blob;
}

struct target {
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    ID3D11Texture2D *texture, *staging;
    ID3D11RenderTargetView *rtv;
};

static HRESULT create_target(struct target *t, ID3D11Device *device) {
    D3D11_TEXTURE2D_DESC desc = { SIZE, SIZE, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT,
                                  D3D11_BIND_RENDER_TARGET, 0, 0 };
    HRESULT hr;
    t->device = device;
    ID3D11Device_GetImmediateContext(device, &t->context);
    if (FAILED(hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &t->texture))) return hr;
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &t->staging))) return hr;
    if (FAILED(hr = ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)t->texture, NULL, &t->rtv))) return hr;
    const float blue[4] = { 0, 0, 1, 1 };
    D3D11_VIEWPORT vp = { 0, 0, SIZE, SIZE, 0, 1 };
    ID3D11DeviceContext_ClearRenderTargetView(t->context, t->rtv, blue);
    ID3D11DeviceContext_OMSetRenderTargets(t->context, 1, &t->rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(t->context, 1, &vp);
    return S_OK;
}

/* The pixel at (x, y) after the draws, as 0xAABBGGRR */
static DWORD read_pixel(struct target *t, int x, int y) {
    D3D11_MAPPED_SUBRESOURCE map;
    DWORD value = 0xdeadbeef;
    ID3D11DeviceContext_CopyResource(t->context, (ID3D11Resource *)t->staging, (ID3D11Resource *)t->texture);
    if (SUCCEEDED(ID3D11DeviceContext_Map(t->context, (ID3D11Resource *)t->staging, 0, D3D11_MAP_READ, 0, &map))) {
        value = *(DWORD *)((BYTE *)map.pData + y * map.RowPitch + x * 4);
        ID3D11DeviceContext_Unmap(t->context, (ID3D11Resource *)t->staging, 0);
    }
    return value;
}

static void release_target(struct target *t) {
    if (t->rtv) ID3D11RenderTargetView_Release(t->rtv);
    if (t->staging) ID3D11Texture2D_Release(t->staging);
    if (t->texture) ID3D11Texture2D_Release(t->texture);
    if (t->context) ID3D11DeviceContext_Release(t->context);
}

#define RED  0xff0000ffu
#define BLUE 0xffff0000u

/* Binds the quad vertex shader at depth z, the red pixel shader, and a rasterizer state */
static HRESULT bind_quad(struct target *t, float z, BOOL depth_clip, ID3D11Buffer **cb, ID3D11VertexShader **vs,
                         ID3D11PixelShader **ps, ID3D11RasterizerState **rs) {
    ID3DBlob *vsb = compile(vs_quad_source, "vs_5_0"), *psb = compile(ps_source, "ps_5_0");
    float constants[4] = { z, 0, 0, 0 };
    D3D11_BUFFER_DESC bd = { sizeof(constants), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA data = { constants, 0, 0 };
    D3D11_RASTERIZER_DESC rd = { D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0.0f, 0.0f, depth_clip, FALSE, FALSE, FALSE };
    HRESULT hr = E_FAIL;
    if (!vsb || !psb) goto out;
    if (FAILED(hr = ID3D11Device_CreateVertexShader(t->device, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, vs))) goto out;
    if (FAILED(hr = ID3D11Device_CreatePixelShader(t->device, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, ps))) goto out;
    if (FAILED(hr = ID3D11Device_CreateBuffer(t->device, &bd, &data, cb))) goto out;
    if (FAILED(hr = ID3D11Device_CreateRasterizerState(t->device, &rd, rs))) goto out;
    ID3D11DeviceContext_VSSetShader(t->context, *vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(t->context, 0, 1, cb);
    ID3D11DeviceContext_PSSetShader(t->context, *ps, NULL, 0);
    ID3D11DeviceContext_RSSetState(t->context, *rs);
    ID3D11DeviceContext_IASetPrimitiveTopology(t->context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
out:
    if (vsb) ID3D10Blob_Release(vsb);
    if (psb) ID3D10Blob_Release(psb);
    return hr;
}

static HRESULT create_device(ID3D11Device **device) {
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    return D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &level, 1, D3D11_SDK_VERSION, device, NULL, NULL);
}

#define RELEASE(p) do { if (p) IUnknown_Release((IUnknown *)(p)); } while (0)

/* A red quad over the left half; the right half keeps the clear color */
static int test_draw(void) {
    ID3D11Device *device = NULL; struct target t = { 0 };
    ID3D11Buffer *cb = NULL; ID3D11VertexShader *vs = NULL; ID3D11PixelShader *ps = NULL; ID3D11RasterizerState *rs = NULL;
    DWORD left, right;
    CHECK_HR(create_device(&device), "D3D11CreateDevice");
    CHECK_HR(create_target(&t, device), "render target");
    CHECK_HR(bind_quad(&t, 0.5f, TRUE, &cb, &vs, &ps, &rs), "pipeline");
    ID3D11DeviceContext_Draw(t.context, 6, 0);
    left = read_pixel(&t, 16, 32); right = read_pixel(&t, 48, 32);
    if (left != RED || right != BLUE) {
        static char m[96]; snprintf(m, sizeof(m), "left %08lx (want %08x), right %08lx (want %08x)",
                                    (unsigned long)left, RED, (unsigned long)right, BLUE); failure = m;
    }
done:
    RELEASE(rs); RELEASE(cb); RELEASE(vs); RELEASE(ps); release_target(&t); RELEASE(device);
    return !failure;
}

/* z = 1.5 is past the far plane: clipped with DepthClipEnable, drawn (clamped to 1) without it */
static int test_depth_clip(void) {
    ID3D11Device *device = NULL; struct target t = { 0 };
    ID3D11Buffer *cb = NULL; ID3D11VertexShader *vs = NULL; ID3D11PixelShader *ps = NULL; ID3D11RasterizerState *rs = NULL;
    DWORD clipped, clamped;
    CHECK_HR(create_device(&device), "D3D11CreateDevice");
    CHECK_HR(create_target(&t, device), "render target");
    CHECK_HR(bind_quad(&t, 1.5f, TRUE, &cb, &vs, &ps, &rs), "pipeline");
    ID3D11DeviceContext_Draw(t.context, 6, 0);
    clipped = read_pixel(&t, 16, 32);
    RELEASE(rs); rs = NULL;
    {
        D3D11_RASTERIZER_DESC rd = { D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0.0f, 0.0f, FALSE, FALSE, FALSE, FALSE };
        CHECK_HR(ID3D11Device_CreateRasterizerState(device, &rd, &rs), "rasterizer state");
        ID3D11DeviceContext_RSSetState(t.context, rs);
    }
    ID3D11DeviceContext_Draw(t.context, 6, 0);
    clamped = read_pixel(&t, 16, 32);
    CHECK(clipped == BLUE, "a quad past the far plane was drawn with depth clip enabled");
    CHECK(clamped == RED, "a quad past the far plane was not drawn with depth clip disabled");
done:
    RELEASE(rs); RELEASE(cb); RELEASE(vs); RELEASE(ps); release_target(&t); RELEASE(device);
    return !failure;
}

static HRESULT wait_query(ID3D11DeviceContext *context, ID3D11Query *query, void *data, UINT size) {
    HRESULT hr;
    for (int i = 0; i < 2000; i++) {
        if ((hr = ID3D11DeviceContext_GetData(context, (ID3D11Asynchronous *)query, data, size, 0)) != S_FALSE) return hr;
        Sleep(1);
    }
    return S_FALSE;
}

/* A pipeline statistics query around a draw must complete (MoltenVK has no such queries: zeros are fine) */
static int test_pipeline_stats(void) {
    ID3D11Device *device = NULL; struct target t = { 0 }; ID3D11Query *query = NULL;
    ID3D11Buffer *cb = NULL; ID3D11VertexShader *vs = NULL; ID3D11PixelShader *ps = NULL; ID3D11RasterizerState *rs = NULL;
    D3D11_QUERY_DESC qd = { D3D11_QUERY_PIPELINE_STATISTICS, 0 };
    D3D11_QUERY_DATA_PIPELINE_STATISTICS stats;
    CHECK_HR(create_device(&device), "D3D11CreateDevice");
    CHECK_HR(create_target(&t, device), "render target");
    CHECK_HR(bind_quad(&t, 0.5f, TRUE, &cb, &vs, &ps, &rs), "pipeline");
    CHECK_HR(ID3D11Device_CreateQuery(device, &qd, &query), "CreateQuery");
    ID3D11DeviceContext_Begin(t.context, (ID3D11Asynchronous *)query);
    ID3D11DeviceContext_Draw(t.context, 6, 0);
    ID3D11DeviceContext_End(t.context, (ID3D11Asynchronous *)query);
    ID3D11DeviceContext_Flush(t.context);
    CHECK(wait_query(t.context, query, &stats, sizeof(stats)) == S_OK, "the query never completed");
    printf("pipeline statistics: IAVertices %llu VSInvocations %llu PSInvocations %llu\n",
           (unsigned long long)stats.IAVertices, (unsigned long long)stats.VSInvocations, (unsigned long long)stats.PSInvocations);
    CHECK(read_pixel(&t, 16, 32) == RED, "the draw inside the query did not render");
done:
    RELEASE(query); RELEASE(rs); RELEASE(cb); RELEASE(vs); RELEASE(ps); release_target(&t); RELEASE(device);
    return !failure;
}

/* Stream output of a vertex shader's positions, with an SO statistics query */
static int test_stream_output(void) {
    ID3D11Device *device = NULL; ID3D11DeviceContext *context = NULL; ID3DBlob *vsb = NULL;
    ID3D11VertexShader *vs = NULL; ID3D11GeometryShader *gs = NULL; ID3D11Buffer *so = NULL, *readback = NULL;
    ID3D11Query *query = NULL;
    D3D11_SO_DECLARATION_ENTRY decl = { 0, "SV_Position", 0, 0, 4, 0 };
    UINT stride = 16, offset = 0;
    D3D11_BUFFER_DESC bd = { 3 * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_STREAM_OUTPUT, 0, 0, 0 };
    D3D11_QUERY_DESC qd = { D3D11_QUERY_SO_STATISTICS, 0 };
    D3D11_QUERY_DATA_SO_STATISTICS stats = { 0 };
    D3D11_MAPPED_SUBRESOURCE map;
    CHECK_HR(create_device(&device), "D3D11CreateDevice");
    ID3D11Device_GetImmediateContext(device, &context);
    CHECK((vsb = compile(vs_so_source, "vs_5_0")) != NULL, "compile");
    CHECK_HR(ID3D11Device_CreateVertexShader(device, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &vs), "CreateVertexShader");
    CHECK_HR(ID3D11Device_CreateGeometryShaderWithStreamOutput(device, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb),
        &decl, 1, &stride, 1, D3D11_SO_NO_RASTERIZED_STREAM, NULL, &gs), "CreateGeometryShaderWithStreamOutput");
    CHECK_HR(ID3D11Device_CreateBuffer(device, &bd, NULL, &so), "SO buffer");
    bd.Usage = D3D11_USAGE_STAGING; bd.BindFlags = 0; bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK_HR(ID3D11Device_CreateBuffer(device, &bd, NULL, &readback), "readback buffer");
    CHECK_HR(ID3D11Device_CreateQuery(device, &qd, &query), "CreateQuery");
    ID3D11DeviceContext_VSSetShader(context, vs, NULL, 0);
    ID3D11DeviceContext_GSSetShader(context, gs, NULL, 0);
    ID3D11DeviceContext_SOSetTargets(context, 1, &so, &offset);
    ID3D11DeviceContext_IASetPrimitiveTopology(context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_Begin(context, (ID3D11Asynchronous *)query);
    ID3D11DeviceContext_Draw(context, 3, 0);
    ID3D11DeviceContext_End(context, (ID3D11Asynchronous *)query);
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)readback, (ID3D11Resource *)so);
    ID3D11DeviceContext_Flush(context);
    CHECK_HR(ID3D11DeviceContext_Map(context, (ID3D11Resource *)readback, 0, D3D11_MAP_READ, 0, &map), "Map");
    {
        HRESULT query_hr = wait_query(context, query, &stats, sizeof(stats));
        const float *v = map.pData;
        int ok = v[4] == 1.0f && v[5] == 2.0f && v[8] == 2.0f && v[9] == 4.0f && v[2] == 0.5f;
        printf("stream output: primitives written %llu, vertex 2 = (%g, %g, %g, %g)\n",
               (unsigned long long)stats.NumPrimitivesWritten, v[8], v[9], v[10], v[11]);
        ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)readback, 0);
        CHECK(ok, "the stream output buffer does not hold the vertex positions");
        CHECK(query_hr == S_OK, "the SO statistics query never completed");
        CHECK(stats.NumPrimitivesWritten == 1, "the SO statistics query did not count one primitive");
    }
done:
    RELEASE(query); RELEASE(readback); RELEASE(so); RELEASE(gs); RELEASE(vs); RELEASE(vsb); RELEASE(context); RELEASE(device);
    return !failure;
}

/* A geometry shader turns a point into a quad over the right half */
static int test_geometry_shader(void) {
    ID3D11Device *device = NULL; struct target t = { 0 };
    ID3DBlob *vsb = NULL, *gsb = NULL, *psb = NULL;
    ID3D11VertexShader *vs = NULL; ID3D11GeometryShader *gs = NULL; ID3D11PixelShader *ps = NULL;
    DWORD left, right;
    CHECK_HR(create_device(&device), "D3D11CreateDevice");
    CHECK_HR(create_target(&t, device), "render target");
    CHECK((vsb = compile(vs_point_source, "vs_5_0")) && (gsb = compile(gs_source, "gs_5_0")) && (psb = compile(ps_source, "ps_5_0")), "compile");
    CHECK_HR(ID3D11Device_CreateVertexShader(device, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &vs), "CreateVertexShader");
    CHECK_HR(ID3D11Device_CreateGeometryShader(device, ID3D10Blob_GetBufferPointer(gsb), ID3D10Blob_GetBufferSize(gsb), NULL, &gs), "CreateGeometryShader");
    CHECK_HR(ID3D11Device_CreatePixelShader(device, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &ps), "CreatePixelShader");
    ID3D11DeviceContext_VSSetShader(t.context, vs, NULL, 0);
    ID3D11DeviceContext_GSSetShader(t.context, gs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(t.context, ps, NULL, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(t.context, D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    ID3D11DeviceContext_Draw(t.context, 1, 0);
    left = read_pixel(&t, 16, 32); right = read_pixel(&t, 48, 32);
    if (left != BLUE || right != RED) {
        static char m[96]; snprintf(m, sizeof(m), "left %08lx (want %08x), right %08lx (want %08x)",
                                    (unsigned long)left, BLUE, (unsigned long)right, RED); failure = m;
    }
done:
    RELEASE(vs); RELEASE(gs); RELEASE(ps); RELEASE(vsb); RELEASE(gsb); RELEASE(psb); release_target(&t); RELEASE(device);
    return !failure;
}

/* D3D11 on a D3D12 device (VKD3D-Proton's, through DXVK's interop): the same draw as test_draw */
static int test_d3d11on12(void) {
    ID3D12Device *d3d12 = NULL; ID3D12CommandQueue *queue = NULL; ID3D11Device *device = NULL;
    struct target t = { 0 };
    ID3D11Buffer *cb = NULL; ID3D11VertexShader *vs = NULL; ID3D11PixelShader *ps = NULL; ID3D11RasterizerState *rs = NULL;
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0 };
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    DWORD left, right;
    CHECK_HR(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&d3d12), "D3D12CreateDevice");
    CHECK_HR(ID3D12Device_CreateCommandQueue(d3d12, &qd, &IID_ID3D12CommandQueue, (void **)&queue), "CreateCommandQueue");
    CHECK_HR(D3D11On12CreateDevice((IUnknown *)d3d12, 0, &level, 1, (IUnknown **)&queue, 1, 0, &device, NULL, NULL), "D3D11On12CreateDevice");
    CHECK_HR(create_target(&t, device), "render target");
    CHECK_HR(bind_quad(&t, 0.5f, TRUE, &cb, &vs, &ps, &rs), "pipeline");
    ID3D11DeviceContext_Draw(t.context, 6, 0);
    left = read_pixel(&t, 16, 32); right = read_pixel(&t, 48, 32);
    if (left != RED || right != BLUE) {
        static char m[96]; snprintf(m, sizeof(m), "left %08lx (want %08x), right %08lx (want %08x)",
                                    (unsigned long)left, RED, (unsigned long)right, BLUE); failure = m;
    }
done:
    RELEASE(rs); RELEASE(cb); RELEASE(vs); RELEASE(ps); release_target(&t); RELEASE(device); RELEASE(queue); RELEASE(d3d12);
    return !failure;
}

static const struct { const char *name; int (*run)(void); } tests[] = {
    { "draw", test_draw }, { "depth_clip", test_depth_clip }, { "pipeline_stats", test_pipeline_stats },
    { "stream_output", test_stream_output }, { "geometry_shader", test_geometry_shader }, { "d3d11on12", test_d3d11on12 },
};

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: d3d11_draw.exe <test>|list\n"); return 2; }
    for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        if (!strcmp(argv[1], "list")) { printf("%s\n", tests[i].name); continue; }
        if (strcmp(argv[1], tests[i].name)) continue;
        int ok = tests[i].run();
        if (ok) printf("PASS %s\n", tests[i].name); else printf("FAIL %s: %s\n", tests[i].name, failure);
        fflush(stdout);
        return ok ? 0 : 1;
    }
    if (strcmp(argv[1], "list")) { fprintf(stderr, "no test %s\n", argv[1]); return 2; }
    return 0;
}
