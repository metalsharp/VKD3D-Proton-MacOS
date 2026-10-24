// D3D12 functional tests for VKD3D-Proton on MoltenVK (run under MetalSharp Wine).
// Shaders are compiled at run time: SM 6.x with dxcompiler.dll, SM 5.x with d3dcompiler_47.dll.
//   d3d12test.exe [test-name ...]     (no arguments runs every test)
#include <math.h>
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
    D3D12_FEATURE_DATA_SHADER_MODEL sm = { (D3D_SHADER_MODEL)0x69 };
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
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1 = {};
    g.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &o1, sizeof(o1));
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 o3 = {};
    g.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &o3, sizeof(o3));
    printf("  vprt-without-gs %d waveops %d int64 %d copy-queue-timestamps %d casting-fully-typed %d\n",
           o.VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation, o1.WaveOps, o1.Int64ShaderOps,
           o3.CopyQueueTimestampQueriesSupported, o3.CastingFullyTypedFormatSupported);
    // The other options, for spotting what is not supported.
#define CAPS(n, ...) do { D3D12_FEATURE_DATA_D3D12_OPTIONS##n d = {}; \
        if (SUCCEEDED(g.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS##n, &d, sizeof(d)))) { printf("  options%s:", #n); __VA_ARGS__; printf("\n"); } \
        else printf("  options%s: not available\n", #n); } while (0)
#define F(field) printf(" " #field "=%d", (int)d.field)
    CAPS(, F(DoublePrecisionFloatShaderOps), F(OutputMergerLogicOp), F(MinPrecisionSupport), F(PSSpecifiedStencilRefSupported),
         F(TypedUAVLoadAdditionalFormats), F(ROVsSupported), F(StandardSwizzle64KBSupported), F(CrossNodeSharingTier),
         F(ResourceHeapTier), F(MaxGPUVirtualAddressBitsPerResource));
    CAPS(1, F(WaveLaneCountMin), F(WaveLaneCountMax), F(TotalLaneCount), F(ExpandedComputeResourceStates));
    CAPS(2, F(ProgrammableSamplePositionsTier));
    CAPS(3, F(WriteBufferImmediateSupportFlags), F(ViewInstancingTier), F(BarycentricsSupported));
    CAPS(4, F(MSAA64KBAlignedTextureSupported), F(SharedResourceCompatibilityTier), F(Native16BitShaderOpsSupported));
    CAPS(5, F(SRVOnlyTiledResourceTier3), F(RenderPassesTier));
    CAPS(6, F(AdditionalShadingRatesSupported), F(PerPrimitiveShadingRateSupportedWithViewportIndexing), F(ShadingRateImageTileSize),
         F(BackgroundProcessingSupported));
    CAPS(8, F(UnalignedBlockTexturesSupported));
    CAPS(9, F(MeshShaderPipelineStatsSupported), F(MeshShaderSupportsFullRangeRenderTargetArrayIndex), F(AtomicInt64OnTypedResourceSupported),
         F(AtomicInt64OnGroupSharedSupported), F(DerivativesInMeshAndAmplificationShadersSupported), F(WaveMMATier));
    CAPS(10, F(VariableRateShadingSumCombinerSupported), F(MeshShaderPerPrimitiveShadingRateSupported));
    CAPS(11, F(AtomicInt64OnDescriptorHeapResourceSupported));
    CAPS(12, F(MSPrimitivesPipelineStatisticIncludesCulledPrimitives), F(EnhancedBarriersSupported), F(RelaxedFormatCastingSupported));
    CAPS(13, F(UnrestrictedBufferTextureCopyPitchSupported), F(UnrestrictedVertexElementAlignmentSupported),
         F(InvertedViewportHeightFlipsYSupported), F(InvertedViewportDepthFlipsZSupported), F(TextureCopyBetweenDimensionsSupported),
         F(AlphaBlendFactorSupported));
    CAPS(14, F(AdvancedTextureOpsSupported), F(WriteableMSAATexturesSupported), F(IndependentFrontAndBackStencilRefMaskSupported));
    CAPS(15, F(TriangleFanSupported), F(DynamicIndexBufferStripCutSupported));
    CAPS(16, F(DynamicDepthBiasSupported), F(GPUUploadHeapSupported));
    CAPS(17, F(NonNormalizedCoordinateSamplersSupported), F(ManualWriteTrackingResourceSupported));
    CAPS(18, F(RenderPassesValid));
#undef F
#undef CAPS
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

// SM 6.6/6.7: 64-bit buffer atomics, compute-shader derivatives on a 2D thread group, QuadAny.
static const char *cs_sm67_src = R"(
RWByteAddressBuffer o : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 gtid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    float f = gtid.x * 3.0 + gtid.y * 5.0 + (gtid.x & 1) * gtid.y * 2.0;
    int64_t prev;
    o.InterlockedAdd64(0, 0x100000001ll, prev);
    int64_t unused;
    o.InterlockedMin64(8, int64_t(gi) - 32, unused);
    o.Store4(16 + gi * 16, uint4(asuint(ddx_fine(f)), asuint(ddy_fine(f)), QuadAny(gtid.x == 1 && gtid.y == 1) ? 1 : 0,
                                 uint(uint64_t(prev) >> 32)));
}
)";

static bool test_sm67()
{
    D3D12_ROOT_PARAMETER rp = {};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &rp };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    ID3D12PipelineState *ps = create_compute_pipeline(rs, compile_dxc(cs_sm67_src, "main", "cs_6_7"));
    if (!rs || !ps)
        return false;
    const UINT64 size = 16 + 64 * 16;
    ID3D12Resource *buf = create_buffer(size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g.list->SetComputeRootSignature(rs);
    g.list->SetPipelineState(ps);
    g.list->SetComputeRootUnorderedAccessView(0, buf->GetGPUVirtualAddress());
    g.list->Dispatch(1, 1, 1);
    ID3D12Resource *rb;
    const uint32_t *v = read_buffer(buf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, size, &rb);
    if (!v)
        return false;
    EXPECT(v[0] == 64 && v[1] == 64, "add64 = %08x%08x, want 0000004000000040", v[1], v[0]);
    EXPECT((int32_t)v[2] == -32 && v[3] == 0xffffffffu, "min64 = %08x%08x, want ffffffffffffffe0", v[3], v[2]);
    uint64_t seen = 0;
    for (uint32_t gi = 0; gi < 64; gi++)
    {
        uint32_t x = gi % 8, y = gi / 8;
        const uint32_t *t = v + 4 + gi * 4;
        float dx, dy;
        memcpy(&dx, &t[0], 4);
        memcpy(&dy, &t[1], 4);
        EXPECT(dx == 3.0f + 2.0f * (y & ~1u) + 2.0f * (y & 1u), "ddx_fine at (%u,%u) = %f", x, y, dx);
        EXPECT(dy == 5.0f + 2.0f * (x & 1u), "ddy_fine at (%u,%u) = %f", x, y, dy);
        EXPECT(t[2] == (x < 2 && y < 2 ? 1u : 0u), "QuadAny at (%u,%u) = %u", x, y, t[2]);
        EXPECT(t[3] < 64 && !(seen & (1ull << t[3])), "add64 returned %u twice or out of range", t[3]);
        seen |= 1ull << (t[3] & 63);
    }
    rb->Release(); buf->Release(); ps->Release(); rs->Release();
    return true;
}
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

// Separating axis test: does triangle t (pixel coordinates) intersect the square [x0,x1]x[y0,y1]?
static bool tri_box(const float t[3][2], float x0, float y0, float x1, float y1)
{
    float minx = fminf(t[0][0], fminf(t[1][0], t[2][0])), maxx = fmaxf(t[0][0], fmaxf(t[1][0], t[2][0]));
    float miny = fminf(t[0][1], fminf(t[1][1], t[2][1])), maxy = fmaxf(t[0][1], fmaxf(t[1][1], t[2][1]));
    if (maxx < x0 || minx > x1 || maxy < y0 || miny > y1)
        return false;
    for (int e = 0; e < 3; e++)
    {
        const float *a = t[e], *b = t[(e + 1) % 3];
        float nx = b[1] - a[1], ny = a[0] - b[0], tmin = INFINITY, tmax = -INFINITY, bmin = INFINITY, bmax = -INFINITY;
        for (int k = 0; k < 3; k++)
        {
            float d = nx * t[k][0] + ny * t[k][1];
            tmin = fminf(tmin, d); tmax = fmaxf(tmax, d);
        }
        const float cx[4] = { x0, x1, x0, x1 }, cy[4] = { y0, y0, y1, y1 };
        for (int k = 0; k < 4; k++)
        {
            float d = nx * cx[k] + ny * cy[k];
            bmin = fminf(bmin, d); bmax = fmaxf(bmax, d);
        }
        if (tmax < bmin || bmax < tmin)
            return false;
    }
    return true;
}

// Conservative rasterization tier 1 (12_1): every pixel a triangle touches is covered, and none it
// misses by more than the uncertainty. Vertex buffer + root constants feed the vertex shader; both
// DrawInstanced and DrawIndexedInstanced (reversed winding).
static bool test_conservative()
{
    static const char *src = R"(
float2 offset : register(b0);
float4 vs(float2 pos : POSITION) : SV_Position { return float4(pos + offset, 0.5, 1); }
float4 ps() : SV_Target { return 1; }
)";
    std::vector<char> vs = compile_dxc(src, "vs", "vs_6_0"), ps = compile_dxc(src, "ps", "ps_6_0");
    if (vs.empty() || ps.empty())
        return false;
    D3D12_ROOT_PARAMETER rp = {};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp.Constants.Num32BitValues = 2;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
    ID3D12RootSignature *rs = create_root_signature(rsd);

    D3D12_INPUT_ELEMENT_DESC ie = { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = rs;
    pd.VS = { vs.data(), vs.size() };
    pd.PS = { ps.data(), ps.size() };
    pd.InputLayout = { &ie, 1 };
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = ~0u;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState *pso = nullptr;
    CHECK_HR(g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)));

    static const float tris[][3][2] = {
        { { 3.3f, 4.2f }, { 9.7f, 5.1f }, { 5.4f, 11.6f } },
        { { 1.2f, 1.5f }, { 30.6f, 3.1f }, { 1.4f, 2.0f } },
        { { 16.5f, 2.2f }, { 17.1f, 29.8f }, { 16.6f, 29.9f } },
        { { 20.2f, 20.3f }, { 20.6f, 20.5f }, { 20.4f, 20.9f } },
        { { 5.0f, 25.0f }, { 28.0f, 30.0f }, { 6.0f, 25.2f } },
    };
    const UINT W = 32, TRIS = sizeof(tris) / sizeof(tris[0]);
    const float offset[2] = { 0.125f, -0.25f };
    ID3D12Resource *vb = create_buffer(TRIS * 3 * 8, D3D12_HEAP_TYPE_UPLOAD), *ib = create_buffer(16, D3D12_HEAP_TYPE_UPLOAD);
    float *v = nullptr;
    vb->Map(0, nullptr, (void **)&v);
    for (UINT t = 0; t < TRIS; t++)
        for (UINT k = 0; k < 3; k++)
        {
            v[(t * 3 + k) * 2 + 0] = tris[t][k][0] / 16.0f - 1.0f - offset[0];
            v[(t * 3 + k) * 2 + 1] = 1.0f - tris[t][k][1] / 16.0f - offset[1];
        }
    uint16_t *idx = nullptr;
    ib->Map(0, nullptr, (void **)&idx);
    idx[0] = 0; idx[1] = 2; idx[2] = 1;

    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = W; td.Height = W; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource *rt = nullptr;
    CHECK_HR(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&rt)));
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    ID3D12DescriptorHeap *rtv_heap = nullptr;
    CHECK_HR(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap)));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    g.device->CreateRenderTargetView(rt, nullptr, rtv);
    ID3D12Resource *rb = create_buffer(W * W * 4, D3D12_HEAP_TYPE_READBACK);

    for (UINT t = 0; t < TRIS; t++)
        for (int indexed = 0; indexed < 2; indexed++)
        {
            float clear[4] = { 0, 0, 0, 0 };
            g.list->ClearRenderTargetView(rtv, clear, 0, nullptr);
            g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            D3D12_VIEWPORT vp = { 0, 0, (float)W, (float)W, 0, 1 };
            D3D12_RECT sc = { 0, 0, (LONG)W, (LONG)W };
            g.list->RSSetViewports(1, &vp);
            g.list->RSSetScissorRects(1, &sc);
            g.list->SetGraphicsRootSignature(rs);
            g.list->SetPipelineState(pso);
            g.list->SetGraphicsRoot32BitConstants(0, 2, offset, 0);
            g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            D3D12_VERTEX_BUFFER_VIEW vbv = { vb->GetGPUVirtualAddress() + t * 3 * 8, 3 * 8, 8 };
            g.list->IASetVertexBuffers(0, 1, &vbv);
            if (indexed)
            {
                D3D12_INDEX_BUFFER_VIEW ibv = { ib->GetGPUVirtualAddress(), 6, DXGI_FORMAT_R16_UINT };
                g.list->IASetIndexBuffer(&ibv);
                g.list->DrawIndexedInstanced(3, 1, 0, 0, 0);
            }
            else
                g.list->DrawInstanced(3, 1, 0, 0);
            transition(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
            dst.pResource = rb;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, W, W, 1, W * 4 };
            srcl.pResource = rt;
            srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            g.list->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
            transition(rt, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            if (!execute_and_wait())
                return false;
            const uint32_t *px = nullptr;
            D3D12_RANGE range = { 0, W * W * 4 };
            rb->Map(0, &range, (void **)&px);
            int missing = 0, extra = 0;
            for (UINT y = 0; y < W; y++)
                for (UINT x = 0; x < W; x++)
                {
                    bool got = px[y * W + x] != 0;
                    missing += tri_box(tris[t], x, y, x + 1, y + 1) && !got;
                    extra += got && !tri_box(tris[t], x - 0.02f, y - 0.02f, x + 1.02f, y + 1.02f);
                }
            D3D12_RANGE none = { 0, 0 };
            rb->Unmap(0, &none);
            EXPECT(!missing && !extra, "triangle %u %s: %d missing, %d extra pixels", t, indexed ? "indexed" : "direct", missing, extra);
        }
    rb->Release(); rtv_heap->Release(); rt->Release(); ib->Release(); vb->Release(); pso->Release(); rs->Release();
    return true;
}

