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
static D3D12_CPU_DESCRIPTOR_HANDLE dsv() { return p.dsv_heap->GetCPUDescriptorHandleForHeapStart(); }
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
    DXGI_FORMAT rt = BACKBUFFER_FORMAT;
    bool depth = false, blend = false, input = false;
    UINT samples = 1;
    unsigned salt = 0;
};

static ID3D12PipelineState *create_pso(const PsoDesc &d, double *compile_ms = nullptr, double *create_ms = nullptr)
{
    double t0 = qpc_ms();
    std::string src = d.salt ? salted(common_hlsl, d.salt) : std::string(common_hlsl);
    ID3DBlob *vs = compile(src, d.vs, "vs_5_0"), *ps = compile(src, d.ps, "ps_5_0");
    double t1 = qpc_ms();
    D3D12_GRAPHICS_PIPELINE_STATE_DESC g = {};
    g.pRootSignature = p.root_signature;
    g.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
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
    if (d.depth)
    {
        g.DepthStencilState.DepthEnable = TRUE;
        g.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        g.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        g.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    }
    static const D3D12_INPUT_ELEMENT_DESC elements[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    if (d.input)
        g.InputLayout = { elements, 2 };
    g.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    g.NumRenderTargets = 1;
    g.RTVFormats[0] = d.rt;
    g.SampleDesc.Count = d.samples;
    ID3D12PipelineState *pso = nullptr;
    CHECK(p.device->CreateGraphicsPipelineState(&g, IID_PPV_ARGS(&pso)));
    double t2 = qpc_ms();
    vs->Release();
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
    hd.NumDescriptors = FRAMES_IN_FLIGHT + 8;
    CHECK(p.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&p.rtv_heap)));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    hd.NumDescriptors = 1;
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
    bool present = false;   // Only the present scene paces by the swap chain; the rest run offscreen.
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

static std::vector<Scene> scenes()
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
    const UINT serialized = 20;
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
            CHECK(p.swapchain->Present(0, DXGI_PRESENT_ALLOW_TEARING));
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
