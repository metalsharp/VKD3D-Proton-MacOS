// D3D12 functional tests for VKD3D-Proton on MoltenVK (run under MetalSharp Wine).
// Shaders are compiled at run time: SM 6.x with dxcompiler.dll, SM 5.x with d3dcompiler_47.dll.
//   d3d12test.exe [test-name ...]     (no arguments runs every test)
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <dxcapi.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

static int g_failures, g_checks;
#define CHECK_HR(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("  %s:%d: %s = 0x%08lx\n", __FILE__, __LINE__, #x, (unsigned long)hr_); return false; } } while (0)
#define EXPECT(cond, ...) do { g_checks++; if (!(cond)) { if (g_failures++ < 40) { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } } while (0)

struct Context
{
    IDXGIFactory4 *factory = nullptr;
    IDXGIAdapter1 *adapter = nullptr;
    ID3D12Device *device = nullptr;
    ID3D12CommandQueue *queue = nullptr;
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *list = nullptr;
    ID3D12Fence *fence = nullptr;
    UINT64 fence_value = 0;
    HANDLE event = nullptr;
    D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
};
static Context g;

// ---- shader compilation ----

// DXIL cache (D3D12TEST_CACHE=<dir>): a run with dxcompiler.dll (the native arm64 build, with
// MetalSharp Wine's arm64 dxcompiler) fills it, and x86_64 runs without a dxcompiler read from it.
static std::string cache_path(const char *source, const char *entry, const char *target, const wchar_t *const *extra, int extra_count)
{
    char dir[MAX_PATH];
    if (!GetEnvironmentVariableA("D3D12TEST_CACHE", dir, sizeof(dir)))
        return {};
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](const void *p, size_t n) { for (size_t i = 0; i < n; i++) h = (h ^ ((const unsigned char *)p)[i]) * 1099511628211ull; };
    mix(source, strlen(source)); mix(entry, strlen(entry)); mix(target, strlen(target));
    for (int i = 0; i < extra_count; i++)
        mix(extra[i], wcslen(extra[i]) * sizeof(wchar_t));
    char name[64];
    snprintf(name, sizeof(name), "\\%016llx.dxil", (unsigned long long)h);
    return std::string(dir) + name;
}

static std::vector<char> compile_dxc_uncached(const char *source, const char *entry, const char *target, const wchar_t *const *extra, int extra_count);

static std::vector<char> compile_dxc(const char *source, const char *entry, const char *target, const wchar_t *const *extra = nullptr, int extra_count = 0)
{
    std::string path = cache_path(source, entry, target, extra, extra_count);
    if (!path.empty())
    {
        if (FILE *f = fopen(path.c_str(), "rb"))
        {
            std::vector<char> out;
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)))
                out.insert(out.end(), buf, buf + n);
            fclose(f);
            return out;
        }
    }
    std::vector<char> out = compile_dxc_uncached(source, entry, target, extra, extra_count);
    if (!out.empty() && !path.empty())
        if (FILE *f = fopen(path.c_str(), "wb"))
        {
            fwrite(out.data(), 1, out.size(), f);
            fclose(f);
        }
    return out;
}

static std::vector<char> compile_dxc_uncached(const char *source, const char *entry, const char *target, const wchar_t *const *extra, int extra_count)
{
    static DxcCreateInstanceProc create;
    if (!create)
    {
        HMODULE m = LoadLibraryA("dxcompiler.dll");
        if (!m)
        {
            char path[MAX_PATH];
            if (GetEnvironmentVariableA("DXCOMPILER_PATH", path, sizeof(path)))
                m = LoadLibraryA(path);
        }
        if (!m || !(create = (DxcCreateInstanceProc)(void *)GetProcAddress(m, "DxcCreateInstance")))
        {
            printf("  dxcompiler.dll unavailable\n");
            return {};
        }
    }
    IDxcCompiler3 *compiler = nullptr;
    if (FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))))
        return {};
    wchar_t wentry[64], wtarget[32];
    MultiByteToWideChar(CP_UTF8, 0, entry, -1, wentry, 64);
    MultiByteToWideChar(CP_UTF8, 0, target, -1, wtarget, 32);
    std::vector<LPCWSTR> args = { L"-E", wentry, L"-T", wtarget, L"-HV", L"2021" };
    for (int i = 0; i < extra_count; i++)
        args.push_back(extra[i]);
    DxcBuffer buffer = { source, strlen(source), DXC_CP_UTF8 };
    IDxcResult *result = nullptr;
    std::vector<char> out;
    if (SUCCEEDED(compiler->Compile(&buffer, args.data(), (UINT32)args.size(), nullptr, IID_PPV_ARGS(&result))))
    {
        HRESULT status;
        result->GetStatus(&status);
        IDxcBlobUtf8 *errors = nullptr;
        result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
        if (errors && errors->GetStringLength())
            printf("  dxc %s: %s\n", target, errors->GetStringPointer());
        if (errors)
            errors->Release();
        IDxcBlob *object = nullptr;
        if (SUCCEEDED(status) && SUCCEEDED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr)) && object)
        {
            out.assign((char *)object->GetBufferPointer(), (char *)object->GetBufferPointer() + object->GetBufferSize());
            object->Release();
        }
        result->Release();
    }
    compiler->Release();
    return out;
}