// DXR 1.1 inline ray tracing: a bottom-level structure from an RGBA8_UNORM vertex buffer (a 1.1
// format), one top-level instance, and RayQuery in a compute shader.
static bool test_rayquery()
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5 = {};
    g.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5));
    if (o5.RaytracingTier < D3D12_RAYTRACING_TIER_1_1)
    {
        printf("  skipped: raytracing tier %d\n", o5.RaytracingTier);
        return true;
    }
    ID3D12Device5 *dev5 = nullptr;
    ID3D12GraphicsCommandList4 *list4 = nullptr;
    CHECK_HR(g.device->QueryInterface(IID_PPV_ARGS(&dev5)));
    CHECK_HR(g.list->QueryInterface(IID_PPV_ARGS(&list4)));

    // Triangle (0,0) (1,0) (0,1) at z = 0.5, as RGBA8_UNORM.
    const uint8_t verts[3][4] = { { 0, 0, 128, 255 }, { 255, 0, 128, 255 }, { 0, 255, 128, 255 } };
    ID3D12Resource *vb = create_buffer(sizeof(verts), D3D12_HEAP_TYPE_UPLOAD);
    void *map = nullptr;
    vb->Map(0, nullptr, &map);
    memcpy(map, verts, sizeof(verts));

    D3D12_RAYTRACING_GEOMETRY_DESC geom = {};
    geom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geom.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geom.Triangles.VertexFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    geom.Triangles.VertexCount = 3;
    geom.Triangles.VertexBuffer = { vb->GetGPUVirtualAddress(), 4 };
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blas_in = {};
    blas_in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    blas_in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    blas_in.NumDescs = 1;
    blas_in.pGeometryDescs = &geom;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO blas_info = {};
    dev5->GetRaytracingAccelerationStructurePrebuildInfo(&blas_in, &blas_info);

    D3D12_RAYTRACING_INSTANCE_DESC inst = {};
    inst.Transform[0][0] = inst.Transform[1][1] = inst.Transform[2][2] = 1.0f;
    inst.InstanceMask = 0xff;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlas_in = {};
    tlas_in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tlas_in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    tlas_in.NumDescs = 1;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlas_info = {};
    dev5->GetRaytracingAccelerationStructurePrebuildInfo(&tlas_in, &tlas_info);

    const D3D12_RESOURCE_FLAGS uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource *blas = create_buffer(blas_info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, uav,
                                         D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    ID3D12Resource *tlas = create_buffer(tlas_info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, uav,
                                         D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    UINT64 scratch_size = blas_info.ScratchDataSizeInBytes > tlas_info.ScratchDataSizeInBytes
            ? blas_info.ScratchDataSizeInBytes : tlas_info.ScratchDataSizeInBytes;
    ID3D12Resource *scratch = create_buffer(scratch_size, D3D12_HEAP_TYPE_DEFAULT, uav, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    inst.AccelerationStructure = blas->GetGPUVirtualAddress();
    ID3D12Resource *inst_buf = create_buffer(sizeof(inst), D3D12_HEAP_TYPE_UPLOAD);
    inst_buf->Map(0, nullptr, &map);
    memcpy(map, &inst, sizeof(inst));
    tlas_in.InstanceDescs = inst_buf->GetGPUVirtualAddress();
    if (!blas || !tlas || !scratch)
        return false;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build = {};
    build.Inputs = blas_in;
    build.DestAccelerationStructureData = blas->GetGPUVirtualAddress();
    build.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
    list4->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = blas;
    list4->ResourceBarrier(1, &b);
    build.Inputs = tlas_in;
    build.DestAccelerationStructureData = tlas->GetGPUVirtualAddress();
    list4->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
    b.UAV.pResource = tlas;
    list4->ResourceBarrier(1, &b);

    static const char *src = R"(
RaytracingAccelerationStructure scene : register(t0);
RWByteAddressBuffer o : register(u0);
[numthreads(4, 1, 1)]
void main(uint tid : SV_DispatchThreadID)
{
    static const float2 xy[4] = { float2(0.25, 0.25), float2(0.9, 0.9), float2(0.1, 0.1), float2(0.5, 0.4) };
    RayDesc r;
    r.Origin = float3(xy[tid], -1.0);
    r.Direction = float3(0, 0, 1);
    r.TMin = 0.0;
    r.TMax = 10.0;
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(scene, 0, 0xff, r);
    q.Proceed();
    o.Store2(tid * 8, uint2(q.CommittedStatus(), asuint(q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? q.CommittedRayT() : -1.0)));
}
)";
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 2, rp };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    ID3D12PipelineState *ps = create_compute_pipeline(rs, compile_dxc(src, "main", "cs_6_5"));
    if (!rs || !ps)
        return false;
    ID3D12Resource *out = create_buffer(32, D3D12_HEAP_TYPE_DEFAULT, uav, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g.list->SetComputeRootSignature(rs);
    g.list->SetPipelineState(ps);
    g.list->SetComputeRootShaderResourceView(0, tlas->GetGPUVirtualAddress());
    g.list->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
    g.list->Dispatch(1, 1, 1);
    ID3D12Resource *rb;
    const uint32_t *v = read_buffer(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 32, &rb);
    if (!v)
        return false;
    static const bool hit[4] = { true, false, true, true };
    for (int i = 0; i < 4; i++)
    {
        float t;
        memcpy(&t, &v[i * 2 + 1], 4);
        if (hit[i])
            EXPECT(v[i * 2] == 1 && fabsf(t - 1.502f) < 0.01f, "ray %d: status %u t %f, want a hit at 1.5", i, v[i * 2], t);
        else
            EXPECT(v[i * 2] == 0, "ray %d: status %u t %f, want a miss", i, v[i * 2], t);
    }
    rb->Release(); out->Release(); ps->Release(); rs->Release(); inst_buf->Release(); scratch->Release();
    tlas->Release(); blas->Release(); vb->Release(); list4->Release(); dev5->Release();
    return true;
}

// Sampler feedback 0.9: WriteSamplerFeedbackLevel into a MinMip map (16x16 mip regions of a 64x64
// texture), decoded with ResolveSubresourceRegion. Feedback may be conservative (VKD3D-Proton records
// mip N at regions 2^N times coarser), so check what must hold: every access is recorded, no region
// reports a finer mip than was requested anywhere, and a mip 0 access marks exactly its region.
static bool test_samplerfeedback()
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7 = {};
    g.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof(o7));
    if (o7.SamplerFeedbackTier < D3D12_SAMPLER_FEEDBACK_TIER_0_9)
    {
        printf("  skipped: sampler feedback tier %d\n", o7.SamplerFeedbackTier);
        return true;
    }
    ID3D12Device8 *dev8 = nullptr;
    ID3D12GraphicsCommandList1 *list1 = nullptr;
    CHECK_HR(g.device->QueryInterface(IID_PPV_ARGS(&dev8)));
    CHECK_HR(g.list->QueryInterface(IID_PPV_ARGS(&list1)));

    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 64; td.Height = 64; td.DepthOrArraySize = 1; td.MipLevels = 7;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    ID3D12Resource *tex = nullptr;
    CHECK_HR(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&tex)));
    D3D12_RESOURCE_DESC1 fd = {};
    fd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    fd.Width = 64; fd.Height = 64; fd.DepthOrArraySize = 1; fd.MipLevels = 7;
    fd.Format = (DXGI_FORMAT)189;   // DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE (missing from the mingw headers)
    fd.SampleDesc.Count = 1;
    fd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    fd.SamplerFeedbackMipRegion = { 16, 16, 1 };
    ID3D12Resource *fb = nullptr;
    CHECK_HR(dev8->CreateCommittedResource2(&hp, D3D12_HEAP_FLAG_NONE, &fd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, nullptr,
                                            IID_PPV_ARGS(&fb)));

    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap *heap = nullptr;
    CHECK_HR(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
    UINT inc = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap->GetCPUDescriptorHandleForHeapStart();
    g.device->CreateShaderResourceView(tex, nullptr, cpu);
    cpu.ptr += inc;
    dev8->CreateSamplerFeedbackUnorderedAccessView(tex, fb, cpu);

    D3D12_DESCRIPTOR_RANGE ranges[2] = {
        { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 },
        { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1 },
    };
    D3D12_ROOT_PARAMETER rp = {};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp.DescriptorTable = { 2, ranges };
    D3D12_STATIC_SAMPLER_DESC ss = {};
    ss.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.MaxLOD = D3D12_FLOAT32_MAX;
    ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &rp, 1, &ss };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    static const char *src = R"(
