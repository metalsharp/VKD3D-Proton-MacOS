// D3D12 performance probe: one small scene per CPU or GPU cost of the D3D12 -> VKD3D-Proton ->
// MoltenVK -> Metal path, for before/after comparisons. tests/perf/perf_probe.py runs every scene
// in its own process and compares runs. Not part of the functional tests.
//
//   d3d12_perf_probe.exe SCENE [frames] [warmup] [salt]
//   d3d12_perf_probe.exe list
//
// Each frame renders into the backbuffers of a 1920x1080 flip-model swap chain created like Unity's
// (frame latency waitable, maximum latency 2), three frames in flight. Only the present scene
// presents: presentation is paced by the window server (an unfocused window is composited at the
// display's refresh rate), which would hide every other cost. Prints, for the frames after the
// warm-up: cpu (time between frames), work (CPU time from the frame's start through its
// submission), and gpu (submit-to-completion time of serialized frames run afterwards): mean,
// median, 95th percentile and max; the first frame's CPU time, any scene-specific values
// ("extra name=value"), then D3D12_PERF_PROBE_OK SCENE.
// Shaders are DXBC (d3dcompiler_47), like PEAK's; the salt makes compiles cold.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <algorithm>
#include <functional>

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("error: %s:%d: %s = 0x%08lx\n", __FILE__, __LINE__, #x, (unsigned long)hr_); exit(3); } } while (0)

static const UINT W = 1920, H = 1080, FRAMES_IN_FLIGHT = 3, HEAP_SIZE = 16384;
static const DXGI_FORMAT BACKBUFFER_FORMAT = DXGI_FORMAT_R8G8B8A8_UNORM;
static const DXGI_FORMAT HDR_FORMAT = DXGI_FORMAT_R16G16B16A16_FLOAT;

static double qpc_ms()
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return 1000.0 * (double)t.QuadPart / (double)freq.QuadPart;
}

// ---- device, swap chain and per-frame state ----

struct Probe
{
    ID3D12Device *device = nullptr;
    ID3D12CommandQueue *queue = nullptr;
    IDXGISwapChain3 *swapchain = nullptr;
    HANDLE waitable = nullptr;
    ID3D12CommandAllocator *allocators[FRAMES_IN_FLIGHT] = {};
    ID3D12GraphicsCommandList *list = nullptr;
    ID3D12Fence *fence = nullptr;
    UINT64 fence_value = 0, frame_fence[FRAMES_IN_FLIGHT] = {};
    HANDLE event = nullptr;
    ID3D12Resource *backbuffers[FRAMES_IN_FLIGHT] = {};
    ID3D12DescriptorHeap *rtv_heap = nullptr, *dsv_heap = nullptr, *srv_heap = nullptr, *cpu_srv_heap = nullptr;
    UINT rtv_size = 0, srv_size = 0;
    ID3D12QueryHeap *timestamps = nullptr;
    ID3D12Resource *timestamp_readback = nullptr;
    UINT64 timestamp_frequency = 0;
    ID3D12RootSignature *root_signature = nullptr, *compute_root_signature = nullptr;
    // Shared resources most scenes use.
    ID3D12Resource *upload_ring = nullptr;
    uint8_t *upload_ptr = nullptr;
    UINT64 upload_size = 64ull << 20, upload_offset = 0;
    ID3D12Resource *hdr_targets[3] = {}, *depth = nullptr, *textures[8] = {};
    ID3D12Resource *uav_buffer = nullptr;
    unsigned salt = 0;
    UINT frame_index = 0;
};
static Probe p;

static D3D12_CPU_DESCRIPTOR_HANDLE rtv(UINT i) { auto h = p.rtv_heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += (SIZE_T)i * p.rtv_size; return h; }
static D3D12_CPU_DESCRIPTOR_HANDLE dsv(UINT i = 0) { auto h = p.dsv_heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += (SIZE_T)i * p.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV); return h; }
static D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu(UINT i) { auto h = p.srv_heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += (SIZE_T)i * p.srv_size; return h; }
static D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu(UINT i) { auto h = p.srv_heap->GetGPUDescriptorHandleForHeapStart(); h.ptr += (UINT64)i * p.srv_size; return h; }
static D3D12_CPU_DESCRIPTOR_HANDLE staging_cpu(UINT i) { auto h = p.cpu_srv_heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += (SIZE_T)i * p.srv_size; return h; }

static ID3D12Resource *create_buffer(UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE,
                                     D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON)
{
    D3D12_HEAP_PROPERTIES hp = { heap };
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags;
    if (heap == D3D12_HEAP_TYPE_UPLOAD)
        state = D3D12_RESOURCE_STATE_GENERIC_READ;
    else if (heap == D3D12_HEAP_TYPE_READBACK)
        state = D3D12_RESOURCE_STATE_COPY_DEST;
    ID3D12Resource *r = nullptr;
    CHECK(p.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r)));
    return r;
}

static ID3D12Resource *create_texture(UINT w, UINT h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state,
                                      UINT mips = 1, UINT samples = 1, const D3D12_CLEAR_VALUE *clear = nullptr)
{
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = mips;
    rd.Format = format;
    rd.SampleDesc.Count = samples;
    rd.Flags = flags;
    ID3D12Resource *r = nullptr;
    CHECK(p.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, clear, IID_PPV_ARGS(&r)));
    return r;
}

// Bump allocation in the persistently mapped upload ring (wraps; the ring is far larger than a
// frame's use, and a frame is complete before its slot comes around again).
static D3D12_GPU_VIRTUAL_ADDRESS upload_alloc(UINT64 size, void **cpu)
{
    size = (size + 255) & ~255ull;
    if (p.upload_offset + size > p.upload_size)
        p.upload_offset = 0;
    *cpu = p.upload_ptr + p.upload_offset;
    D3D12_GPU_VIRTUAL_ADDRESS va = p.upload_ring->GetGPUVirtualAddress() + p.upload_offset;
    p.upload_offset += size;
    return va;
}

static void barrier(ID3D12GraphicsCommandList *l, ID3D12Resource *r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    l->ResourceBarrier(1, &b);
}

static void wait_fence(UINT64 value)
{
    if (p.fence->GetCompletedValue() >= value)
        return;
    CHECK(p.fence->SetEventOnCompletion(value, p.event));
    WaitForSingleObject(p.event, INFINITE);
}

static UINT64 signal_queue()
{
    CHECK(p.queue->Signal(p.fence, ++p.fence_value));
    return p.fence_value;
}

// ---- shaders and pipelines ----

static ID3DBlob *compile(const std::string &src, const char *entry, const char *target)
{
    ID3DBlob *code = nullptr, *errors = nullptr;
    HRESULT hr = D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr))
    {
        printf("error: compile %s: %s\n", entry, errors ? (const char *)errors->GetBufferPointer() : "?");
        exit(3);
    }
    if (errors)
        errors->Release();
    return code;
}

// b0: 4 root constants (offset.xy, scale, value), b1: root CBV, t0-t7: descriptor table, s0: static sampler.
static const char *common_hlsl = R"(
cbuffer RC : register(b0) { float4 rc; };
cbuffer CB : register(b1) { float4 cb[4]; };
Texture2D tex[8] : register(t0);
SamplerState smp : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs_quad(uint id : SV_VertexID)
{
    V o;
    float2 q = float2((id << 1) & 2, id & 2);
    o.uv = q;
    o.pos = float4((q * 2 - 1) * rc.z + rc.xy, 0.5, 1) + cb[0] * 0.0001;
    return o;
}
float4 ps_flat(V i) : SV_Target { return float4(rc.w, i.uv, 1) + cb[1] * 0.25; }
float4 ps_heavy(V i) : SV_Target
{
    float3 c = float3(i.uv, rc.w);
    [loop] for (int k = 0; k < 24; k++)
        c = frac(c * 1.37 + sin(c.zxy * 4.1 + k) * 0.5);
    return float4(c, 1);
}
float4 ps_tex(V i) : SV_Target
{
    float4 c = 0;
    [unroll] for (int k = 0; k < 8; k++)
        c += tex[k].Sample(smp, i.uv * (1 + k * 0.13));
    return c * 0.125;
}
struct VI { float3 pos : POSITION; float2 uv : TEXCOORD0; };
V vs_input(VI v) { V o; o.pos = float4(v.pos.xy * rc.z + rc.xy, 0.5, 1); o.uv = v.uv; return o; }
// Alpha test, as in Unity's cutout shaders (foliage, fences): discard defeats hidden-surface removal.
float4 ps_alpha(V i) : SV_Target
{
    float4 c = tex[0].Sample(smp, i.uv * 3);
    clip(c.g - 0.5);
    return c * rc.w;
}
// Post-processing style: a few taps of the previous pass.
float4 ps_post(V i) : SV_Target
{
    float2 d = float2(1.0 / 1920, 1.0 / 1080);
    return (tex[0].Sample(smp, i.uv * 0.5) + tex[0].Sample(smp, i.uv * 0.5 + d) +
            tex[0].Sample(smp, i.uv * 0.5 - d) + tex[0].Sample(smp, i.uv * 0.5 + d.yx)) * 0.25 + rc.w * 0.01;
}
float4 ps_sample1(V i) : SV_Target { return tex[0].Sample(smp, i.uv * 3) * rc.w; }
// UI: textured, blended.
float4 ps_ui(V i) : SV_Target { float4 c = tex[0].Sample(smp, i.uv); c.a = 0.5; return c; }
// Instances spread over the screen.
V vs_instanced(uint id : SV_VertexID, uint inst : SV_InstanceID)
{
    V o;
    float2 q = float2((id << 1) & 2, id & 2);
    float2 at = float2(inst % 400, inst / 400 % 400) / 200.0 - 1;
    o.uv = q;
    o.pos = float4(at + q * 0.004, 0.5, 1);
    return o;
}
// Dense geometry: a grid of tiny triangles from the vertex index alone (vertex-bound).
V vs_dense(uint id : SV_VertexID)
{
    V o;
    uint tri = id / 3, corner = id % 3;
    float2 at = float2(tri % 1024, tri / 1024 % 1024) / 512.0 - 1;
    float2 q = float2(corner == 1, corner == 2) * (1.0 / 512);
    o.uv = q;
    o.pos = float4(at + q + sin(at.yx * 7 + rc.w) * 0.001, 0.5, 1);
    return o;
}
)";

// SM 5.1 dynamic indexing into a large table (bindless-style materials).
static const char *bindless_hlsl = R"(
cbuffer RC : register(b0) { float4 rc; uint4 idx; };
Texture2D tex[1000] : register(t0, space1);
SamplerState smp : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; nointerpolation uint index : INDEX; };
V vs_quad(uint id : SV_VertexID)
{
    V o;
    float2 q = float2((id << 1) & 2, id & 2);
    o.uv = q;
    o.pos = float4((q * 2 - 1) * rc.z + rc.xy, 0.5, 1);
    o.index = idx.x;
    return o;
}
float4 ps_bindless(V i) : SV_Target { return tex[NonUniformResourceIndex(i.index)].Sample(smp, i.uv); }
)";