static std::vector<char> compile_fxc(const char *source, const char *entry, const char *target)
{
    ID3DBlob *blob = nullptr, *errors = nullptr;
    HRESULT hr = D3DCompile(source, strlen(source), nullptr, nullptr, nullptr, entry, target, 0, 0, &blob, &errors);
    if (errors)
    {
        printf("  fxc %s: %s\n", target, (char *)errors->GetBufferPointer());
        errors->Release();
    }
    if (FAILED(hr))
        return {};
    std::vector<char> out((char *)blob->GetBufferPointer(), (char *)blob->GetBufferPointer() + blob->GetBufferSize());
    blob->Release();
    return out;
}

// ---- helpers ----

static bool init()
{
    CHECK_HR(CreateDXGIFactory2(0, IID_PPV_ARGS(&g.factory)));
    CHECK_HR(g.factory->EnumAdapters1(0, &g.adapter));
    static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
                                                D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    for (D3D_FEATURE_LEVEL level : levels)
        if (SUCCEEDED(D3D12CreateDevice(g.adapter, level, IID_PPV_ARGS(&g.device))))
        {
            g.feature_level = level;
            break;
        }
    if (!g.device)
    {
        printf("  D3D12CreateDevice failed at every feature level\n");
        return false;
    }
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    CHECK_HR(g.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue)));
    CHECK_HR(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.allocator)));
    CHECK_HR(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.allocator, nullptr, IID_PPV_ARGS(&g.list)));
    CHECK_HR(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)));
    g.event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    return true;
}

static bool execute_and_wait()
{
    CHECK_HR(g.list->Close());
    ID3D12CommandList *lists[] = { g.list };
    g.queue->ExecuteCommandLists(1, lists);
    CHECK_HR(g.queue->Signal(g.fence, ++g.fence_value));
    CHECK_HR(g.fence->SetEventOnCompletion(g.fence_value, g.event));
    if (WaitForSingleObject(g.event, 10000) != WAIT_OBJECT_0)
    {
        printf("  GPU timeout (device removed: 0x%08lx)\n", (unsigned long)g.device->GetDeviceRemovedReason());
        return false;
    }
    CHECK_HR(g.allocator->Reset());
    CHECK_HR(g.list->Reset(g.allocator, nullptr));
    return true;
}

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
    if (FAILED(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

static void transition(ID3D12Resource *r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    g.list->ResourceBarrier(1, &b);
}

static ID3D12RootSignature *create_root_signature(const D3D12_ROOT_SIGNATURE_DESC &desc)
{
    ID3DBlob *blob = nullptr, *error = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)))
    {
        if (error)
            printf("  root signature: %s\n", (char *)error->GetBufferPointer());
        return nullptr;
    }
    ID3D12RootSignature *rs = nullptr;
    g.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs));
    blob->Release();
    return rs;
}

static ID3D12PipelineState *create_compute_pipeline(ID3D12RootSignature *rs, const std::vector<char> &cs)
{
    if (cs.empty())
        return nullptr;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = rs;
    pd.CS = { cs.data(), cs.size() };
    ID3D12PipelineState *ps = nullptr;
    HRESULT hr = g.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&ps));
    if (FAILED(hr))
        printf("  CreateComputePipelineState = 0x%08lx\n", (unsigned long)hr);
    return ps;
}