Texture2D<float4> tex : register(t0);
FeedbackTexture2D<SAMPLER_FEEDBACK_MIN_MIP> fb : register(u0);
SamplerState s : register(s0);
[numthreads(1, 1, 1)]
void main()
{
    fb.WriteSamplerFeedbackLevel(tex, s, float2(0.1, 0.1), 2.0);
    fb.WriteSamplerFeedbackLevel(tex, s, float2(0.9, 0.6), 3.0);
    fb.WriteSamplerFeedbackLevel(tex, s, float2(0.6, 0.1), 0.0);
}
)";
    ID3D12PipelineState *ps = create_compute_pipeline(rs, compile_dxc(src, "main", "cs_6_5"));
    if (!rs || !ps)
        return false;
    g.list->SetDescriptorHeaps(1, &heap);
    g.list->SetComputeRootSignature(rs);
    g.list->SetPipelineState(ps);
    g.list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    g.list->Dispatch(1, 1, 1);

    td.Width = 4; td.Height = 4; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8_UINT;
    ID3D12Resource *decoded = nullptr;
    CHECK_HR(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr,
                                               IID_PPV_ARGS(&decoded)));
    transition(fb, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    list1->ResolveSubresourceRegion(decoded, 0, 0, 0, fb, 0, nullptr, DXGI_FORMAT_R8_UINT, D3D12_RESOLVE_MODE_DECODE_SAMPLER_FEEDBACK);
    transition(decoded, D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12Resource *rb = create_buffer(256 * 4, D3D12_HEAP_TYPE_READBACK);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
    dst.pResource = rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8_UINT, 4, 4, 1, 256 };
    srcl.pResource = decoded;
    srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
    if (!execute_and_wait())
        return false;
    const uint8_t *px = nullptr;
    D3D12_RANGE range = { 0, 256 * 4 };
    rb->Map(0, &range, (void **)&px);
    for (UINT y = 0; y < 4; y++)
        for (UINT x = 0; x < 4; x++)
        {
            uint8_t v = px[y * 256 + x];
            if (x == 2 && y == 0)
                EXPECT(v == 0, "min mip of region (2,0) = %u, want 0", v);
            else if (x == 0 && y == 0)
                EXPECT(v == 2, "min mip of region (0,0) = %u, want 2", v);
            else if (x == 3 && y == 2)
                EXPECT(v == 2 || v == 3, "min mip of region (3,2) = %u, want 3 (or 2, conservatively)", v);
            else
                EXPECT(v == 0xff || v == 2 || v == 3, "min mip of region (%u,%u) = %u, want 255 (or 2/3, conservatively)", x, y, v);
        }
    rb->Release(); decoded->Release(); ps->Release(); rs->Release(); heap->Release(); fb->Release(); tex->Release();
    list1->Release(); dev8->Release();
    return true;
}