// Compute post-processing writing a 1080p texture.
static const char *compute_image_hlsl = R"(
cbuffer RC : register(b0) { float4 rc; };
RWTexture2D<float4> img : register(u0);
Texture2D<float4> src : register(t0);
[numthreads(8, 8, 1)] void cs_blur(uint3 t : SV_DispatchThreadID)
{
    float4 c = src.Load(int3(t.xy, 0)) + src.Load(int3(t.xy + uint2(1, 0), 0)) + src.Load(int3(t.xy + uint2(0, 1), 0));
    img[t.xy] = c * 0.333 + rc;
}
)";

static const char *compute_hlsl = R"(
cbuffer RC : register(b0) { uint4 rc; };
RWStructuredBuffer<uint> u : register(u0);
[numthreads(64, 1, 1)] void cs_add(uint t : SV_DispatchThreadID) { u[t & 4095] += rc.x + t; }
)";

static std::string salted(const char *src, unsigned salt)
{
    // A different constant per salt gives a different DXBC hash, so nothing below can reuse a compile.
    char buf[96];
    snprintf(buf, sizeof(buf), "\nstatic const float SALT_%u = %u.0;\n", salt, salt);
    return std::string(buf) + src;
}

struct PsoDesc
{
    const char *vs = "vs_quad", *ps = "ps_flat";
    const char *hlsl = nullptr;              // shader source (default common_hlsl)
    const char *ps_target = "ps_5_0", *vs_target = "vs_5_0";
    DXGI_FORMAT rt = BACKBUFFER_FORMAT, dsv = DXGI_FORMAT_D32_FLOAT;
    UINT num_rt = 1;                         // 0: depth-only
    bool depth = false, depth_write = true, blend = false, input = false;
    bool stencil_write = false, stencil_test = false;
    UINT samples = 1;
    unsigned salt = 0;
    ID3D12RootSignature *rs = nullptr;      // default p.root_signature
};

static ID3D12PipelineState *create_pso(const PsoDesc &d, double *compile_ms = nullptr, double *create_ms = nullptr)
{
    double t0 = qpc_ms();
    const char *hlsl = d.hlsl ? d.hlsl : common_hlsl;
    std::string src = d.salt ? salted(hlsl, d.salt) : std::string(hlsl);
    ID3DBlob *vs = compile(src, d.vs, d.vs_target), *ps = d.ps ? compile(src, d.ps, d.ps_target) : nullptr;
    double t1 = qpc_ms();
    D3D12_GRAPHICS_PIPELINE_STATE_DESC g = {};
    g.pRootSignature = d.rs ? d.rs : p.root_signature;
    g.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    if (ps)
        g.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    g.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    if (d.blend)
    {
        auto &b = g.BlendState.RenderTarget[0];
        b.BlendEnable = TRUE;
        b.SrcBlend = D3D12_BLEND_SRC_ALPHA; b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA; b.BlendOp = D3D12_BLEND_OP_ADD;
        b.SrcBlendAlpha = D3D12_BLEND_ONE; b.DestBlendAlpha = D3D12_BLEND_ZERO; b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    g.SampleMask = UINT_MAX;
    g.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    g.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    g.RasterizerState.DepthClipEnable = TRUE;
    if (d.depth || d.stencil_write || d.stencil_test)
    {
        g.DepthStencilState.DepthEnable = d.depth;
        g.DepthStencilState.DepthWriteMask = d.depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        g.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        g.DSVFormat = d.dsv;
        if (d.stencil_write || d.stencil_test)
        {
            auto &f = g.DepthStencilState.FrontFace;
            g.DepthStencilState.StencilEnable = TRUE;
            g.DepthStencilState.StencilReadMask = g.DepthStencilState.StencilWriteMask = 0xff;
            f.StencilFailOp = f.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
            f.StencilPassOp = d.stencil_write ? D3D12_STENCIL_OP_REPLACE : D3D12_STENCIL_OP_KEEP;
            f.StencilFunc = d.stencil_test ? D3D12_COMPARISON_FUNC_EQUAL : D3D12_COMPARISON_FUNC_ALWAYS;
            g.DepthStencilState.BackFace = f;
        }
    }
    static const D3D12_INPUT_ELEMENT_DESC elements[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    if (d.input)
        g.InputLayout = { elements, 2 };
    g.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    g.NumRenderTargets = d.num_rt;
    if (d.num_rt)
        g.RTVFormats[0] = d.rt;
    g.SampleDesc.Count = d.samples;
    ID3D12PipelineState *pso = nullptr;
    CHECK(p.device->CreateGraphicsPipelineState(&g, IID_PPV_ARGS(&pso)));
    double t2 = qpc_ms();
    vs->Release();
    if (ps)
        ps->Release();
    if (compile_ms) *compile_ms += t1 - t0;
    if (create_ms) *create_ms += t2 - t1;
    return pso;
}

static ID3D12PipelineState *create_compute_pso(unsigned salt = 0)
{
    std::string src = salt ? salted(compute_hlsl, salt) : std::string(compute_hlsl);
    ID3DBlob *cs = compile(src, "cs_add", "cs_5_0");
    D3D12_COMPUTE_PIPELINE_STATE_DESC c = {};
    c.pRootSignature = p.compute_root_signature;
    c.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
    ID3D12PipelineState *pso = nullptr;
    CHECK(p.device->CreateComputePipelineState(&c, IID_PPV_ARGS(&pso)));
    cs->Release();
    return pso;
}

static ID3D12RootSignature *serialize_root_signature(const D3D12_ROOT_SIGNATURE_DESC &desc)
{
    ID3DBlob *blob = nullptr, *errors = nullptr;
    CHECK(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors));
    ID3D12RootSignature *rs = nullptr;
    CHECK(p.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs)));
    blob->Release();
    return rs;
}

// ---- setup ----

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