// Copies a buffer to a readback buffer and maps it.
static const uint32_t *read_buffer(ID3D12Resource *src, D3D12_RESOURCE_STATES state, UINT64 size, ID3D12Resource **readback)
{
    *readback = create_buffer(size, D3D12_HEAP_TYPE_READBACK);
    transition(src, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    g.list->CopyBufferRegion(*readback, 0, src, 0, size);
    if (!execute_and_wait())
        return nullptr;
    void *p = nullptr;
    D3D12_RANGE range = { 0, (SIZE_T)size };
    (*readback)->Map(0, &range, &p);
    return (const uint32_t *)p;
}

// ---- tests ----

static bool test_caps()
{
    printf("  feature level 0x%x\n", g.feature_level);
    D3D12_FEATURE_DATA_SHADER_MODEL sm = { D3D_SHADER_MODEL_6_7 };
    if (SUCCEEDED(g.device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))))
        printf("  shader model 0x%x\n", sm.HighestShaderModel);
    D3D12_FEATURE_DATA_D3D12_OPTIONS o = {};
    g.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o));
    printf("  binding %d tiled %d conservative %d rovs %d logicop %d\n", o.ResourceBindingTier, o.TiledResourcesTier,
           o.ConservativeRasterizationTier, o.ROVsSupported, o.OutputMergerLogicOp);
    printf("  typed-uav-load-additional %d\n", o.TypedUAVLoadAdditionalFormats);
    static const DXGI_FORMAT typed[] = { DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM,
                                         DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UINT, DXGI_FORMAT_R16_UINT };
    for (DXGI_FORMAT f : typed)
    {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = { f };
        g.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs));
        if (!(fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD))
            printf("  no typed UAV load: format %d\n", f);
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS2 o2 = {};
    g.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2, &o2, sizeof(o2));
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5 = {};
    g.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5));
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 o6 = {};
    g.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &o6, sizeof(o6));
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7 = {};
    g.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof(o7));
    printf("  depthbounds %d raytracing %d vrs %d mesh %d samplerfeedback %d\n", o2.DepthBoundsTestSupported, o5.RaytracingTier,
           o6.VariableShadingRateTier, o7.MeshShaderTier, o7.SamplerFeedbackTier);
    return true;
}

static const char *cs_uav_src = R"(
RWByteAddressBuffer o : register(u0);
[numthreads(64, 1, 1)]
void main(uint tid : SV_DispatchThreadID)
{
    o.Store(tid * 4, tid * 3 + 1);
}
)";

static bool run_uav_test(const std::vector<char> &cs)
{
    D3D12_ROOT_PARAMETER rp = {};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &rp };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    ID3D12PipelineState *ps = create_compute_pipeline(rs, cs);
    if (!rs || !ps)
        return false;
    ID3D12Resource *buf = create_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g.list->SetComputeRootSignature(rs);
    g.list->SetPipelineState(ps);
    g.list->SetComputeRootUnorderedAccessView(0, buf->GetGPUVirtualAddress());
    g.list->Dispatch(1, 1, 1);
    ID3D12Resource *rb;
    const uint32_t *v = read_buffer(buf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 256, &rb);
    if (!v)
        return false;
    for (uint32_t i = 0; i < 64; i++)
        EXPECT(v[i] == i * 3 + 1, "uav[%u] = %u, want %u", i, v[i], i * 3 + 1);
    rb->Release(); buf->Release(); ps->Release(); rs->Release();
    return true;
}

static bool test_compute_dxil() { return run_uav_test(compile_dxc(cs_uav_src, "main", "cs_6_0")); }
static bool test_compute_dxbc() { return run_uav_test(compile_fxc(cs_uav_src, "main", "cs_5_0")); }

static bool test_draw()
{
    static const char *src = R"(
float4 vs(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps() : SV_Target { return float4(0.25, 0.5, 0.75, 1.0); }
)";
    std::vector<char> vs = compile_dxc(src, "vs", "vs_6_0"), ps = compile_dxc(src, "ps", "ps_6_0");
    if (vs.empty() || ps.empty())
        return false;
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    ID3D12RootSignature *rs = create_root_signature(rsd);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = rs;
    pd.VS = { vs.data(), vs.size() };
    pd.PS = { ps.data(), ps.size() };
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = ~0u;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState *pso = nullptr;
    CHECK_HR(g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)));

    const UINT W = 64, H = 64;
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = W; td.Height = H; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv = { DXGI_FORMAT_R8G8B8A8_UNORM, { 0, 0, 0, 0 } };
    ID3D12Resource *rt = nullptr;
    CHECK_HR(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&rt)));
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    ID3D12DescriptorHeap *rtv_heap = nullptr;
    CHECK_HR(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap)));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    g.device->CreateRenderTargetView(rt, nullptr, rtv);

    float clear[4] = { 1, 0, 0, 1 };
    g.list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp = { 0, 0, (float)W, (float)H, 0, 1 };
    D3D12_RECT sc = { 0, 0, (LONG)W, (LONG)H / 2 };  // bottom half keeps the clear color
    g.list->RSSetViewports(1, &vp);
    g.list->RSSetScissorRects(1, &sc);
    g.list->SetGraphicsRootSignature(rs);
    g.list->SetPipelineState(pso);
    g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.list->DrawInstanced(3, 1, 0, 0);

    ID3D12Resource *rb = create_buffer(W * H * 4, D3D12_HEAP_TYPE_READBACK);
    transition(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
    dst.pResource = rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 1, W * 4 };
    srcl.pResource = rt;
    srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
    if (!execute_and_wait())
        return false;
    const uint32_t *px = nullptr;
    D3D12_RANGE range = { 0, W * H * 4 };
    rb->Map(0, &range, (void **)&px);
    uint32_t drawn = px[8 * W + 8], cleared = px[56 * W + 8];
    EXPECT(drawn == 0xffbf8040, "drawn pixel 0x%08x, want 0xffbf8040", drawn);
    EXPECT(cleared == 0xff0000ff, "cleared pixel 0x%08x, want 0xff0000ff", cleared);
    rb->Release(); rtv_heap->Release(); rt->Release(); pso->Release(); rs->Release();
    return true;
}