// Variable rate shading tier 2: per-draw rate, per-primitive SV_ShadingRate and a shading rate image
// are all accepted; on MoltenVK the draw shades at full rate (the finest rate is always valid output).
static bool test_vrs()
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 o6 = {};
    g.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &o6, sizeof(o6));
    if (o6.VariableShadingRateTier < D3D12_VARIABLE_SHADING_RATE_TIER_2)
    {
        printf("  skipped: VRS tier %d\n", o6.VariableShadingRateTier);
        return true;
    }
    EXPECT(o6.ShadingRateImageTileSize >= 8 && o6.ShadingRateImageTileSize <= 32, "tile size %u", o6.ShadingRateImageTileSize);
    ID3D12GraphicsCommandList5 *list5 = nullptr;
    CHECK_HR(g.list->QueryInterface(IID_PPV_ARGS(&list5)));
    static const char *src = R"(
struct V { float4 pos : SV_Position; uint rate : SV_ShadingRate; };
V vs(uint id : SV_VertexID)
{
    V o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.rate = 0x5; // D3D12_SHADING_RATE_2X2
    return o;
}
float4 ps(float4 pos : SV_Position) : SV_Target { return float4(pos.x / 64.0, pos.y / 64.0, 0, 1); }
)";
    std::vector<char> vs = compile_dxc(src, "vs", "vs_6_4"), ps = compile_dxc(src, "ps", "ps_6_4");
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

    const UINT W = 64;
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = W; td.Height = W; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource *rt = nullptr;
    CHECK_HR(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&rt)));
    UINT tiles = (W + o6.ShadingRateImageTileSize - 1) / o6.ShadingRateImageTileSize;
    td.Width = tiles; td.Height = tiles;
    td.Format = DXGI_FORMAT_R8_UINT;
    td.Flags = D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource *sri = nullptr;
    CHECK_HR(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE, nullptr,
                                               IID_PPV_ARGS(&sri)));
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    ID3D12DescriptorHeap *rtv_heap = nullptr;
    CHECK_HR(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap)));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    g.device->CreateRenderTargetView(rt, nullptr, rtv);

    g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp = { 0, 0, (float)W, (float)W, 0, 1 };
    D3D12_RECT sc = { 0, 0, (LONG)W, (LONG)W };
    g.list->RSSetViewports(1, &vp);
    g.list->RSSetScissorRects(1, &sc);
    g.list->SetGraphicsRootSignature(rs);
    g.list->SetPipelineState(pso);
    D3D12_SHADING_RATE_COMBINER combiners[2] = { D3D12_SHADING_RATE_COMBINER_MAX, D3D12_SHADING_RATE_COMBINER_MAX };
    list5->RSSetShadingRate(D3D12_SHADING_RATE_2X2, combiners);
    list5->RSSetShadingRateImage(sri);
    g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.list->DrawInstanced(3, 1, 0, 0);
    list5->RSSetShadingRateImage(nullptr);
    list5->RSSetShadingRate(D3D12_SHADING_RATE_1X1, nullptr);

    ID3D12Resource *rb = create_buffer(W * W * 4, D3D12_HEAP_TYPE_READBACK);
    transition(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
    dst.pResource = rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, W, W, 1, W * 4 };
    srcl.pResource = rt;
    srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
    if (!execute_and_wait())
        return false;
    const uint8_t *px = nullptr;
    D3D12_RANGE range = { 0, W * W * 4 };
    rb->Map(0, &range, (void **)&px);
    int wrong = 0;
    for (UINT y = 0; y < W; y++)
        for (UINT x = 0; x < W; x++)
        {
            // Full rate: the shader ran at each pixel center. A 2x2 coarse pixel would share one value.
            int r = px[(y * W + x) * 4], g8 = px[(y * W + x) * 4 + 1];
            int wr = (int)((x + 0.5f) / 64.0f * 255.0f + 0.5f), wg = (int)((y + 0.5f) / 64.0f * 255.0f + 0.5f);
            wrong += abs(r - wr) > 1 || abs(g8 - wg) > 1;
        }
    EXPECT(!wrong, "%d pixels not shaded at their own center", wrong);
    rb->Release(); rtv_heap->Release(); sri->Release(); rt->Release(); pso->Release(); rs->Release(); list5->Release();
    return true;
}

// Tiled resources tier 3: a reserved Texture3D with one tile mapped. A compute shader writes the whole
// volume (unmapped tiles drop writes), another reads it back with residency feedback.
static bool test_tiled3d()
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS o = {};
    g.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o));
    if (o.TiledResourcesTier < D3D12_TILED_RESOURCES_TIER_3)
    {
        printf("  skipped: tiled resources tier %d\n", o.TiledResourcesTier);
        return true;
    }
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    td.Width = 256; td.Height = 256; td.DepthOrArraySize = 4; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource *vol = nullptr;
    CHECK_HR(g.device->CreateReservedResource(&td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&vol)));
    UINT tile_count = 0;
    D3D12_TILE_SHAPE shape = {};
    D3D12_SUBRESOURCE_TILING tiling = {};
    UINT tiling_count = 1;
    g.device->GetResourceTiling(vol, &tile_count, nullptr, &shape, &tiling_count, 0, &tiling);
    printf("  %u tiles, shape %ux%ux%u, mip 0 %ux%ux%u tiles\n", tile_count, shape.WidthInTexels, shape.HeightInTexels,
           shape.DepthInTexels, tiling.WidthInTiles, tiling.HeightInTiles, tiling.DepthInTiles);
    EXPECT(shape.WidthInTexels * shape.HeightInTexels * shape.DepthInTexels * 4 == 65536, "tile is %u texels",
           shape.WidthInTexels * shape.HeightInTexels * shape.DepthInTexels);

    D3D12_HEAP_DESC hd = { 65536, { D3D12_HEAP_TYPE_DEFAULT }, 0, D3D12_HEAP_FLAG_DENY_BUFFERS | D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES };
    ID3D12Heap *heap = nullptr;
    CHECK_HR(g.device->CreateHeap(&hd, IID_PPV_ARGS(&heap)));
    // Map the tile covering texel (130, 5, 2).
    D3D12_TILED_RESOURCE_COORDINATE coord = { 130 / shape.WidthInTexels, 5 / shape.HeightInTexels, 2 / shape.DepthInTexels, 0 };
    D3D12_TILE_REGION_SIZE region = { 1, FALSE, 0, 0, 0 };
    D3D12_TILE_RANGE_FLAGS range_flags = D3D12_TILE_RANGE_FLAG_NONE;
    UINT heap_start = 0, range_count = 1;
    g.queue->UpdateTileMappings(vol, 1, &coord, &region, heap, 1, &range_flags, &heap_start, &range_count, D3D12_TILE_MAPPING_FLAG_NONE);

    static const char *src = R"(