static void setup()
{
    WNDCLASSA wc = {};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "d3d12_perf_probe";
    RegisterClassA(&wc);
    RECT r = { 0, 0, (LONG)W, (LONG)H };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowA("d3d12_perf_probe", "d3d12_perf_probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0,
                              r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);

    IDXGIFactory4 *factory = nullptr;
    CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
    CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&p.device)));
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    CHECK(p.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&p.queue)));

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = W;
    sd.Height = H;
    sd.Format = BACKBUFFER_FORMAT;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = FRAMES_IN_FLIGHT;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    IDXGISwapChain1 *sc1 = nullptr;
    CHECK(factory->CreateSwapChainForHwnd(p.queue, hwnd, &sd, nullptr, nullptr, &sc1));
    CHECK(sc1->QueryInterface(IID_PPV_ARGS(&p.swapchain)));
    sc1->Release();
    CHECK(p.swapchain->SetMaximumFrameLatency(2));
    p.waitable = p.swapchain->GetFrameLatencyWaitableObject();

    for (UINT i = 0; i < FRAMES_IN_FLIGHT; i++)
    {
        CHECK(p.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&p.allocators[i])));
        CHECK(p.swapchain->GetBuffer(i, IID_PPV_ARGS(&p.backbuffers[i])));
    }
    CHECK(p.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, p.allocators[0], nullptr, IID_PPV_ARGS(&p.list)));
    CHECK(p.list->Close());
    CHECK(p.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&p.fence)));
    p.event = CreateEventA(nullptr, FALSE, FALSE, nullptr);

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = FRAMES_IN_FLIGHT + 16;
    CHECK(p.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&p.rtv_heap)));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    hd.NumDescriptors = 4;
    CHECK(p.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&p.dsv_heap)));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = HEAP_SIZE;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    CHECK(p.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&p.srv_heap)));
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    CHECK(p.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&p.cpu_srv_heap)));
    p.rtv_size = p.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    p.srv_size = p.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (UINT i = 0; i < FRAMES_IN_FLIGHT; i++)
        p.device->CreateRenderTargetView(p.backbuffers[i], nullptr, rtv(i));

    D3D12_QUERY_HEAP_DESC qh = {};
    qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qh.Count = 2 * FRAMES_IN_FLIGHT;
    CHECK(p.device->CreateQueryHeap(&qh, IID_PPV_ARGS(&p.timestamps)));
    p.timestamp_readback = create_buffer(16 * FRAMES_IN_FLIGHT, D3D12_HEAP_TYPE_READBACK);
    CHECK(p.queue->GetTimestampFrequency(&p.timestamp_frequency));

    // Graphics root signature: root constants b0, root CBV b1, SRV table t0-t7, static sampler s0.
    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 8, 0, 0, 0 };
    D3D12_ROOT_PARAMETER rp[3] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants = { 0, 0, 4 };
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rp[1].Descriptor = { 1, 0 };
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[2].DescriptorTable = { 1, &range };
    D3D12_STATIC_SAMPLER_DESC ss = {};
    ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    ss.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 3, rp, 1, &ss, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
    p.root_signature = serialize_root_signature(rsd);
    // Compute: root constants b0, root UAV u0.
    D3D12_ROOT_PARAMETER cp[2] = {};
    cp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    cp[0].Constants = { 0, 0, 4 };
    cp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    cp[1].Descriptor = { 0, 0 };
    D3D12_ROOT_SIGNATURE_DESC crsd = { 2, cp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    p.compute_root_signature = serialize_root_signature(crsd);

    p.upload_ring = create_buffer(p.upload_size, D3D12_HEAP_TYPE_UPLOAD);
    CHECK(p.upload_ring->Map(0, nullptr, (void **)&p.upload_ptr));

    D3D12_CLEAR_VALUE cv = { HDR_FORMAT, { 0, 0, 0, 0 } };
    for (int i = 0; i < 3; i++)
    {
        p.hdr_targets[i] = create_texture(W, H, HDR_FORMAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 1, 1, &cv);
        p.device->CreateRenderTargetView(p.hdr_targets[i], nullptr, rtv(FRAMES_IN_FLIGHT + i));
    }
    D3D12_CLEAR_VALUE dv = { DXGI_FORMAT_D32_FLOAT };
    dv.DepthStencil.Depth = 1.0f;
    p.depth = create_texture(W, H, DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                             D3D12_RESOURCE_STATE_DEPTH_WRITE, 1, 1, &dv);
    p.device->CreateDepthStencilView(p.depth, nullptr, dsv());

    // Eight 512x512 RGBA8 textures with mips, filled once, SRVs in heap slots 0-7.
    std::vector<uint32_t> texels(512 * 512);
    for (UINT i = 0; i < texels.size(); i++)
        texels[i] = 0xff000000u | (i * 2654435761u >> 8);
    ID3D12CommandAllocator *a = p.allocators[0];
    CHECK(a->Reset());
    CHECK(p.list->Reset(a, nullptr));
    for (int t = 0; t < 8; t++)
    {
        p.textures[t] = create_texture(512, 512, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, 1);
        void *cpu;
        D3D12_GPU_VIRTUAL_ADDRESS va = upload_alloc(512 * 512 * 4, &cpu);
        memcpy(cpu, texels.data(), 512 * 512 * 4);
        D3D12_TEXTURE_COPY_LOCATION dst = { p.textures[t], D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
        D3D12_TEXTURE_COPY_LOCATION src = { p.upload_ring, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        src.PlacedFootprint.Offset = va - p.upload_ring->GetGPUVirtualAddress();
        src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, 512, 512, 1, 512 * 4 };
        p.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        barrier(p.list, p.textures[t], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        p.device->CreateShaderResourceView(p.textures[t], nullptr, srv_cpu(t));
    }
    CHECK(p.list->Close());
    ID3D12CommandList *lists[] = { p.list };
    p.queue->ExecuteCommandLists(1, lists);
    wait_fence(signal_queue());
    p.uav_buffer = create_buffer(4096 * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    factory->Release();
}

// ---- scenes ----
// init() runs once before the first frame; frame() records into p.list between the frame's
// begin (backbuffer bound as render target, cleared, viewport set) and end, and may submit
// work of its own. Extras accumulate per-operation timings.

struct Extras
{
    std::vector<std::pair<std::string, double>> values;
    bool frozen = false;
    void add(const char *name, double v)
    {
        if (frozen) return;
        for (auto &e : values)
            if (e.first == name) { e.second += v; return; }
        values.emplace_back(name, v);
    }
};
static Extras extras;

struct Scene
{
    const char *name, *desc;
    std::function<void()> init;
    std::function<void(UINT frame)> frame;
    bool present = false;   // Only the present scenes pace by the swap chain; the rest run offscreen.
    UINT sync_interval = 0;
};

static void set_quad_state(ID3D12GraphicsCommandList *l, ID3D12PipelineState *pso)
{
    l->SetGraphicsRootSignature(p.root_signature);
    l->SetPipelineState(pso);
    l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    void *cpu;
    D3D12_GPU_VIRTUAL_ADDRESS cb = upload_alloc(256, &cpu);
    memset(cpu, 0, 256);
    l->SetGraphicsRootConstantBufferView(1, cb);
    l->SetGraphicsRootDescriptorTable(2, srv_gpu(0));
}

static void viewport(ID3D12GraphicsCommandList *l, UINT w = W, UINT h = H)
{
    D3D12_VIEWPORT vp = { 0, 0, (float)w, (float)h, 0, 1 };
    D3D12_RECT sc = { 0, 0, (LONG)w, (LONG)h };
    l->RSSetViewports(1, &vp);
    l->RSSetScissorRects(1, &sc);
}

static void small_quad(ID3D12GraphicsCommandList *l, UINT i, float value)
{
    float rc[4] = { (float)(i % 64) / 32.0f - 1.0f, (float)(i / 64 % 64) / 32.0f - 1.0f, 0.01f, value };
    l->SetGraphicsRoot32BitConstants(0, 4, rc, 0);
    l->DrawInstanced(3, 1, 0, 0);
}

static void submit_and_wait(ID3D12GraphicsCommandList *l)
{
    CHECK(l->Close());
    ID3D12CommandList *lists[] = { l };
    p.queue->ExecuteCommandLists(1, lists);
    wait_fence(signal_queue());
}

static std::vector<Scene> base_scenes()
{
    static ID3D12PipelineState *flat, *heavy, *texd, *input, *flat_depth, *flat_hdr, *flat_msaa, *compute, *psos[16];
    static ID3D12Resource *vb, *ib, *buffers_default, *upload_texture_dst, *readback, *msaa_target, *resolve_target;
    static ID3D12Heap *placed_heap;
    static ID3D12CommandAllocator *extra_allocs[40 * FRAMES_IN_FLIGHT];
    static ID3D12GraphicsCommandList *extra_lists[64];
    static ID3D12QueryHeap *ts_heap, *occlusion_heap;
    static ID3D12CommandSignature *draw_signature;
    static ID3D12Resource *indirect_args;

    auto base = [] { flat = create_pso({}); };
    return {
        { "empty", "clear only: the fixed cost of a frame (offscreen)", base, [](UINT) {} },
        { "present", "clear + Present(0) through the frame latency waitable, like a game's frame", base, [](UINT) {}, true },
        { "draws", "5000 draws, root constants per draw", base, [](UINT f) {
            set_quad_state(p.list, flat);
            double t = qpc_ms();
            for (UINT i = 0; i < 5000; i++) small_quad(p.list, i, f * 0.01f);
            extras.add("record_us_per_draw", (qpc_ms() - t) * 1000.0 / 5000);
        } },
        { "draws_rootcbv", "5000 draws, a fresh root CBV per draw (Unity's pattern)", base, [](UINT f) {
            set_quad_state(p.list, flat);
            double t = qpc_ms();
            for (UINT i = 0; i < 5000; i++)
            {
                void *cpu;
                D3D12_GPU_VIRTUAL_ADDRESS cb = upload_alloc(64, &cpu);
                ((float *)cpu)[4] = (float)i;
                p.list->SetGraphicsRootConstantBufferView(1, cb);
                small_quad(p.list, i, f * 0.01f);
            }
            extras.add("record_us_per_draw", (qpc_ms() - t) * 1000.0 / 5000);
        } },
        { "draws_tables", "5000 draws, descriptor table switch per draw", [] {
            flat = create_pso({});
            for (UINT i = 8; i < 8 + 1024; i++) p.device->CreateShaderResourceView(p.textures[i % 8], nullptr, srv_cpu(i));
        }, [](UINT f) {
            set_quad_state(p.list, flat);
            double t = qpc_ms();
            for (UINT i = 0; i < 5000; i++)
            {
                p.list->SetGraphicsRootDescriptorTable(2, srv_gpu(8 + (i % 1000)));
                small_quad(p.list, i, f * 0.01f);
            }
            extras.add("record_us_per_draw", (qpc_ms() - t) * 1000.0 / 5000);
        } },
        { "draws_pso", "4000 draws over 16 pipelines, a pipeline change per draw", [] {
            for (unsigned i = 0; i < 16; i++) { PsoDesc d; d.blend = i & 1; d.salt = 1000 + i; psos[i] = create_pso(d); }
        }, [](UINT f) {
            set_quad_state(p.list, psos[0]);
            double t = qpc_ms();
            for (UINT i = 0; i < 4000; i++) { p.list->SetPipelineState(psos[i % 16]); small_quad(p.list, i, f * 0.01f); }
            extras.add("record_us_per_draw", (qpc_ms() - t) * 1000.0 / 4000);
        } },
        { "draws_vbib", "5000 indexed draws, vertex and index buffer binds per draw", [] {
            PsoDesc d; d.vs = "vs_input"; d.input = true; input = create_pso(d);
            vb = create_buffer(1 << 20, D3D12_HEAP_TYPE_UPLOAD);
            ib = create_buffer(1 << 16, D3D12_HEAP_TYPE_UPLOAD);
            float *v; CHECK(vb->Map(0, nullptr, (void **)&v));
            for (int i = 0; i < 4096; i++) { float q[5] = { (float)(i & 1), (float)(i >> 1 & 1), 0, 0, 0 }; memcpy(v + i * 5, q, sizeof(q)); }
            vb->Unmap(0, nullptr);
            uint16_t *ix; CHECK(ib->Map(0, nullptr, (void **)&ix));
            for (int i = 0; i < 1024; i++) ix[i] = (uint16_t)(i % 3);
            ib->Unmap(0, nullptr);
        }, [](UINT f) {
            set_quad_state(p.list, input);
            double t = qpc_ms();
            for (UINT i = 0; i < 5000; i++)
            {
                D3D12_VERTEX_BUFFER_VIEW vbv = { vb->GetGPUVirtualAddress() + (i % 256) * 20 * 4, 20 * 64, 20 };
                D3D12_INDEX_BUFFER_VIEW ibv = { ib->GetGPUVirtualAddress() + (i % 64) * 6, 600, DXGI_FORMAT_R16_UINT };
                p.list->IASetVertexBuffers(0, 1, &vbv);
                p.list->IASetIndexBuffer(&ibv);
                float rc[4] = { (float)(i % 64) / 32.0f - 1.0f, (float)(i / 64 % 64) / 32.0f - 1.0f, 0.01f, f * 0.01f };
                p.list->SetGraphicsRoot32BitConstants(0, 4, rc, 0);
                p.list->DrawIndexedInstanced(3, 1, 0, 0, 0);
            }
            extras.add("record_us_per_draw", (qpc_ms() - t) * 1000.0 / 5000);
        } },
        { "passes", "100 render passes of 6 draws across 3 HDR targets (load, clear, draw, sample)", [] {
            PsoDesc d; d.rt = HDR_FORMAT; flat_hdr = create_pso(d);
            for (int i = 0; i < 3; i++) p.device->CreateShaderResourceView(p.hdr_targets[i], nullptr, srv_cpu(2000 + i));
        }, [](UINT f) {
            double t = qpc_ms();
            for (UINT pass = 0; pass < 100; pass++)
            {
                ID3D12Resource *target = p.hdr_targets[pass % 3];
                barrier(p.list, target, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                auto h = rtv(FRAMES_IN_FLIGHT + pass % 3);
                p.list->OMSetRenderTargets(1, &h, FALSE, nullptr);
                if (pass % 5 == 0) { float c[4] = { 0, 0, 0, 0 }; p.list->ClearRenderTargetView(h, c, 0, nullptr); }
                set_quad_state(p.list, flat_hdr);
                p.list->SetGraphicsRootDescriptorTable(2, srv_gpu(2000 + (pass + 1) % 3));
                for (UINT i = 0; i < 6; i++) small_quad(p.list, pass * 6 + i, f * 0.01f);
                barrier(p.list, target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
            auto bb = rtv(p.frame_index);
            p.list->OMSetRenderTargets(1, &bb, FALSE, nullptr);
            extras.add("record_us_per_pass", (qpc_ms() - t) * 1000.0 / 100);
        } },
        { "clears", "40 full-screen color and depth clears into 1080p targets", base, [](UINT) {
            for (UINT i = 0; i < 20; i++)
            {
                ID3D12Resource *target = p.hdr_targets[i % 3];
                barrier(p.list, target, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                float c[4] = { i * 0.05f, 0, 0, 1 };
                p.list->ClearRenderTargetView(rtv(FRAMES_IN_FLIGHT + i % 3), c, 0, nullptr);
                p.list->ClearDepthStencilView(dsv(), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
                barrier(p.list, target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
        } },
        { "barriers", "1000 transition barriers on textures between draws", base, [](UINT f) {
            set_quad_state(p.list, flat);
            double t = qpc_ms();
            for (UINT i = 0; i < 500; i++)
            {
                ID3D12Resource *tex = p.textures[i % 8];
                barrier(p.list, tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                small_quad(p.list, i, f * 0.01f);
                barrier(p.list, tex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
            extras.add("record_us_per_barrier", (qpc_ms() - t) * 1000.0 / 1000);
        } },
        { "upload_buffer", "4 MiB written to an upload heap and copied to a default buffer per frame", [] {
            flat = create_pso({});
            buffers_default = create_buffer(4 << 20, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        }, [](UINT f) {
            void *cpu;
            double t = qpc_ms();
            D3D12_GPU_VIRTUAL_ADDRESS va = upload_alloc(4 << 20, &cpu);
            memset(cpu, f & 0xff, 4 << 20);
            extras.add("memset_ms", qpc_ms() - t);
            p.list->CopyBufferRegion(buffers_default, 0, p.upload_ring, va - p.upload_ring->GetGPUVirtualAddress(), 4 << 20);
        } },
        { "upload_texture", "a 2048x2048 RGBA8 texture uploaded per frame (CopyTextureRegion)", [] {
            upload_texture_dst = create_texture(2048, 2048, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        }, [](UINT f) {
            void *cpu;
            D3D12_GPU_VIRTUAL_ADDRESS va = upload_alloc(2048 * 2048 * 4, &cpu);
            memset(cpu, f & 0xff, 2048 * 2048 * 4);
            D3D12_TEXTURE_COPY_LOCATION dst = { upload_texture_dst, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
            D3D12_TEXTURE_COPY_LOCATION src = { p.upload_ring, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
            src.PlacedFootprint.Offset = va - p.upload_ring->GetGPUVirtualAddress();
            src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, 2048, 2048, 1, 2048 * 4 };
            p.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        } },
        { "map_unmap", "1000 Map/Unmap pairs on upload buffers per frame", [] {
            vb = create_buffer(1 << 20, D3D12_HEAP_TYPE_UPLOAD);
        }, [](UINT) {
            double t = qpc_ms();
            for (UINT i = 0; i < 1000; i++)
            {
                void *cpu;
                D3D12_RANGE none = { 0, 0 };
                CHECK(vb->Map(0, &none, &cpu));
                ((uint32_t *)cpu)[i] = i;
                vb->Unmap(0, nullptr);
            }
            extras.add("us_per_map_unmap", (qpc_ms() - t) * 1000.0 / 1000);
        } },
        { "resource_churn", "50 committed buffers and 10 textures created and released per frame", base, [](UINT) {
            double t = qpc_ms();
            std::vector<ID3D12Resource *> rs;
            for (UINT i = 0; i < 50; i++) rs.push_back(create_buffer(64 << 10, D3D12_HEAP_TYPE_DEFAULT));
            for (UINT i = 0; i < 10; i++) rs.push_back(create_texture(256, 256, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON));
            double t1 = qpc_ms();
            for (auto *r : rs) r->Release();
            extras.add("create_us_per_resource", (t1 - t) * 1000.0 / 60);
            extras.add("release_us_per_resource", (qpc_ms() - t1) * 1000.0 / 60);
        } },
        { "placed_churn", "200 placed buffers created and released per frame in one heap", [] {
            D3D12_HEAP_DESC hd = {};
            hd.SizeInBytes = 64 << 20;
            hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
            hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
            CHECK(p.device->CreateHeap(&hd, IID_PPV_ARGS(&placed_heap)));
        }, [](UINT) {
            D3D12_RESOURCE_DESC rd = {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = 64 << 10;
            rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
            rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            double t = qpc_ms();
            std::vector<ID3D12Resource *> rs(200);
            for (UINT i = 0; i < 200; i++)
                CHECK(p.device->CreatePlacedResource(placed_heap, (UINT64)i * (64 << 10), &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&rs[i])));
            double t1 = qpc_ms();
            for (auto *r : rs) r->Release();
            extras.add("create_us_per_resource", (t1 - t) * 1000.0 / 200);
            extras.add("release_us_per_resource", (qpc_ms() - t1) * 1000.0 / 200);
        } },
        { "descriptors", "5000 SRV/CBV writes and 5000 descriptor copies per frame", base, [](UINT) {
            double t = qpc_ms();
            for (UINT i = 0; i < 2500; i++)
            {
                p.device->CreateShaderResourceView(p.textures[i % 8], nullptr, staging_cpu(i));
                D3D12_CONSTANT_BUFFER_VIEW_DESC cbv = { p.upload_ring->GetGPUVirtualAddress() + i * 256, 256 };
                p.device->CreateConstantBufferView(&cbv, staging_cpu(2500 + i));
            }
            double t1 = qpc_ms();
            for (UINT i = 0; i < 5000; i++)
                p.device->CopyDescriptorsSimple(1, srv_cpu(4000 + i), staging_cpu(i), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            extras.add("us_per_view", (t1 - t) * 1000.0 / 5000);
            extras.add("us_per_copy", (qpc_ms() - t1) * 1000.0 / 5000);
        } },
        { "fence_sync", "8 mid-frame submissions, each waited on by the CPU (readback-style stalls)", [] {
            flat = create_pso({});
            for (int i = 0; i < 2; i++)
            {
                CHECK(p.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&extra_allocs[i])));
                CHECK(p.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, extra_allocs[i], nullptr, IID_PPV_ARGS(&extra_lists[i])));
                CHECK(extra_lists[i]->Close());
            }
        }, [](UINT f) {
            double wait = 0;
            for (UINT k = 0; k < 8; k++)
            {
                ID3D12GraphicsCommandList *l = extra_lists[0];
                CHECK(extra_allocs[0]->Reset());
                CHECK(l->Reset(extra_allocs[0], nullptr));
                auto h = rtv(FRAMES_IN_FLIGHT);
                barrier(l, p.hdr_targets[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                l->OMSetRenderTargets(1, &h, FALSE, nullptr);
                viewport(l);
                set_quad_state(l, flat);
                for (UINT i = 0; i < 200; i++) small_quad(l, i, f * 0.01f);
                barrier(l, p.hdr_targets[0], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                double t = qpc_ms();
                submit_and_wait(l);
                wait += qpc_ms() - t;
            }
            extras.add("submit_wait_ms_each", wait / 8);
        } },
        { "readback", "a 1080p target copied to a readback buffer, waited for and mapped each frame", [] {
            readback = create_buffer((UINT64)W * H * 8, D3D12_HEAP_TYPE_READBACK);
            CHECK(p.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&extra_allocs[0])));
            CHECK(p.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, extra_allocs[0], nullptr, IID_PPV_ARGS(&extra_lists[0])));
            CHECK(extra_lists[0]->Close());
        }, [](UINT) {
            ID3D12GraphicsCommandList *l = extra_lists[0];
            CHECK(extra_allocs[0]->Reset());
            CHECK(l->Reset(extra_allocs[0], nullptr));
            barrier(l, p.hdr_targets[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst = { readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
            dst.PlacedFootprint.Footprint = { HDR_FORMAT, W, H, 1, W * 8 };
            D3D12_TEXTURE_COPY_LOCATION src = { p.hdr_targets[0], D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
            l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            barrier(l, p.hdr_targets[0], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            double t = qpc_ms();
            submit_and_wait(l);
            double t1 = qpc_ms();
            void *cpu;
            D3D12_RANGE r = { 0, (SIZE_T)W * H * 8 };
            CHECK(readback->Map(0, &r, &cpu));
            volatile uint64_t sum = 0;
            for (UINT i = 0; i < W * H; i += 64) sum += ((uint64_t *)cpu)[i];
            readback->Unmap(0, nullptr);
            extras.add("copy_wait_ms", t1 - t);
            extras.add("map_read_ms", qpc_ms() - t1);
        } },
        { "many_lists", "40 command lists per frame, each executed separately", [] {
            flat = create_pso({});
            for (UINT i = 0; i < 40 * FRAMES_IN_FLIGHT; i++)
                CHECK(p.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&extra_allocs[i])));
            for (int i = 0; i < 40; i++)
            {
                CHECK(p.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, extra_allocs[i], nullptr, IID_PPV_ARGS(&extra_lists[i])));
                CHECK(extra_lists[i]->Close());
            }
        }, [](UINT f) {
            // Allocators are reused only after the frame's fence (the main loop waits for it).
            double t = qpc_ms();
            for (UINT k = 0; k < 40; k++)
            {
                ID3D12GraphicsCommandList *l = extra_lists[k];
                ID3D12CommandAllocator *a = extra_allocs[p.frame_index * 40 + k];
                CHECK(a->Reset());
                CHECK(l->Reset(a, nullptr));
                auto h = rtv(p.frame_index);
                l->OMSetRenderTargets(1, &h, FALSE, nullptr);
                viewport(l);
                set_quad_state(l, flat);
                for (UINT i = 0; i < 25; i++) small_quad(l, k * 25 + i, f * 0.01f);
                CHECK(l->Close());
                ID3D12CommandList *lists[] = { l };
                p.queue->ExecuteCommandLists(1, lists);
            }
            extras.add("us_per_list", (qpc_ms() - t) * 1000.0 / 40);
        } },
        { "queries", "1000 timestamp queries and an occlusion query per draw, resolved per frame", [] {
            flat = create_pso({});
            D3D12_QUERY_HEAP_DESC qh = { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 1000 };
            CHECK(p.device->CreateQueryHeap(&qh, IID_PPV_ARGS(&ts_heap)));
            qh = { D3D12_QUERY_HEAP_TYPE_OCCLUSION, 500 };
            CHECK(p.device->CreateQueryHeap(&qh, IID_PPV_ARGS(&occlusion_heap)));
            readback = create_buffer(1500 * 8, D3D12_HEAP_TYPE_READBACK);
        }, [](UINT f) {
            set_quad_state(p.list, flat);
            for (UINT i = 0; i < 500; i++)
            {
                p.list->EndQuery(ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, i * 2);
                p.list->BeginQuery(occlusion_heap, D3D12_QUERY_TYPE_BINARY_OCCLUSION, i);
                small_quad(p.list, i, f * 0.01f);
                p.list->EndQuery(occlusion_heap, D3D12_QUERY_TYPE_BINARY_OCCLUSION, i);
                p.list->EndQuery(ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, i * 2 + 1);
            }
            p.list->ResolveQueryData(ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, 0, 1000, readback, 0);
            p.list->ResolveQueryData(occlusion_heap, D3D12_QUERY_TYPE_BINARY_OCCLUSION, 0, 500, readback, 8000);
        } },
        { "timestamps", "1000 timestamp queries between draws, resolved per frame", [] {
            flat = create_pso({});
            D3D12_QUERY_HEAP_DESC qh = { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 1000 };
            CHECK(p.device->CreateQueryHeap(&qh, IID_PPV_ARGS(&ts_heap)));
            readback = create_buffer(1000 * 8, D3D12_HEAP_TYPE_READBACK);
        }, [](UINT f) {
            set_quad_state(p.list, flat);
            for (UINT i = 0; i < 1000; i++)
            {
                p.list->EndQuery(ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, i);
                small_quad(p.list, i, f * 0.01f);
            }
            p.list->ResolveQueryData(ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, 0, 1000, readback, 0);
        } },
        { "occlusion", "500 binary occlusion queries, one per draw, resolved per frame", [] {
            flat = create_pso({});
            D3D12_QUERY_HEAP_DESC qh = { D3D12_QUERY_HEAP_TYPE_OCCLUSION, 500 };
            CHECK(p.device->CreateQueryHeap(&qh, IID_PPV_ARGS(&occlusion_heap)));
            readback = create_buffer(500 * 8, D3D12_HEAP_TYPE_READBACK);
        }, [](UINT f) {
            set_quad_state(p.list, flat);
            for (UINT i = 0; i < 500; i++)
            {
                p.list->BeginQuery(occlusion_heap, D3D12_QUERY_TYPE_BINARY_OCCLUSION, i);
                small_quad(p.list, i, f * 0.01f);
                p.list->EndQuery(occlusion_heap, D3D12_QUERY_TYPE_BINARY_OCCLUSION, i);
            }
            p.list->ResolveQueryData(occlusion_heap, D3D12_QUERY_TYPE_BINARY_OCCLUSION, 0, 500, readback, 0);
        } },
        { "execute_indirect", "ExecuteIndirect of 2000 draws with root constants", [] {
            flat = create_pso({});
            D3D12_INDIRECT_ARGUMENT_DESC args[2] = {};
            args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
            args[0].Constant = { 0, 0, 4 };
            args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
            D3D12_COMMAND_SIGNATURE_DESC cs = { 32, 2, args, 0 };
            CHECK(p.device->CreateCommandSignature(&cs, p.root_signature, IID_PPV_ARGS(&draw_signature)));
            indirect_args = create_buffer(2000 * 32, D3D12_HEAP_TYPE_UPLOAD);
            float *a; CHECK(indirect_args->Map(0, nullptr, (void **)&a));
            for (UINT i = 0; i < 2000; i++)
            {
                float rc[4] = { (float)(i % 64) / 32.0f - 1.0f, (float)(i / 64 % 64) / 32.0f - 1.0f, 0.01f, 0.5f };
                memcpy(a + i * 8, rc, 16);
                uint32_t draw[4] = { 3, 1, 0, 0 };
                memcpy(a + i * 8 + 4, draw, 16);
            }
            indirect_args->Unmap(0, nullptr);
        }, [](UINT) {
            set_quad_state(p.list, flat);
            p.list->ExecuteIndirect(draw_signature, 2000, indirect_args, 0, nullptr, 0);
        } },
        { "fill", "20 full-screen quads with a heavy pixel shader at 1080p (GPU ALU)", [] {
            PsoDesc d; d.ps = "ps_heavy"; heavy = create_pso(d);
        }, [](UINT f) {
            set_quad_state(p.list, heavy);
            for (UINT i = 0; i < 20; i++)
            {
                float rc[4] = { 0, 0, 1, f * 0.01f + i };
                p.list->SetGraphicsRoot32BitConstants(0, 4, rc, 0);
                p.list->DrawInstanced(3, 1, 0, 0);
            }
        } },
        { "sample", "20 full-screen quads sampling 8 textures each at 1080p (GPU texture)", [] {
            PsoDesc d; d.ps = "ps_tex"; texd = create_pso(d);
        }, [](UINT f) {
            set_quad_state(p.list, texd);
            for (UINT i = 0; i < 20; i++)
            {
                float rc[4] = { 0, 0, 1, f * 0.01f };
                p.list->SetGraphicsRoot32BitConstants(0, 4, rc, 0);
                p.list->DrawInstanced(3, 1, 0, 0);
            }
        } },
        { "blend_depth", "40 full-screen blended quads with depth test at 1080p (GPU ROP)", [] {
            PsoDesc d; d.blend = true; d.depth = true; flat_depth = create_pso(d);
        }, [](UINT f) {
            auto h = rtv(p.frame_index);
            auto d = dsv();
            p.list->ClearDepthStencilView(d, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
            p.list->OMSetRenderTargets(1, &h, FALSE, &d);
            set_quad_state(p.list, flat_depth);
            for (UINT i = 0; i < 40; i++)
            {
                float rc[4] = { 0, 0, 1, 0.5f + f * 0.001f };
                p.list->SetGraphicsRoot32BitConstants(0, 4, rc, 0);
                p.list->DrawInstanced(3, 1, 0, 0);
            }
        } },
        { "msaa", "4x MSAA 1080p target: 200 draws and a resolve per frame", [] {
            PsoDesc d; d.rt = HDR_FORMAT; d.samples = 4; flat_msaa = create_pso(d);
            D3D12_CLEAR_VALUE cv = { HDR_FORMAT, { 0, 0, 0, 0 } };
            msaa_target = create_texture(W, H, HDR_FORMAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, 1, 4, &cv);
            p.device->CreateRenderTargetView(msaa_target, nullptr, rtv(FRAMES_IN_FLIGHT + 4));
            resolve_target = create_texture(W, H, HDR_FORMAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_RESOLVE_DEST);
        }, [](UINT f) {
            barrier(p.list, msaa_target, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            auto h = rtv(FRAMES_IN_FLIGHT + 4);
            p.list->OMSetRenderTargets(1, &h, FALSE, nullptr);
            float c[4] = { 0, 0, 0, 0 };
            p.list->ClearRenderTargetView(h, c, 0, nullptr);
            set_quad_state(p.list, flat_msaa);
            for (UINT i = 0; i < 200; i++) small_quad(p.list, i, f * 0.01f);
            barrier(p.list, msaa_target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
            p.list->ResolveSubresource(resolve_target, 0, msaa_target, 0, HDR_FORMAT);
            auto bb = rtv(p.frame_index);
            p.list->OMSetRenderTargets(1, &bb, FALSE, nullptr);
        } },
        { "compute", "2000 small dispatches with a UAV barrier each", [] {
            compute = create_compute_pso();
        }, [](UINT f) {
            p.list->SetComputeRootSignature(p.compute_root_signature);
            p.list->SetPipelineState(compute);
            p.list->SetComputeRootUnorderedAccessView(1, p.uav_buffer->GetGPUVirtualAddress());
            D3D12_RESOURCE_BARRIER b = {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            b.UAV.pResource = p.uav_buffer;
            double t = qpc_ms();
            for (UINT i = 0; i < 2000; i++)
            {
                uint32_t rc[4] = { i + f, 0, 0, 0 };
                p.list->SetComputeRoot32BitConstants(0, 4, rc, 0);
                p.list->Dispatch(1, 1, 1);
                p.list->ResourceBarrier(1, &b);
            }
            extras.add("record_us_per_dispatch", (qpc_ms() - t) * 1000.0 / 2000);
        } },
        { "pso_cold", "4 new graphics pipelines per frame from never-seen shaders (compile stutter)", base, [](UINT f) {
            double compile_ms = 0, create_ms = 0;
            for (UINT i = 0; i < 4; i++)
            {
                PsoDesc d; d.ps = (i & 1) ? "ps_heavy" : "ps_tex"; d.salt = p.salt * 100000 + f * 4 + i + 1;
                create_pso(d, &compile_ms, &create_ms)->Release();
            }
            extras.add("hlsl_compile_ms_per_pso", compile_ms / 4);
            extras.add("create_ms_per_pso", create_ms / 4);
        } },
        { "pso_warm", "the same 4 graphics pipelines re-created per frame (cache hits)", base, [](UINT) {
            double compile_ms = 0, create_ms = 0;
            for (UINT i = 0; i < 4; i++)
            {
                PsoDesc d; d.ps = (i & 1) ? "ps_heavy" : "ps_tex"; d.salt = 7 + i;
                create_pso(d, &compile_ms, &create_ms)->Release();
            }
            extras.add("create_ms_per_pso", create_ms / 4);
        } },
    };
}

// ---- scenes added for the typical shape of a Unity frame and the costs of tile-based GPUs ----

// Records upload work on the first allocator outside the frame loop and waits for it (scene init).
static void base_init_none() {}

static void run_init_commands(const std::function<void(ID3D12GraphicsCommandList *)> &record)
{
    CHECK(p.allocators[0]->Reset());
    CHECK(p.list->Reset(p.allocators[0], nullptr));
    record(p.list);
    submit_and_wait(p.list);
}

static void fullscreen(ID3D12GraphicsCommandList *l, float value)
{
    float rc[4] = { 0, 0, 1, value };
    l->SetGraphicsRoot32BitConstants(0, 4, rc, 0);
    l->DrawInstanced(3, 1, 0, 0);
}

static ID3D12Resource *create_depth(UINT w, UINT h, DXGI_FORMAT typeless, DXGI_FORMAT dsv_format, D3D12_RESOURCE_STATES state, UINT dsv_slot)
{
    D3D12_CLEAR_VALUE cv = { dsv_format };
    cv.DepthStencil.Depth = 1.0f;
    ID3D12Resource *r = create_texture(w, h, typeless, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, state, 1, 1, &cv);
    D3D12_DEPTH_STENCIL_VIEW_DESC dd = {};
    dd.Format = dsv_format;
    dd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    p.device->CreateDepthStencilView(r, &dd, dsv(dsv_slot));
    return r;
}

static std::vector<Scene> more_scenes()
{
    // Heap slots: 8-1007 texture copies (bindless/tables), 10000+ the resources below.
    enum { SRV_SHADOW = 10000, UAV_IMAGE = 10001, SRV_HDR = 10010, SRV_BC7 = 10020, SRV_RGBA8 = 10030, CPU_UAV_IMAGE = 15001 };
    static ID3D12PipelineState *flat, *shadow, *opaque, *post, *final_pso, *ui, *alpha, *sample1, *stencil_w, *stencil_t, *flat_hdr,
        *cimg, *inst, *dense, *bindless, *rs_psos[2], *texd;
    static ID3D12RootSignature *cimg_rs, *bindless_rs, *rs_pair[2];
    static ID3D12Resource *shadow_map, *image, *depth_stencil, *atlas, *bc7, *rgba8, *count_buffer, *args, *dst_buffer;
    static ID3D12DescriptorHeap *second_heap;
    static ID3D12CommandSignature *draw_sig;

    auto hdr_srvs = [] { for (int i = 0; i < 3; i++) p.device->CreateShaderResourceView(p.hdr_targets[i], nullptr, srv_cpu(SRV_HDR + i)); };
    auto texture_copies = [] { for (UINT i = 8; i < 8 + 1000; i++) p.device->CreateShaderResourceView(p.textures[i % 8], nullptr, srv_cpu(i)); };
    auto make_image = [] {
        image = create_texture(W, H, HDR_FORMAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        p.device->CreateUnorderedAccessView(image, nullptr, nullptr, srv_cpu(UAV_IMAGE));
        p.device->CreateUnorderedAccessView(image, nullptr, nullptr, staging_cpu(CPU_UAV_IMAGE));
    };
    auto make_cimg = [] {
        D3D12_DESCRIPTOR_RANGE uav = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0 };
        D3D12_DESCRIPTOR_RANGE srv = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
        D3D12_ROOT_PARAMETER rp[3] = {};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rp[0].Constants = { 0, 0, 4 };
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[1].DescriptorTable = { 1, &uav };
        rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[2].DescriptorTable = { 1, &srv };
        D3D12_ROOT_SIGNATURE_DESC rsd = { 3, rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        cimg_rs = serialize_root_signature(rsd);
        ID3DBlob *cs = compile(compute_image_hlsl, "cs_blur", "cs_5_0");
        D3D12_COMPUTE_PIPELINE_STATE_DESC c = {};
        c.pRootSignature = cimg_rs;
        c.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
        CHECK(p.device->CreateComputePipelineState(&c, IID_PPV_ARGS(&cimg)));
        cs->Release();
    };
    auto dispatch_image = [](ID3D12GraphicsCommandList *l, UINT count, UINT frame) {
        l->SetComputeRootSignature(cimg_rs);
        l->SetPipelineState(cimg);
        l->SetComputeRootDescriptorTable(1, srv_gpu(UAV_IMAGE));
        l->SetComputeRootDescriptorTable(2, srv_gpu(SRV_HDR));
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = image;
        for (UINT i = 0; i < count; i++)
        {
            float rc[4] = { frame * 0.001f, 0, 0, 0 };
            l->SetComputeRoot32BitConstants(0, 4, rc, 0);
            l->Dispatch(W / 8, H / 8, 1);
            l->ResourceBarrier(1, &b);
        }
    };
    auto make_shadow = [] {
        shadow_map = create_depth(2048, 2048, DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 1);
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_R32_FLOAT;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        p.device->CreateShaderResourceView(shadow_map, &sd, srv_cpu(SRV_SHADOW));
        PsoDesc d; d.ps = nullptr; d.num_rt = 0; d.depth = true; shadow = create_pso(d);
    };
    auto shadow_pass = [](ID3D12GraphicsCommandList *l, UINT draws, UINT frame) {
        barrier(l, shadow_map, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        auto d = dsv(1);
        l->OMSetRenderTargets(0, nullptr, FALSE, &d);
        l->ClearDepthStencilView(d, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        viewport(l, 2048, 2048);
        set_quad_state(l, shadow);
        for (UINT i = 0; i < draws; i++)
        {
            float rc[4] = { (float)(i % 64) / 32.0f - 1.0f, (float)(i / 64 % 64) / 32.0f - 1.0f, 0.04f, frame * 0.01f };
            l->SetGraphicsRoot32BitConstants(0, 4, rc, 0);
            l->DrawInstanced(3, 1, 0, 0);
        }
        barrier(l, shadow_map, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        viewport(l);
    };
    auto back_to_backbuffer = [](ID3D12GraphicsCommandList *l) {
        auto bb = rtv(p.frame_index);
        l->OMSetRenderTargets(1, &bb, FALSE, nullptr);
        viewport(l);
    };
    // One full-screen pass from target src (or the backbuffer if dst < 0) into hdr target dst.
    auto post_pass = [back_to_backbuffer](ID3D12GraphicsCommandList *l, ID3D12PipelineState *pso, int src, int dst, float value) {
        if (dst >= 0)
        {
            barrier(l, p.hdr_targets[dst], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            auto h = rtv(FRAMES_IN_FLIGHT + dst);
            l->OMSetRenderTargets(1, &h, FALSE, nullptr);
        }
        else
            back_to_backbuffer(l);
        set_quad_state(l, pso);
        l->SetGraphicsRootDescriptorTable(2, srv_gpu(SRV_HDR + src));
        fullscreen(l, value);
        if (dst >= 0)
            barrier(l, p.hdr_targets[dst], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    };
    auto make_bc = [] {
        // BC7 and RGBA8 1024x1024 textures with the same footprint in texels, 8 SRVs each.
        bc7 = create_texture(1024, 1024, DXGI_FORMAT_BC7_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        rgba8 = create_texture(1024, 1024, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        run_init_commands([](ID3D12GraphicsCommandList *l) {
            void *cpu;
            D3D12_GPU_VIRTUAL_ADDRESS va = upload_alloc(1024 * 1024 * 4, &cpu);
            uint32_t *w = (uint32_t *)cpu;
            for (UINT i = 0; i < 1024 * 1024; i++) w[i] = i * 2654435761u;
            D3D12_TEXTURE_COPY_LOCATION src = { p.upload_ring, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
            src.PlacedFootprint.Offset = va - p.upload_ring->GetGPUVirtualAddress();
            src.PlacedFootprint.Footprint = { DXGI_FORMAT_BC7_UNORM, 1024, 1024, 1, 4096 };
            D3D12_TEXTURE_COPY_LOCATION dst = { bc7, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
            l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, 1024, 1024, 1, 4096 };
            dst.pResource = rgba8;
            l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            barrier(l, bc7, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            barrier(l, rgba8, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        });
        for (UINT i = 0; i < 8; i++)
        {
            p.device->CreateShaderResourceView(bc7, nullptr, srv_cpu(SRV_BC7 + i));
            p.device->CreateShaderResourceView(rgba8, nullptr, srv_cpu(SRV_RGBA8 + i));
        }
        PsoDesc d; d.ps = "ps_tex"; texd = create_pso(d);
    };
    auto sample_frame = [](UINT base, UINT f) {
        set_quad_state(p.list, texd);
        p.list->SetGraphicsRootDescriptorTable(2, srv_gpu(base));
        for (UINT i = 0; i < 20; i++) fullscreen(p.list, f * 0.01f);
    };

    return {
        { "unity_frame", "a Unity-shaped frame: 400-draw shadow map, 800 opaque draws with a root CBV each, 4 post passes, "
                         "3 compute passes with UAV barriers, composite and 300 UI quads", [make_shadow, hdr_srvs, make_image, make_cimg, texture_copies] {
            make_shadow(); hdr_srvs(); make_image(); make_cimg(); texture_copies();
            PsoDesc d; d.rt = HDR_FORMAT; d.depth = true; d.ps = "ps_tex"; opaque = create_pso(d);
            d = {}; d.rt = HDR_FORMAT; d.ps = "ps_post"; post = create_pso(d);
            d = {}; d.ps = "ps_post"; final_pso = create_pso(d);
            d = {}; d.ps = "ps_ui"; d.blend = true; ui = create_pso(d);
        }, [shadow_pass, post_pass, dispatch_image, back_to_backbuffer](UINT f) {
            double t = qpc_ms();
            ID3D12GraphicsCommandList *l = p.list;
            shadow_pass(l, 400, f);
            // Opaque pass into hdr target 0 with depth; a fresh root CBV per draw, as Unity binds per-object data.
            barrier(l, p.hdr_targets[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            auto h = rtv(FRAMES_IN_FLIGHT);
            auto d = dsv(0);
            l->OMSetRenderTargets(1, &h, FALSE, &d);
            float c[4] = { 0, 0, 0, 0 };
            l->ClearRenderTargetView(h, c, 0, nullptr);
            l->ClearDepthStencilView(d, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
            set_quad_state(l, opaque);
            for (UINT i = 0; i < 800; i++)
            {
                void *cpu;
                D3D12_GPU_VIRTUAL_ADDRESS cb = upload_alloc(256, &cpu);
                ((float *)cpu)[4] = (float)i;
                l->SetGraphicsRootConstantBufferView(1, cb);
                if (i % 16 == 0) l->SetGraphicsRootDescriptorTable(2, srv_gpu(8 + (i / 16) * 8 % 992));
                small_quad(l, i, f * 0.01f);
            }
            barrier(l, p.hdr_targets[0], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            // Post chain: 0 -> 1 -> 2 -> 1 -> 2.
            post_pass(l, post, 0, 1, f * 0.01f);
            post_pass(l, post, 1, 2, f * 0.01f);
            post_pass(l, post, 2, 1, f * 0.01f);
            post_pass(l, post, 1, 2, f * 0.01f);
            dispatch_image(l, 3, f);
            // Composite into the backbuffer, then UI.
            post_pass(l, final_pso, 2, -1, f * 0.01f);
            set_quad_state(l, ui);
            for (UINT i = 0; i < 300; i++)
            {
                if (i % 10 == 0) l->SetGraphicsRootDescriptorTable(2, srv_gpu(8 + i % 1000));
                small_quad(l, i, 1.0f);
            }
            back_to_backbuffer(l);
            extras.add("record_ms", qpc_ms() - t);
        } },
        { "depth_only", "2000 draws into a 2048x2048 shadow map with no pixel shader", make_shadow, [shadow_pass](UINT f) {
            shadow_pass(p.list, 2000, f);
        } },
        { "overdraw_opaque", "50 full-screen depth-tested quads sampling a texture (baseline for alpha_test)", [] {
            PsoDesc d; d.ps = "ps_sample1"; d.depth = true; d.depth_write = false; sample1 = create_pso(d);
        }, [](UINT f) {
            auto h = rtv(p.frame_index);
            auto d = dsv(0);
            p.list->ClearDepthStencilView(d, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
            p.list->OMSetRenderTargets(1, &h, FALSE, &d);
            set_quad_state(p.list, sample1);
            for (UINT i = 0; i < 50; i++) fullscreen(p.list, 1.0f);
        } },
        { "alpha_test", "50 full-screen depth-tested quads with discard (Unity cutout shaders)", [] {
            PsoDesc d; d.ps = "ps_alpha"; d.depth = true; d.depth_write = false; alpha = create_pso(d);
        }, [](UINT f) {
            auto h = rtv(p.frame_index);
            auto d = dsv(0);
            p.list->ClearDepthStencilView(d, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
            p.list->OMSetRenderTargets(1, &h, FALSE, &d);
            set_quad_state(p.list, alpha);
            for (UINT i = 0; i < 50; i++) fullscreen(p.list, 1.0f);
        } },
        { "stencil", "200 quads writing stencil, then 20 full-screen quads tested against it (D24S8)", [] {
            depth_stencil = create_depth(W, H, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_D24_UNORM_S8_UINT, D3D12_RESOURCE_STATE_DEPTH_WRITE, 2);
            PsoDesc d; d.stencil_write = true; d.dsv = DXGI_FORMAT_D24_UNORM_S8_UINT; stencil_w = create_pso(d);
            d = {}; d.stencil_test = true; d.dsv = DXGI_FORMAT_D24_UNORM_S8_UINT; d.ps = "ps_heavy"; stencil_t = create_pso(d);
        }, [](UINT f) {
            auto h = rtv(p.frame_index);
            auto d = dsv(2);
            p.list->ClearDepthStencilView(d, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
            p.list->OMSetRenderTargets(1, &h, FALSE, &d);
            p.list->OMSetStencilRef(1);
            set_quad_state(p.list, stencil_w);
            for (UINT i = 0; i < 200; i++)
            {
                float rc[4] = { (float)(i % 16) / 8.0f - 0.9f, (float)(i / 16) / 8.0f - 0.9f, 0.05f, 0.2f };
                p.list->SetGraphicsRoot32BitConstants(0, 4, rc, 0);
                p.list->DrawInstanced(3, 1, 0, 0);
            }
            set_quad_state(p.list, stencil_t);
            for (UINT i = 0; i < 20; i++) fullscreen(p.list, f * 0.01f);
        } },
        { "postfx_chain", "10 full-screen post passes ping-ponging two 1080p HDR targets", [hdr_srvs] {
            hdr_srvs();
            PsoDesc d; d.rt = HDR_FORMAT; d.ps = "ps_post"; post = create_pso(d);
        }, [post_pass, back_to_backbuffer](UINT f) {
            double t = qpc_ms();
            for (int i = 0; i < 10; i++) post_pass(p.list, post, 1 + (i + 1) % 2, 1 + i % 2, f * 0.01f);
            back_to_backbuffer(p.list);
            extras.add("record_us_per_pass", (qpc_ms() - t) * 100.0);
        } },
        { "compute_image", "10 compute passes over a 1080p RWTexture2D with UAV barriers (compute post-processing)", [make_image, make_cimg, hdr_srvs] {
            make_image(); make_cimg(); hdr_srvs();
        }, [dispatch_image](UINT f) {
            dispatch_image(p.list, 10, f);
        } },
        { "clear_uav", "10 ClearUnorderedAccessViewFloat calls on a 1080p texture", make_image, [](UINT f) {
            float v[4] = { f * 0.001f, 0, 0, 1 };
            D3D12_RESOURCE_BARRIER b = {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            b.UAV.pResource = image;
            for (UINT i = 0; i < 10; i++)
            {
                p.list->ClearUnorderedAccessViewFloat(srv_gpu(UAV_IMAGE), staging_cpu(CPU_UAV_IMAGE), image, v, 0, nullptr);
                p.list->ResourceBarrier(1, &b);
            }
        } },
        { "render_pass_api", "50 D3D12 render passes (BeginRenderPass) of 6 draws, clear or discard on load, preserve on store", [] {
            PsoDesc d; d.rt = HDR_FORMAT; flat_hdr = create_pso(d);
        }, [back_to_backbuffer](UINT f) {
            ID3D12GraphicsCommandList4 *l4 = nullptr;
            CHECK(p.list->QueryInterface(IID_PPV_ARGS(&l4)));
            for (UINT pass = 0; pass < 50; pass++)
            {
                ID3D12Resource *target = p.hdr_targets[pass % 3];
                barrier(p.list, target, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                D3D12_RENDER_PASS_RENDER_TARGET_DESC rt = {};
                rt.cpuDescriptor = rtv(FRAMES_IN_FLIGHT + pass % 3);
                rt.BeginningAccess.Type = pass & 1 ? D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_DISCARD : D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
                rt.BeginningAccess.Clear.ClearValue.Format = HDR_FORMAT;
                rt.EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
                l4->BeginRenderPass(1, &rt, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
                set_quad_state(p.list, flat_hdr);
                for (UINT i = 0; i < 6; i++) small_quad(p.list, pass * 6 + i, f * 0.01f);
                l4->EndRenderPass();
                barrier(p.list, target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
            l4->Release();
            back_to_backbuffer(p.list);
        } },
        { "discard_resource", "50 passes, each preceded by DiscardResource on its target (should load nothing)", [] {
            PsoDesc d; d.rt = HDR_FORMAT; flat_hdr = create_pso(d);
        }, [back_to_backbuffer](UINT f) {
            for (UINT pass = 0; pass < 50; pass++)
            {
                ID3D12Resource *target = p.hdr_targets[pass % 3];
                barrier(p.list, target, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                p.list->DiscardResource(target, nullptr);
                auto h = rtv(FRAMES_IN_FLIGHT + pass % 3);
                p.list->OMSetRenderTargets(1, &h, FALSE, nullptr);
                set_quad_state(p.list, flat_hdr);
                for (UINT i = 0; i < 6; i++) small_quad(p.list, pass * 6 + i, f * 0.01f);
                barrier(p.list, target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
            back_to_backbuffer(p.list);
        } },
        { "bindless", "2000 draws indexing a 1000-texture unbounded table per draw (SM 5.1 dynamic indexing)", [texture_copies] {
            texture_copies();
            D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 1, 0 };
            D3D12_ROOT_PARAMETER rp[2] = {};
            rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            rp[0].Constants = { 0, 0, 8 };
            rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            rp[1].DescriptorTable = { 1, &range };
            D3D12_STATIC_SAMPLER_DESC ss = {};
            ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            ss.MaxLOD = D3D12_FLOAT32_MAX;
            D3D12_ROOT_SIGNATURE_DESC rsd = { 2, rp, 1, &ss, D3D12_ROOT_SIGNATURE_FLAG_NONE };
            bindless_rs = serialize_root_signature(rsd);
            PsoDesc d; d.hlsl = bindless_hlsl; d.ps = "ps_bindless"; d.vs_target = "vs_5_1"; d.ps_target = "ps_5_1"; d.rs = bindless_rs;
            bindless = create_pso(d);
        }, [](UINT f) {
            p.list->SetGraphicsRootSignature(bindless_rs);
            p.list->SetPipelineState(bindless);
            p.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            p.list->SetGraphicsRootDescriptorTable(1, srv_gpu(8));
            double t = qpc_ms();
            for (UINT i = 0; i < 2000; i++)
            {
                uint32_t rc[8];
                float pos[4] = { (float)(i % 64) / 32.0f - 1.0f, (float)(i / 64 % 64) / 32.0f - 1.0f, 0.02f, f * 0.01f };
                memcpy(rc, pos, 16);
                rc[4] = i % 1000; rc[5] = rc[6] = rc[7] = 0;
                p.list->SetGraphicsRoot32BitConstants(0, 8, rc, 0);
                p.list->DrawInstanced(3, 1, 0, 0);
            }
            extras.add("record_us_per_draw", (qpc_ms() - t) * 1000.0 / 2000);
        } },
        { "root_signature_switch", "3000 draws alternating two root signatures (and their pipelines), rebinding all parameters", [] {
            D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 8, 0, 0, 0 };
            for (int k = 0; k < 2; k++)
            {
                D3D12_ROOT_PARAMETER rp[3] = {};
                rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
                rp[0].Constants = { 0, 0, 4u + k * 4 };
                rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
                rp[1].Descriptor = { 1, 0 };
                rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                rp[2].DescriptorTable = { 1, &range };
                D3D12_STATIC_SAMPLER_DESC ss = {};
                ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
                ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
                ss.MaxLOD = D3D12_FLOAT32_MAX;
                D3D12_ROOT_SIGNATURE_DESC rsd = { 3, rp, 1, &ss, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
                rs_pair[k] = serialize_root_signature(rsd);
                PsoDesc d; d.rs = rs_pair[k]; d.blend = k; rs_psos[k] = create_pso(d);
            }
        }, [](UINT f) {
            p.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            void *cpu;
            D3D12_GPU_VIRTUAL_ADDRESS cb = upload_alloc(256, &cpu);
            double t = qpc_ms();
            for (UINT i = 0; i < 3000; i++)
            {
                int k = i & 1;
                p.list->SetGraphicsRootSignature(rs_pair[k]);
                p.list->SetPipelineState(rs_psos[k]);
                p.list->SetGraphicsRootConstantBufferView(1, cb);
                p.list->SetGraphicsRootDescriptorTable(2, srv_gpu(0));
                small_quad(p.list, i, f * 0.01f);
            }
            extras.add("record_us_per_draw", (qpc_ms() - t) * 1000.0 / 3000);
        } },
        { "heap_switch", "2000 draws alternating two shader-visible descriptor heaps", [] {
            flat = nullptr;
            D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 64, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
            CHECK(p.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&second_heap)));
            auto h = second_heap->GetCPUDescriptorHandleForHeapStart();
            for (UINT i = 0; i < 8; i++, h.ptr += p.srv_size) p.device->CreateShaderResourceView(p.textures[7 - i], nullptr, h);
            PsoDesc d; d.ps = "ps_tex"; texd = create_pso(d);
        }, [](UINT f) {
            set_quad_state(p.list, texd);
            double t = qpc_ms();
            for (UINT i = 0; i < 2000; i++)
            {
                ID3D12DescriptorHeap *heap = (i & 1) ? second_heap : p.srv_heap;
                p.list->SetDescriptorHeaps(1, &heap);
                p.list->SetGraphicsRootDescriptorTable(2, heap->GetGPUDescriptorHandleForHeapStart());
                small_quad(p.list, i, f * 0.01f);
            }
            ID3D12DescriptorHeap *heaps[] = { p.srv_heap };
            p.list->SetDescriptorHeaps(1, heaps);
            extras.add("record_us_per_draw", (qpc_ms() - t) * 1000.0 / 2000);
        } },
        { "ui_quads", "4000 small blended, textured quads, a texture change every 8", [texture_copies] {
            texture_copies();
            PsoDesc d; d.ps = "ps_ui"; d.blend = true; ui = create_pso(d);
        }, [](UINT f) {
            set_quad_state(p.list, ui);
            double t = qpc_ms();
            for (UINT i = 0; i < 4000; i++)
            {
                if (i % 8 == 0) p.list->SetGraphicsRootDescriptorTable(2, srv_gpu(8 + (i / 8) % 992));
                small_quad(p.list, i, 1.0f);
            }
            extras.add("record_us_per_draw", (qpc_ms() - t) * 1000.0 / 4000);
        } },
        { "small_copies", "1000 256-byte CopyBufferRegion uploads per frame (per-object constant uploads)", [] {
            dst_buffer = create_buffer(1000 * 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        }, [](UINT f) {
            double t = qpc_ms();
            for (UINT i = 0; i < 1000; i++)
            {
                void *cpu;
                D3D12_GPU_VIRTUAL_ADDRESS va = upload_alloc(256, &cpu);
                ((uint32_t *)cpu)[0] = f + i;
                p.list->CopyBufferRegion(dst_buffer, i * 256, p.upload_ring, va - p.upload_ring->GetGPUVirtualAddress(), 256);
            }
            extras.add("record_us_per_copy", (qpc_ms() - t));
        } },
        { "small_texture_updates", "200 64x64 CopyTextureRegion updates into a 1024x1024 atlas per frame (fonts, UI)", [] {
            atlas = create_texture(1024, 1024, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }, [](UINT f) {
            barrier(p.list, atlas, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            for (UINT i = 0; i < 200; i++)
            {
                void *cpu;
                D3D12_GPU_VIRTUAL_ADDRESS va = upload_alloc(64 * 64 * 4, &cpu);
                memset(cpu, (f + i) & 0xff, 64 * 64 * 4);
                D3D12_TEXTURE_COPY_LOCATION src = { p.upload_ring, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                src.PlacedFootprint.Offset = va - p.upload_ring->GetGPUVirtualAddress();
                src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 256 };
                D3D12_TEXTURE_COPY_LOCATION dst = { atlas, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                p.list->CopyTextureRegion(&dst, (i % 16) * 64, (i / 16 % 16) * 64, 0, &src, nullptr);
            }
            barrier(p.list, atlas, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        } },
        { "copy_resource", "10 CopyResource calls of a 1080p HDR target", base_init_none, [](UINT) {
            barrier(p.list, p.hdr_targets[1], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
            barrier(p.list, p.hdr_targets[2], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            for (UINT i = 0; i < 10; i++) p.list->CopyResource(p.hdr_targets[2], p.hdr_targets[1]);
            barrier(p.list, p.hdr_targets[1], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            barrier(p.list, p.hdr_targets[2], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        } },
        { "big_resources", "4 64 MiB textures (4096x4096 RGBA8) created and released per frame", base_init_none, [](UINT) {
            double t = qpc_ms();
            ID3D12Resource *r[4];
            for (int i = 0; i < 4; i++)
                r[i] = create_texture(4096, 4096, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
            double t1 = qpc_ms();
            for (int i = 0; i < 4; i++) r[i]->Release();
            extras.add("create_ms_per_resource", (t1 - t) / 4);
            extras.add("release_ms_per_resource", (qpc_ms() - t1) / 4);
        } },
        { "execute_indirect_count", "ExecuteIndirect with a count buffer: up to 4000 draws, 2000 executed", [] {
            flat = create_pso({});
            D3D12_INDIRECT_ARGUMENT_DESC a[2] = {};
            a[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
            a[0].Constant = { 0, 0, 4 };
            a[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
            D3D12_COMMAND_SIGNATURE_DESC cs = { 32, 2, a, 0 };
            CHECK(p.device->CreateCommandSignature(&cs, p.root_signature, IID_PPV_ARGS(&draw_sig)));
            args = create_buffer(4000 * 32, D3D12_HEAP_TYPE_UPLOAD);
            float *v; CHECK(args->Map(0, nullptr, (void **)&v));
            for (UINT i = 0; i < 4000; i++)
            {
                float rc[4] = { (float)(i % 64) / 32.0f - 1.0f, (float)(i / 64 % 64) / 32.0f - 1.0f, 0.01f, 0.5f };
                memcpy(v + i * 8, rc, 16);
                uint32_t draw[4] = { 3, 1, 0, 0 };
                memcpy(v + i * 8 + 4, draw, 16);
            }
            args->Unmap(0, nullptr);
            count_buffer = create_buffer(256, D3D12_HEAP_TYPE_UPLOAD);
            uint32_t *c; CHECK(count_buffer->Map(0, nullptr, (void **)&c));
            *c = 2000;
            count_buffer->Unmap(0, nullptr);
        }, [](UINT) {
            set_quad_state(p.list, flat);
            p.list->ExecuteIndirect(draw_sig, 4000, args, 0, count_buffer, 0);
        } },
        { "instancing", "one draw of 160000 small instances (vertex/instance throughput)", [] {
            PsoDesc d; d.vs = "vs_instanced"; inst = create_pso(d);
        }, [](UINT) {
            set_quad_state(p.list, inst);
            p.list->DrawInstanced(3, 160000, 0, 0);
        } },
        { "vertex_heavy", "one draw of 1M tiny triangles (vertex-bound geometry)", [] {
            PsoDesc d; d.vs = "vs_dense"; dense = create_pso(d);
        }, [](UINT f) {
            set_quad_state(p.list, dense);
            float rc[4] = { 0, 0, 1, f * 0.01f };
            p.list->SetGraphicsRoot32BitConstants(0, 4, rc, 0);
            p.list->DrawInstanced(3 * 1048576, 1, 0, 0);
        } },
        { "sample_bc7", "20 full-screen quads sampling 8 BC7 textures each (compressed bandwidth)", make_bc, [sample_frame](UINT f) { sample_frame(SRV_BC7, f); } },
        { "sample_rgba8", "the same with RGBA8 textures (baseline for sample_bc7)", make_bc, [sample_frame](UINT f) { sample_frame(SRV_RGBA8, f); } },
        { "present_vsync", "clear + Present(1): the display-locked frame", [] {}, [](UINT) {}, true, 1 },
    };
}

static std::vector<Scene> scenes()
{
    std::vector<Scene> all = base_scenes(), more = more_scenes();
    all.insert(all.end(), more.begin(), more.end());
    return all;
}

// ---- main loop ----

static double percentile(std::vector<double> v, double q)
{
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, (size_t)(q * (v.size() - 1) + 0.5))];
}

static void report(const char *what, const std::vector<double> &v)
{
    double sum = 0, mx = 0;
    for (double x : v) { sum += x; mx = std::max(mx, x); }
    printf("%s frames %zu mean %.3f median %.3f p95 %.3f max %.3f ms\n", what, v.size(), v.empty() ? 0 : sum / v.size(),
           percentile(v, 0.5), percentile(v, 0.95), mx);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2 || !strcmp(argv[1], "list"))
    {
        // Listing needs no device: the scenes' functions only run when selected.
        for (const Scene &s : scenes()) printf("%-18s %s\n", s.name, s.desc);
        return argc < 2 ? 1 : 0;
    }
    UINT frames = argc > 2 ? atoi(argv[2]) : 200, warmup = argc > 3 ? atoi(argv[3]) : 30;
    p.salt = argc > 4 ? atoi(argv[4]) : GetTickCount();

    std::vector<Scene> all = scenes();
    const Scene *scene = nullptr;
    for (const Scene &s : all) if (!strcmp(s.name, argv[1])) scene = &s;
    if (!scene) { printf("unknown scene %s\n", argv[1]); return 2; }

    setup();
    double t = qpc_ms();
    scene->init();
    printf("extra init_ms=%.3f\n", qpc_ms() - t);

    std::vector<double> cpu, gpu, gpu_ts, work;
    double first = 0, last_present = 0;
    // Timed frames, then serialized frames: each is submitted and waited for, and its
    // submit-to-completion time is the "gpu" figure (independent of timestamp queries).
    const UINT serialized = 60;
    for (UINT f = 0; f < warmup + frames + serialized; f++)
    {
        bool timed = f >= warmup && f < warmup + frames, sync = f >= warmup + frames;
        if (f == warmup + frames)
            wait_fence(p.fence_value);
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        if (scene->present)
            WaitForSingleObjectEx(p.waitable, 1000, TRUE);
        double frame_start = qpc_ms();
        p.frame_index = scene->present ? p.swapchain->GetCurrentBackBufferIndex() : f % FRAMES_IN_FLIGHT;
        UINT i = p.frame_index;
        wait_fence(p.frame_fence[i]);
        if (p.frame_fence[i] && timed && f > warmup + FRAMES_IN_FLIGHT)
        {
            uint64_t *ts;
            D3D12_RANGE r = { i * 16, i * 16 + 16 };
            CHECK(p.timestamp_readback->Map(0, &r, (void **)&ts));
            static int dumped;
            if (getenv("PROBE_DEBUG_TS") && dumped++ < 5)
                printf("ts slot %u: %llu %llu\n", i, (unsigned long long)ts[i * 2], (unsigned long long)ts[i * 2 + 1]);
            if (ts[i * 2 + 1] > ts[i * 2])
                gpu_ts.push_back(1000.0 * (double)(ts[i * 2 + 1] - ts[i * 2]) / (double)p.timestamp_frequency);
            p.timestamp_readback->Unmap(0, nullptr);
        }
        CHECK(p.allocators[i]->Reset());
        CHECK(p.list->Reset(p.allocators[i], nullptr));
        ID3D12DescriptorHeap *heaps[] = { p.srv_heap };
        p.list->SetDescriptorHeaps(1, heaps);
        p.list->EndQuery(p.timestamps, D3D12_QUERY_TYPE_TIMESTAMP, i * 2);
        barrier(p.list, p.backbuffers[i], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        auto h = rtv(i);
        float clear[4] = { 0.1f, 0.1f, 0.12f, 1 };
        p.list->ClearRenderTargetView(h, clear, 0, nullptr);
        p.list->OMSetRenderTargets(1, &h, FALSE, nullptr);
        viewport(p.list);
        scene->frame(f);
        barrier(p.list, p.backbuffers[i], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        p.list->EndQuery(p.timestamps, D3D12_QUERY_TYPE_TIMESTAMP, i * 2 + 1);
        p.list->ResolveQueryData(p.timestamps, D3D12_QUERY_TYPE_TIMESTAMP, i * 2, 2, p.timestamp_readback, i * 16);
        CHECK(p.list->Close());
        ID3D12CommandList *lists[] = { p.list };
        double submit = qpc_ms();
        p.queue->ExecuteCommandLists(1, lists);
        p.frame_fence[i] = signal_queue();
        if (sync)
        {
            wait_fence(p.frame_fence[i]);
            gpu.push_back(qpc_ms() - submit);
        }
        if (timed)
            work.push_back(qpc_ms() - frame_start);
        if (scene->present)
            CHECK(p.swapchain->Present(scene->sync_interval, scene->sync_interval ? 0 : DXGI_PRESENT_ALLOW_TEARING));
        double now = qpc_ms();
        if (f == 0) first = now - t;
        else if (timed) cpu.push_back(now - last_present);
        last_present = now;
        if (f + 1 == warmup) extras.values.clear();
        if (f + 1 == warmup + frames) { for (auto &e : extras.values) e.second /= frames; extras.frozen = true; }
    }
    wait_fence(p.fence_value);

    report("cpu", cpu);
    report("work", work);
    report("gpu", gpu);
    if (!gpu_ts.empty()) report("gputs", gpu_ts);
    printf("first %.3f ms\n", first);
    // Extras are per frame over the timed frames (accumulation during the serialized frames is dropped).
    for (auto &e : extras.values) printf("extra %s=%.4f\n", e.first.c_str(), e.second);
    printf("D3D12_PERF_PROBE_OK %s\n", scene->name);
    return 0;
}