// Output-merger logic operation (11_1): XOR into an integer render target.
static bool test_logicop()
{
    static const char *src = R"(
float4 vs(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
uint4 ps() : SV_Target { return uint4(0x19, 0x5a, 0xf0, 0x0f); }
)";
    D3D12_FEATURE_DATA_D3D12_OPTIONS o = {};
    g.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o));
    if (!o.OutputMergerLogicOp)
    {
        printf("  OutputMergerLogicOp not supported\n");
        return false;
    }
    std::vector<char> vs = compile_dxc(src, "vs", "vs_6_0"), ps = compile_dxc(src, "ps", "ps_6_0");
    if (vs.empty() || ps.empty())
        return false;
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    ID3D12RootSignature *rs = create_root_signature(rsd);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = rs;
    pd.VS = { vs.data(), vs.size() };
    pd.PS = { ps.data(), ps.size() };
    pd.BlendState.RenderTarget[0].LogicOpEnable = TRUE;
    pd.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_XOR;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = ~0u;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UINT;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState *pso = nullptr;
    CHECK_HR(g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)));

    const UINT W = 16, H = 16;
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = W; td.Height = H; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UINT;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource *rt = nullptr;
    CHECK_HR(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&rt)));
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    ID3D12DescriptorHeap *rtv_heap = nullptr;
    CHECK_HR(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap)));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    g.device->CreateRenderTargetView(rt, nullptr, rtv);
    float clear[4] = { 0xaa, 0x3c, 0x0f, 0xff };  // integer formats take the values as integers
    g.list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp = { 0, 0, (float)W, (float)H, 0, 1 };
    D3D12_RECT sc = { 0, 0, (LONG)W, (LONG)H };
    g.list->RSSetViewports(1, &vp);
    g.list->RSSetScissorRects(1, &sc);
    g.list->SetGraphicsRootSignature(rs);
    g.list->SetPipelineState(pso);
    g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.list->DrawInstanced(3, 1, 0, 0);
    ID3D12Resource *rb = create_buffer(256 * H, D3D12_HEAP_TYPE_READBACK);
    transition(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
    dst.pResource = rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UINT, W, H, 1, 256 };
    srcl.pResource = rt;
    srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
    if (!execute_and_wait())
        return false;
    const uint8_t *px = nullptr;
    D3D12_RANGE range = { 0, 256 * H };
    rb->Map(0, &range, (void **)&px);
    const uint8_t want[4] = { 0xaa ^ 0x19, 0x3c ^ 0x5a, 0x0f ^ 0xf0, 0xff ^ 0x0f };
    for (int c = 0; c < 4; c++)
        EXPECT(px[8 * 256 + 8 * 4 + c] == want[c], "channel %d = 0x%x, want 0x%x", c, px[8 * 256 + 8 * 4 + c], want[c]);
    rb->Release(); rtv_heap->Release(); rt->Release(); pso->Release(); rs->Release();
    return true;
}