RWTexture3D<float4> vol : register(u0);
Texture3D<float4> vol_srv : register(t0);
RWByteAddressBuffer o : register(u1);
[numthreads(8, 8, 1)]
void write(uint3 id : SV_DispatchThreadID) { vol[id] = float4(0.25, 0.5, 0.75, 1.0); }
[numthreads(1, 1, 1)]
void read()
{
    static const int3 at[3] = { int3(130, 5, 2), int3(130, 5, 1), int3(5, 5, 2) };
    for (int i = 0; i < 3; i++)
    {
        uint status;
        float4 v = vol_srv.Load(int4(at[i], 0), int3(0, 0, 0), status);
        o.Store2(i * 8, uint2(CheckAccessFullyMapped(status) ? 1 : 0, asuint(v.x)));
    }
}
)";
    D3D12_DESCRIPTOR_RANGE ranges[2] = {
        { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0 },
        { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 1 },
    };
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable = { 2, ranges };
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[1].Descriptor.ShaderRegister = 1;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 2, rp };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    ID3D12PipelineState *wps = create_compute_pipeline(rs, compile_dxc(src, "write", "cs_6_0"));
    ID3D12PipelineState *rps = create_compute_pipeline(rs, compile_dxc(src, "read", "cs_6_0"));
    if (!rs || !wps || !rps)
        return false;
    D3D12_DESCRIPTOR_HEAP_DESC dhd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap *dheap = nullptr;
    CHECK_HR(g.device->CreateDescriptorHeap(&dhd, IID_PPV_ARGS(&dheap)));
    UINT inc = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = dheap->GetCPUDescriptorHandleForHeapStart();
    g.device->CreateUnorderedAccessView(vol, nullptr, nullptr, cpu);
    cpu.ptr += inc;
    g.device->CreateShaderResourceView(vol, nullptr, cpu);
    ID3D12Resource *out = create_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    g.list->SetDescriptorHeaps(1, &dheap);
    g.list->SetComputeRootSignature(rs);
    g.list->SetComputeRootDescriptorTable(0, dheap->GetGPUDescriptorHandleForHeapStart());
    g.list->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
    g.list->SetPipelineState(wps);
    g.list->Dispatch(256 / 8, 256 / 8, 4);
    D3D12_RESOURCE_BARRIER uavb = {};
    uavb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    g.list->ResourceBarrier(1, &uavb);
    transition(vol, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g.list->SetPipelineState(rps);
    g.list->Dispatch(1, 1, 1);
    ID3D12Resource *rb;
    const uint32_t *v = read_buffer(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 256, &rb);
    if (!v)
        return false;
    static const char *names[3] = { "mapped tile", "unmapped slice", "unmapped tile" };
    for (int i = 0; i < 3; i++)
    {
        float x;
        memcpy(&x, &v[i * 2 + 1], 4);
        bool mapped = i == 0;
        EXPECT(v[i * 2] == (mapped ? 1u : 0u) && fabsf(x - (mapped ? 0.25f : 0.0f)) < 0.01f, "%s: mapped %u value %f", names[i], v[i * 2], x);
    }
    rb->Release(); out->Release(); dheap->Release(); rps->Release(); wps->Release(); rs->Release(); heap->Release(); vol->Release();
    return true;
}

// Typed buffer views at element offsets that are not 16-byte aligned (D3D12 only requires element
// alignment): an SRV from element 3 and a UAV from element 5 of R32_UINT buffers.
static bool test_typedbuffer()
{
    static const char *src = R"(
Buffer<uint> srv : register(t0);
RWBuffer<uint> uav : register(u0);
RWByteAddressBuffer o : register(u1);
[numthreads(8, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    o.Store(i * 4, srv[i]);
    uav[i] = 1000 + i;
}
)";
    D3D12_DESCRIPTOR_RANGE ranges[2] = {
        { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 },
        { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1 },
    };
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable = { 2, ranges };
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[1].Descriptor.ShaderRegister = 1;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 2, rp };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    ID3D12PipelineState *ps = create_compute_pipeline(rs, compile_dxc(src, "main", "cs_6_0"));
    if (!rs || !ps)
        return false;
    ID3D12Resource *upload = create_buffer(256, D3D12_HEAP_TYPE_UPLOAD);
    uint32_t *u = nullptr;
    upload->Map(0, nullptr, (void **)&u);
    for (uint32_t i = 0; i < 64; i++)
        u[i] = i;
    ID3D12Resource *data = create_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    g.list->CopyBufferRegion(data, 0, upload, 0, 256);
    transition(data, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource *target = create_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource *out = create_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap *heap = nullptr;
    CHECK_HR(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
    UINT inc = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_R32_UINT;
    sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Buffer.FirstElement = 3;
    sd.Buffer.NumElements = 8;
    g.device->CreateShaderResourceView(data, &sd, h);
    h.ptr += inc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.Format = DXGI_FORMAT_R32_UINT;
    ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    ud.Buffer.FirstElement = 5;
    ud.Buffer.NumElements = 8;
    g.device->CreateUnorderedAccessView(target, nullptr, &ud, h);

    g.list->SetDescriptorHeaps(1, &heap);
    g.list->SetComputeRootSignature(rs);
    g.list->SetPipelineState(ps);
    g.list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    g.list->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
    g.list->Dispatch(1, 1, 1);
    ID3D12Resource *rb, *rb2;
    const uint32_t *v = read_buffer(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 256, &rb);
    if (!v)
        return false;
    for (uint32_t i = 0; i < 8; i++)
        EXPECT(v[i] == 3 + i, "srv[%u] = %u, want %u", i, v[i], 3 + i);
    const uint32_t *t = read_buffer(target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 256, &rb2);
    if (!t)
        return false;
    for (uint32_t i = 0; i < 16; i++)
    {
        uint32_t want = i >= 5 && i < 13 ? 1000 + i - 5 : 0;
        EXPECT(t[i] == want, "uav element %u = %u, want %u", i, t[i], want);
    }
    rb->Release(); rb2->Release(); heap->Release(); out->Release(); target->Release(); data->Release(); upload->Release();
    ps->Release(); rs->Release();
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

// Stream output from the vertex shader (DXIL and DXBC): two targets, a primitive goes to every target or none
// (the second target overflows), the SO statistics and overflow queries, appending from the filled size, and
// rasterization while streaming out.
static bool run_streamoutput(const char *what, const std::vector<char> &vs, const std::vector<char> &ps)
{
    if (vs.empty() || ps.empty())
        return false;
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT;
    ID3D12RootSignature *rs = create_root_signature(rsd);

    D3D12_SO_DECLARATION_ENTRY decl[2] = {
        { 0, "TEXCOORD", 0, 0, 4, 0 },
        { 0, "SV_Position", 0, 0, 4, 1 },
    };
    UINT strides[2] = { 16, 16 };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = rs;
    pd.VS = { vs.data(), vs.size() };
    pd.PS = { ps.data(), ps.size() };
    pd.StreamOutput = { decl, 2, strides, 2, 0 };
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

    const UINT W = 8, H = 8;
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = W; td.Height = H; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource *rt = nullptr;
    CHECK_HR(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&rt)));
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    ID3D12DescriptorHeap *rtv_heap = nullptr;
    CHECK_HR(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap)));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    g.device->CreateRenderTargetView(rt, nullptr, rtv);

    // One buffer: filled sizes at 0, 8 and 16; slot 0 data at 256, slot 1 at 1024 (room for one triangle),
    // then at 2048. Everything else holds a sentinel.
    const UINT64 size = 4096;
    ID3D12Resource *upload = create_buffer(size, D3D12_HEAP_TYPE_UPLOAD);
    uint32_t *init = nullptr;
    upload->Map(0, nullptr, (void **)&init);
    for (UINT i = 0; i < size / 4; i++)
        init[i] = 0xdeadbeef;
    memset(init, 0, 32);
    upload->Unmap(0, nullptr);
    ID3D12Resource *so = create_buffer(size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    g.list->CopyBufferRegion(so, 0, upload, 0, size);
    transition(so, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);

    D3D12_QUERY_HEAP_DESC qhd = { D3D12_QUERY_HEAP_TYPE_SO_STATISTICS, 2 };
    ID3D12QueryHeap *qh = nullptr;
    CHECK_HR(g.device->CreateQueryHeap(&qhd, IID_PPV_ARGS(&qh)));
    ID3D12Resource *qres = create_buffer(256, D3D12_HEAP_TYPE_READBACK);

    D3D12_GPU_VIRTUAL_ADDRESS base = so->GetGPUVirtualAddress();
    float clear[4] = { 0, 0, 0, 0 };
    g.list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp = { 0, 0, (float)W, (float)H, 0, 1 };
    D3D12_RECT sc = { 0, 0, (LONG)W, (LONG)H };
    g.list->RSSetViewports(1, &vp);
    g.list->RSSetScissorRects(1, &sc);
    g.list->SetGraphicsRootSignature(rs);
    g.list->SetPipelineState(pso);
    g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_STREAM_OUTPUT_BUFFER_VIEW views[2] = {
        { base + 256, 512, base + 0 },
        { base + 1024, 48, base + 8 },
    };
    g.list->SOSetTargets(0, 2, views);
    g.list->BeginQuery(qh, D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 0);
    g.list->DrawInstanced(6, 1, 0, 0);
    g.list->EndQuery(qh, D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 0);
    // Append to slot 0 from its filled size, with slot 1 moved to a new range.
    views[1] = { base + 2048, 512, base + 16 };
    g.list->SOSetTargets(0, 2, views);
    g.list->BeginQuery(qh, D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 1);
    g.list->DrawInstanced(3, 1, 0, 0);
    g.list->EndQuery(qh, D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 1);
    g.list->SOSetTargets(0, 0, nullptr);
    g.list->ResolveQueryData(qh, D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 0, 2, qres, 0);

    ID3D12Resource *rb = create_buffer(W * H * 4, D3D12_HEAP_TYPE_READBACK);
    transition(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
    dst.pResource = rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 1, W * 4 };
    srcl.pResource = rt;
    srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
    ID3D12Resource *sorb = nullptr;
    const uint32_t *d = read_buffer(so, D3D12_RESOURCE_STATE_STREAM_OUT, size, &sorb);
    if (!d)
        return false;

    EXPECT(d[0] == 96, "%s: slot 0 filled size %u, want 96", what, d[0]);
    EXPECT(d[2] == 48, "%s: slot 1 filled size %u, want 48", what, d[2]);
    EXPECT(d[4] == 48, "%s: moved slot 1 filled size %u, want 48", what, d[4]);
    // Slot 0: the first draw's first triangle, then the second draw's.
    for (UINT k = 0; k < 6; k++)
    {
        UINT id = k % 3;
        const float *t = (const float *)&d[(256 + 16 * k) / 4];
        EXPECT(t[0] == id && t[1] == 2.0f * id && t[2] == 3.0f * id && t[3] == 7.0f,
               "%s: slot 0 vertex %u = (%g %g %g %g), want (%u %u %u 7)", what, k, t[0], t[1], t[2], t[3], id, 2 * id, 3 * id);
    }
    EXPECT(d[(256 + 96) / 4] == 0xdeadbeef, "%s: slot 0 written past its filled size", what);
    for (UINT k = 0; k < 3; k++)
    {
        const float *p0 = (const float *)&d[(1024 + 16 * k) / 4], *p1 = (const float *)&d[(2048 + 16 * k) / 4];
        EXPECT(p0[3] == 1.0f && p1[3] == 1.0f, "%s: slot 1 vertex %u w = %g / %g, want 1", what, k, p0[3], p1[3]);
    }
    EXPECT(d[(1024 + 48) / 4] == 0xdeadbeef, "%s: slot 1 written past its size", what);

    const UINT64 *q = nullptr;
    D3D12_RANGE qr = { 0, 32 };
    qres->Map(0, &qr, (void **)&q);
    EXPECT(q[0] == 1 && q[1] == 2, "%s: first draw's SO statistics %llu written / %llu needed, want 1 / 2", what,
           (unsigned long long)q[0], (unsigned long long)q[1]);
    EXPECT(q[2] == 1 && q[3] == 1, "%s: second draw's SO statistics %llu written / %llu needed, want 1 / 1", what,
           (unsigned long long)q[2], (unsigned long long)q[3]);
    const uint32_t *px = nullptr;
    D3D12_RANGE range = { 0, W * H * 4 };
    rb->Map(0, &range, (void **)&px);
    EXPECT(px[1 * W + 1] == 0xff00ff00, "%s: drawn pixel 0x%08x, want 0xff00ff00", what, px[1 * W + 1]);

    qres->Release(); qh->Release(); sorb->Release(); so->Release(); upload->Release();
    rb->Release(); rtv_heap->Release(); rt->Release(); pso->Release(); rs->Release();
    return true;
}

static bool test_streamoutput()
{
    static const char *src = R"(
struct O { float4 pos : SV_Position; float4 t : TEXCOORD0; };
O vs(uint id : SV_VertexID)
{
    O o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.t = float4(id, 2 * id, 3 * id, 7);
    return o;
}
float4 ps() : SV_Target { return float4(0, 1, 0, 1); }
)";
    bool ok = run_streamoutput("DXIL", compile_dxc(src, "vs", "vs_6_0"), compile_dxc(src, "ps", "ps_6_0"));
    return run_streamoutput("DXBC", compile_fxc(src, "vs", "vs_5_0"), compile_fxc(src, "ps", "ps_5_0")) && ok;
}

// Stream output from a geometry shader (DXIL and DXBC), without render targets or rasterization: each point
// becomes a strip of 2 + id % 3 vertexes (0, 1 or 2 triangles), streamed out in order with the strips
// decomposed into triangles.
static bool run_gs_streamoutput(const char *what, const std::vector<char> &vs, const std::vector<char> &gs, const std::vector<char> &ps)
{
    if (vs.empty() || gs.empty() || ps.empty())
        return false;
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT;
    ID3D12RootSignature *rs = create_root_signature(rsd);
    D3D12_SO_DECLARATION_ENTRY decl[1] = { { 0, "TEXCOORD", 0, 0, 4, 0 } };
    UINT strides[1] = { 16 };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = rs;
    pd.VS = { vs.data(), vs.size() };
    pd.GS = { gs.data(), gs.size() };
    pd.PS = { ps.data(), ps.size() };
    pd.StreamOutput = { decl, 1, strides, 1, D3D12_SO_NO_RASTERIZED_STREAM };
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = ~0u;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState *pso = nullptr;
    CHECK_HR(g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)));

    const UINT64 size = 4096;
    ID3D12Resource *upload = create_buffer(size, D3D12_HEAP_TYPE_UPLOAD);
    uint32_t *init = nullptr;
    upload->Map(0, nullptr, (void **)&init);
    for (UINT i = 0; i < size / 4; i++)
        init[i] = 0xdeadbeef;
    memset(init, 0, 16);
    upload->Unmap(0, nullptr);
    ID3D12Resource *so = create_buffer(size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    g.list->CopyBufferRegion(so, 0, upload, 0, size);
    transition(so, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
    D3D12_QUERY_HEAP_DESC qhd = { D3D12_QUERY_HEAP_TYPE_SO_STATISTICS, 1 };
    ID3D12QueryHeap *qh = nullptr;
    CHECK_HR(g.device->CreateQueryHeap(&qhd, IID_PPV_ARGS(&qh)));
    ID3D12Resource *qres = create_buffer(256, D3D12_HEAP_TYPE_READBACK);

    // A stream output only pass: no render targets (VKD3D-Proton then renders into the largest framebuffer).
    g.list->SetGraphicsRootSignature(rs);
    g.list->SetPipelineState(pso);
    g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    D3D12_STREAM_OUTPUT_BUFFER_VIEW view = { so->GetGPUVirtualAddress() + 256, 2048, so->GetGPUVirtualAddress() };
    g.list->SOSetTargets(0, 1, &view);
    g.list->BeginQuery(qh, D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 0);
    g.list->DrawInstanced(6, 1, 0, 0);
    g.list->EndQuery(qh, D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 0);
    g.list->SOSetTargets(0, 0, nullptr);
    g.list->ResolveQueryData(qh, D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 0, 1, qres, 0);
    ID3D12Resource *sorb = nullptr;
    const uint32_t *d = read_buffer(so, D3D12_RESOURCE_STATE_STREAM_OUT, size, &sorb);
    if (!d)
        return false;

    // Points 1, 2, 4, 5 make 1, 2, 1, 2 triangles: 6 triangles, 18 vertexes.
    EXPECT(d[0] == 18 * 16, "%s: filled size %u, want %u", what, d[0], 18 * 16);
    UINT t = 0;
    for (UINT p = 0; p < 6; p++)
        for (UINT tri = 0; tri < p % 3; tri++, t++)
        {
            UINT k[3] = { tri, tri + 1 + (tri & 1), tri + 2 - (tri & 1) };
            for (UINT j = 0; j < 3; j++)
            {
                const float *v = (const float *)&d[(256 + 16 * (3 * t + j)) / 4];
                EXPECT(v[0] == p && v[1] == k[j] && v[3] == 5.0f, "%s: triangle %u vertex %u = (%g %g %g %g), want (%u %u * 5)",
                       what, t, j, v[0], v[1], v[2], v[3], p, k[j]);
            }
        }
    EXPECT(d[(256 + 18 * 16) / 4] == 0xdeadbeef, "%s: written past the filled size", what);
    const UINT64 *q = nullptr;
    D3D12_RANGE qr = { 0, 16 };
    qres->Map(0, &qr, (void **)&q);
    EXPECT(q[0] == 6 && q[1] == 6, "%s: SO statistics %llu written / %llu needed, want 6 / 6", what,
           (unsigned long long)q[0], (unsigned long long)q[1]);
    qres->Release(); qh->Release(); sorb->Release(); so->Release(); upload->Release(); pso->Release(); rs->Release();
    return true;
}

static const char *gs_so_src = R"(
struct V { float4 pos : SV_Position; uint id : ID; };
V vs(uint id : SV_VertexID) { V v; v.pos = float4(0, 0, 0, 1); v.id = id; return v; }
struct O { float4 pos : SV_Position; float4 t : TEXCOORD0; };
[maxvertexcount(4)]
void gs(point V input[1], inout TriangleStream<O> stream)
{
    uint n = 2 + input[0].id % 3;
    for (uint k = 0; k < n; k++)
    {
        O o;
        o.pos = float4(k * 0.5 - 1, (k & 1) * 0.5, 0, 1);
        o.t = float4(input[0].id, k, 0, 5);
        stream.Append(o);
    }
}
float4 ps() : SV_Target { return float4(0, 1, 0, 1); }
)";