// Raw buffer SRVs through a descriptor table: out-of-range and null descriptors read 0,
// out-of-range UAV writes are dropped (robustBufferAccess2 / nullDescriptor).
static bool test_robust()
{
    static const char *src = R"(
ByteAddressBuffer small : register(t0);
ByteAddressBuffer null_srv : register(t1);
RWByteAddressBuffer small_uav : register(u0);
RWByteAddressBuffer o : register(u1);
[numthreads(16, 1, 1)]
void main(uint tid : SV_DispatchThreadID)
{
    o.Store(tid * 4, small.Load(tid * 4));
    o.Store(64 + tid * 4, null_srv.Load(tid * 4));
    small_uav.Store(tid * 4, 0xA0000000 | tid);
    uint prev;
    small_uav.InterlockedAdd(1024 + tid * 4, 1, prev);
    o.Store(128 + tid * 4, prev);
}
)";
    std::vector<char> cs = compile_dxc(src, "main", "cs_6_0");
    D3D12_DESCRIPTOR_RANGE ranges[2] = {
        { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0 },
        { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 2 },
    };
    D3D12_ROOT_PARAMETER rp = {};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp.DescriptorTable = { 2, ranges };
    D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &rp };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    ID3D12PipelineState *ps = create_compute_pipeline(rs, cs);
    if (!rs || !ps)
        return false;

    ID3D12Resource *data = create_buffer(4096, D3D12_HEAP_TYPE_UPLOAD);
    uint32_t *p;
    data->Map(0, nullptr, (void **)&p);
    for (uint32_t i = 0; i < 1024; i++)
        p[i] = 0x11000000 | i;
    data->Unmap(0, nullptr);
    ID3D12Resource *target = create_buffer(4096, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                           D3D12_RESOURCE_STATE_COPY_DEST);
    g.list->CopyBufferRegion(target, 0, data, 0, 4096);
    transition(target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource *out = create_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap *heap = nullptr;
    CHECK_HR(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
    UINT inc = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();

    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = DXGI_FORMAT_R32_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.FirstElement = 20;  // bytes 80..111: 8 words
    srv.Buffer.NumElements = 8;
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    g.device->CreateShaderResourceView(data, &srv, h);
    h.ptr += inc;
    g.device->CreateShaderResourceView(nullptr, &srv, h);
    h.ptr += inc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = DXGI_FORMAT_R32_TYPELESS;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.FirstElement = 100;  // bytes 400..419: 5 words
    uav.Buffer.NumElements = 5;
    uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    g.device->CreateUnorderedAccessView(target, nullptr, &uav, h);
    h.ptr += inc;
    uav.Buffer.FirstElement = 0;
    uav.Buffer.NumElements = 64;
    g.device->CreateUnorderedAccessView(out, nullptr, &uav, h);

    g.list->SetDescriptorHeaps(1, &heap);
    g.list->SetComputeRootSignature(rs);
    g.list->SetPipelineState(ps);
    g.list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    g.list->Dispatch(1, 1, 1);
    D3D12_RESOURCE_BARRIER uavb = {};
    uavb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    g.list->ResourceBarrier(1, &uavb);

    ID3D12Resource *rb, *rb2;
    const uint32_t *v = read_buffer(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 256, &rb);
    if (!v)
        return false;
    for (uint32_t i = 0; i < 16; i++)
    {
        EXPECT(v[i] == (i < 8 ? (0x11000000u | (20 + i)) : 0u), "srv[%u] = 0x%x", i, v[i]);
        EXPECT(v[16 + i] == 0, "null srv[%u] = 0x%x", i, v[16 + i]);
        EXPECT(v[32 + i] == 0, "out-of-range atomic[%u] returned 0x%x", i, v[32 + i]);
    }
    const uint32_t *t = read_buffer(target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 4096, &rb2);
    if (!t)
        return false;
    for (uint32_t i = 0; i < 1024; i++)
    {
        // Raw buffer bounds are 16-byte granular in VKD3D-Proton (the view's 20 bytes round up to 32).
        if (i >= 105 && i < 108)
            continue;
        uint32_t want = (i >= 100 && i < 105) ? (0xA0000000u | (i - 100)) : (0x11000000u | i);
        EXPECT(t[i] == want, "target word %u = 0x%x, want 0x%x", i, t[i], want);
    }
    rb->Release(); rb2->Release(); heap->Release(); out->Release(); target->Release(); data->Release(); ps->Release(); rs->Release();
    return true;
}

struct Test { const char *name; bool (*fn)(); };
static const Test tests[] = {
    { "caps", test_caps },
    { "compute_dxil", test_compute_dxil },
    { "compute_dxbc", test_compute_dxbc },
    { "draw", test_draw },
    { "robust", test_robust },
    { "logicop", test_logicop },
};

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (!init())
        return 2;
    int failed_tests = 0;
    for (const Test &t : tests)
    {
        bool selected = argc < 2;
        for (int i = 1; i < argc; i++)
            selected |= !strcmp(argv[i], t.name);
        if (!selected)
            continue;
        int before = g_failures;
        printf("[%s]\n", t.name);
        bool ok = t.fn() && g_failures == before;
        printf("[%s] %s\n", t.name, ok ? "PASS" : "FAIL");
        failed_tests += !ok;
    }
    printf("%d test(s) failed, %d check(s), %d failure(s)\n", failed_tests, g_checks, g_failures);
    return failed_tests ? 1 : 0;
}