static bool test_gs_streamoutput_dxil()
{
    return run_gs_streamoutput("DXIL", compile_dxc(gs_so_src, "vs", "vs_6_0"), compile_dxc(gs_so_src, "gs", "gs_6_0"),
                               compile_dxc(gs_so_src, "ps", "ps_6_0"));
}

static bool test_gs_streamoutput_dxbc()
{
    return run_gs_streamoutput("DXBC", compile_fxc(gs_so_src, "vs", "vs_5_0"), compile_fxc(gs_so_src, "gs", "gs_5_0"),
                               compile_fxc(gs_so_src, "ps", "ps_5_0"));
}

// Predication (SetPredication): draws and dispatches skipped by the predicate buffer's value, with EQUAL_ZERO
// and NOT_EQUAL_ZERO.
static bool test_predication()
{
    static const char *src = R"(
RWStructuredBuffer<uint> u : register(u0);
[numthreads(1, 1, 1)] void cs() { InterlockedAdd(u[0], 1); }
float4 vs(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps() : SV_Target { return float4(0, 1, 0, 1); }
)";
    std::vector<char> csb = compile_dxc(src, "cs", "cs_6_0"), vsb = compile_dxc(src, "vs", "vs_6_0"), psb = compile_dxc(src, "ps", "ps_6_0");
    if (csb.empty() || vsb.empty() || psb.empty())
        return false;
    D3D12_ROOT_PARAMETER rp = {};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &rp };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    ID3D12PipelineState *cps = create_compute_pipeline(rs, csb);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = rs;
    pd.VS = { vsb.data(), vsb.size() };
    pd.PS = { psb.data(), psb.size() };
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = ~0u;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState *gps = nullptr;
    CHECK_HR(g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&gps)));
    if (!cps)
        return false;

    // Predicates: [0] = 0, [1] = 1 (64-bit each).
    ID3D12Resource *upload = create_buffer(256, D3D12_HEAP_TYPE_UPLOAD);
    uint64_t *init = nullptr;
    upload->Map(0, nullptr, (void **)&init);
    init[0] = 0;
    init[1] = 1;
    upload->Unmap(0, nullptr);
    ID3D12Resource *pred = create_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    g.list->CopyBufferRegion(pred, 0, upload, 0, 16);
    transition(pred, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PREDICATION);
    ID3D12Resource *counter = create_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    const UINT W = 4, H = 4;
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = W; td.Height = H; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource *rt[4] = {};
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 4 };
    ID3D12DescriptorHeap *rtv_heap = nullptr;
    CHECK_HR(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap)));
    UINT rtv_size = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[4];
    float clear[4] = { 0, 0, 0, 0 };
    D3D12_VIEWPORT vp = { 0, 0, (float)W, (float)H, 0, 1 };
    D3D12_RECT sc = { 0, 0, (LONG)W, (LONG)H };
    // Cases: predicate 0 / 1 with EQUAL_ZERO (skips when the value is 0) and NOT_EQUAL_ZERO.
    struct { UINT64 offset; D3D12_PREDICATION_OP op; bool runs; } cases[4] = {
        { 0, D3D12_PREDICATION_OP_EQUAL_ZERO, false },
        { 8, D3D12_PREDICATION_OP_EQUAL_ZERO, true },
        { 0, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO, true },
        { 8, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO, false },
    };
    for (UINT k = 0; k < 4; k++)
    {
        CHECK_HR(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&rt[k])));
        rtv[k] = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rtv[k].ptr += k * rtv_size;
        g.device->CreateRenderTargetView(rt[k], nullptr, rtv[k]);
        g.list->ClearRenderTargetView(rtv[k], clear, 0, nullptr);
        g.list->SetPredication(pred, cases[k].offset, cases[k].op);
        g.list->SetComputeRootSignature(rs);
        g.list->SetComputeRootUnorderedAccessView(0, counter->GetGPUVirtualAddress() + 4 * k);
        g.list->SetPipelineState(cps);
        g.list->Dispatch(1, 1, 1);
        g.list->OMSetRenderTargets(1, &rtv[k], FALSE, nullptr);
        g.list->RSSetViewports(1, &vp);
        g.list->RSSetScissorRects(1, &sc);
        g.list->SetGraphicsRootSignature(rs);
        g.list->SetPipelineState(gps);
        g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g.list->DrawInstanced(3, 1, 0, 0);
        g.list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    }
    ID3D12Resource *rb[4] = {};
    for (UINT k = 0; k < 4; k++)
    {
        rb[k] = create_buffer(W * H * 4, D3D12_HEAP_TYPE_READBACK);
        transition(rt[k], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
        dst.pResource = rb[k];
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 1, 256 };
        srcl.pResource = rt[k];
        srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        g.list->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
    }
    ID3D12Resource *crb = nullptr;
    const uint32_t *c = read_buffer(counter, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 16, &crb);
    if (!c)
        return false;
    for (UINT k = 0; k < 4; k++)
    {
        const uint32_t *px = nullptr;
        D3D12_RANGE range = { 0, 4 };
        rb[k]->Map(0, &range, (void **)&px);
        EXPECT(c[k] == (cases[k].runs ? 1u : 0u), "case %u: dispatch ran %u times, want %u", k, c[k], cases[k].runs ? 1 : 0);
        EXPECT(px[0] == (cases[k].runs ? 0xff00ff00u : 0u), "case %u: pixel 0x%08x, want 0x%08x", k, px[0], cases[k].runs ? 0xff00ff00u : 0u);
        rb[k]->Release(); rt[k]->Release();
    }
    crb->Release(); counter->Release(); pred->Release(); upload->Release(); rtv_heap->Release();
    gps->Release(); cps->Release(); rs->Release();
    return true;
}

// Native 16-bit shader operations (SM 6.2, -enable-16bit-types): half and 16-bit integer arithmetic, 16-bit loads
// and stores of raw and structured buffers, and FP16 denormals (which D3D12 requires to be preserved).
static const char *cs_16bit_src = R"(
RWByteAddressBuffer raw : register(u0);
struct S { half2 h; uint16_t2 u; };
RWStructuredBuffer<S> st : register(u1);
[numthreads(1, 1, 1)]
void main()
{
    half a = raw.Load<half>(0);             // 1.5
    half b = raw.Load<half>(2);             // 2^-14, the smallest normal
    uint16_t c = raw.Load<uint16_t>(4);     // 0xfff0
    raw.Store<half>(16, a * half(3.0));                  // 4.5
    raw.Store<half>(18, b * half(0.0009765625));         // 2^-24, a denormal
    raw.Store<uint16_t>(20, c + uint16_t(0x20));         // wraps to 0x0010
    raw.Store<int16_t>(22, int16_t(-3) * int16_t(5));    // -15
    S s = st[0];
    s.h = s.h * half2(2.0, 0.5);
    s.u = s.u >> uint16_t(1);
    st[1] = s;
}
)";

static bool test_native16bit()
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS4 o4 = {};
    g.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &o4, sizeof(o4));
    EXPECT(o4.Native16BitShaderOpsSupported, "Native16BitShaderOpsSupported is false");
    static const wchar_t *const extra[] = { L"-enable-16bit-types" };
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[1].Descriptor.ShaderRegister = 1;
    rp[0].ShaderVisibility = rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 2, rp };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    ID3D12PipelineState *ps = create_compute_pipeline(rs, compile_dxc(cs_16bit_src, "main", "cs_6_2", extra, 1));
    if (!rs || !ps)
        return false;
    ID3D12Resource *upload = create_buffer(256, D3D12_HEAP_TYPE_UPLOAD);
    uint16_t *init = nullptr;
    upload->Map(0, nullptr, (void **)&init);
    memset(init, 0, 256);
    init[0] = 0x3e00;   // 1.5
    init[1] = 0x0400;   // 2^-14
    init[2] = 0xfff0;
    init[64] = 0x4400;  // st[0].h = (4, 6)
    init[65] = 0x4600;
    init[66] = 0x1234;  // st[0].u = (0x1234, 0x8000)
    init[67] = 0x8000;
    upload->Unmap(0, nullptr);
    ID3D12Resource *buf = create_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_COPY_DEST);
    g.list->CopyBufferRegion(buf, 0, upload, 0, 256);
    transition(buf, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g.list->SetComputeRootSignature(rs);
    g.list->SetPipelineState(ps);
    g.list->SetComputeRootUnorderedAccessView(0, buf->GetGPUVirtualAddress());
    g.list->SetComputeRootUnorderedAccessView(1, buf->GetGPUVirtualAddress() + 128);
    g.list->Dispatch(1, 1, 1);
    ID3D12Resource *rb;
    const uint32_t *v32 = read_buffer(buf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 256, &rb);
    if (!v32)
        return false;
    const uint16_t *v = (const uint16_t *)v32;
    EXPECT(v[8] == 0x4480, "half multiply = %#x, want 0x4480 (4.5)", v[8]);
    EXPECT(v[9] == 0x0001, "half denormal = %#x, want 0x0001 (2^-24)", v[9]);
    EXPECT(v[10] == 0x0010, "uint16 wrapping add = %#x, want 0x10", v[10]);
    EXPECT(v[11] == 0xfff1, "int16 multiply = %#x, want 0xfff1 (-15)", v[11]);
    EXPECT(v[68] == 0x4800 && v[69] == 0x4200, "structured half2 = (%#x %#x), want (0x4800 0x4200)", v[68], v[69]);
    EXPECT(v[70] == 0x091a && v[71] == 0x4000, "structured uint16_t2 = (%#x %#x), want (0x91a 0x4000)", v[70], v[71]);
    rb->Release(); buf->Release(); upload->Release(); ps->Release(); rs->Release();
    return true;
}

// 64-bit atomics on groupshared memory (SM 6.6): add, min, max and compare-exchange from 64 threads in 4 groups.
static const char *cs_gs64_src = R"(
RWStructuredBuffer<uint64_t> r : register(u0);
groupshared uint64_t s_add, s_min, s_max, s_cas;
[numthreads(64, 1, 1)]
void main(uint i : SV_GroupIndex, uint g : SV_GroupID)
{
    if (i == 0) { s_add = 0; s_min = ~0ull; s_max = 0; s_cas = 0; }
    GroupMemoryBarrierWithGroupSync();
    uint64_t v = (uint64_t(i + 1) << 33) | g;
    uint64_t prev;
    InterlockedAdd(s_add, v, prev);
    InterlockedMin(s_min, v, prev);
    InterlockedMax(s_max, v, prev);
    uint64_t old = s_cas;
    while (true)
    {
        InterlockedCompareExchange(s_cas, old, old + 0x100000001ull, prev);
        if (prev == old)
            break;
        old = prev;
    }
    GroupMemoryBarrierWithGroupSync();
    if (i == 0) { r[g * 4] = s_add; r[g * 4 + 1] = s_min; r[g * 4 + 2] = s_max; r[g * 4 + 3] = s_cas; }
}
)";

static bool test_groupshared64()
{
    D3D12_ROOT_PARAMETER rp = {};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &rp };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    ID3D12PipelineState *ps = create_compute_pipeline(rs, compile_dxc(cs_gs64_src, "main", "cs_6_6"));
    if (!rs || !ps)
        return false;
    const UINT groups = 4;
    ID3D12Resource *buf = create_buffer(groups * 32, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g.list->SetComputeRootSignature(rs);
    g.list->SetPipelineState(ps);
    g.list->SetComputeRootUnorderedAccessView(0, buf->GetGPUVirtualAddress());
    g.list->Dispatch(groups, 1, 1);
    ID3D12Resource *rb;
    const uint64_t *r = (const uint64_t *)read_buffer(buf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, groups * 32, &rb);
    if (!r)
        return false;
    for (UINT gi = 0; gi < groups; gi++)
    {
        uint64_t sum = 0;
        for (UINT i = 0; i < 64; i++)
            sum += ((uint64_t)(i + 1) << 33) | gi;
        EXPECT(r[gi * 4] == sum, "group %u add %llx, want %llx", gi, (unsigned long long)r[gi * 4], (unsigned long long)sum);
        EXPECT(r[gi * 4 + 1] == ((1ull << 33) | gi), "group %u min %llx", gi, (unsigned long long)r[gi * 4 + 1]);
        EXPECT(r[gi * 4 + 2] == ((64ull << 33) | gi), "group %u max %llx", gi, (unsigned long long)r[gi * 4 + 2]);
        EXPECT(r[gi * 4 + 3] == 64 * 0x100000001ull, "group %u compare-exchange %llx", gi, (unsigned long long)r[gi * 4 + 3]);
    }
    rb->Release(); buf->Release(); ps->Release(); rs->Release();
    return true;
}

struct Test { const char *name; bool (*fn)(); };
static const Test tests[] = {
    { "caps", test_caps },
    { "compute_dxil", test_compute_dxil },
    { "compute_dxbc", test_compute_dxbc },
    { "sm67", test_sm67 },
    { "draw", test_draw },
    { "conservative", test_conservative },
    { "rayquery", test_rayquery },
    { "samplerfeedback", test_samplerfeedback },
    { "vrs", test_vrs },
    { "tiled3d", test_tiled3d },
    { "typedbuffer", test_typedbuffer },
    { "robust", test_robust },
    { "logicop", test_logicop },
    { "streamoutput", test_streamoutput },
    { "gs_streamoutput_dxil", test_gs_streamoutput_dxil },
    { "gs_streamoutput_dxbc", test_gs_streamoutput_dxbc },
    { "predication", test_predication },
    { "native16bit", test_native16bit },
    { "groupshared64", test_groupshared64 },
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
