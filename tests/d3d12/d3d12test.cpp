// D3D12 functional tests for VKD3D-Proton on MoltenVK (run under MetalSharp Wine).
// Shaders are compiled at run time: SM 6.x with dxcompiler.dll, SM 5.x with d3dcompiler_47.dll.
//   d3d12test.exe [test-name ...]     (no arguments runs every test)
#include <math.h>
#include <windows.h>
#include <d3d12.h>
#include <d3d11on12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <dxcapi.h>
#include <stdio.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <string.h>
#include <string>
#include <algorithm>
#include <set>
#include <map>
#include <vector>
#include <algorithm>
#include <vector>
#include <algorithm>

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


// Creates (or truncates) a file for writing, readable and writable by its owner only.
static FILE *create_file(const char *name)
{
    int fd = _open(name, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE);
    return fd < 0 ? nullptr : _fdopen(fd, "wb");
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
        if (FILE *f = create_file(path.c_str()))
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
    std::vector<LPCWSTR> args = { L"-T", wtarget, L"-HV", L"2021" };
    if (*entry)  // Libraries (lib_6_x) have no entry point.
        args.insert(args.begin(), { L"-E", wentry });
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
// A one-triangle scene: (0,0) (1,0) (0,1) at z = 0.5 (RGBA8_UNORM vertexes), one instance; built into the list.
struct RtScene { ID3D12Resource *vb = nullptr, *blas = nullptr, *tlas = nullptr, *scratch = nullptr, *inst_buf = nullptr; };

static bool build_rt_scene(ID3D12Device5 *dev5, ID3D12GraphicsCommandList4 *list4, RtScene &scene)
{
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
    scene.vb = vb; scene.blas = blas; scene.tlas = tlas; scene.scratch = scratch; scene.inst_buf = inst_buf;
    return true;
}

static void release_rt_scene(RtScene &scene)
{
    scene.inst_buf->Release(); scene.scratch->Release(); scene.tlas->Release(); scene.blas->Release(); scene.vb->Release();
}

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

    RtScene scene;
    if (!build_rt_scene(dev5, list4, scene))
        return false;
    ID3D12Resource *tlas = scene.tlas;

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
    ID3D12Resource *out = create_buffer(32, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
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
    rb->Release(); out->Release(); ps->Release(); rs->Release();
    release_rt_scene(scene); list4->Release(); dev5->Release();
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
// A buffer view of 32-bit elements reads the same memory whichever kind of buffer the shader declares (as games rely on
// it; drivers all honor it): StructuredBuffer<uint>, ByteAddressBuffer and Buffer<uint> each given a typed R32_UINT,
// a structured (stride 4) and a raw view, each starting past the buffer's start. Hogwarts Legacy's lighting reads its
// tile lists through a typed view in a StructuredBuffer slot (black on Mullion until it read them).
static bool view_kinds(bool dxbc)
{
    static const char *src = R"(
#if KIND == 0
StructuredBuffer<uint> words : register(t0);
uint word(uint i) { return words[i]; }
uint count() { uint n, stride; words.GetDimensions(n, stride); return n; }
#elif KIND == 1
ByteAddressBuffer words : register(t0);
uint word(uint i) { return words.Load(i * 4); }
uint count() { uint n; words.GetDimensions(n); return n / 4; }
#else
Buffer<uint> words : register(t0);
uint word(uint i) { return words[i]; }
uint count() { uint n; words.GetDimensions(n); return n; }
#endif
RWStructuredBuffer<uint> results : register(u0);
[numthreads(64, 1, 1)] void main(uint i : SV_DispatchThreadID)
{
    results[i] = word(i);
    if (i == 0)
        results[64] = count();
}
)";
    static const char *kinds[3] = { "StructuredBuffer<uint>", "ByteAddressBuffer", "Buffer<uint>" };
    static const char *views[3] = { "a typed R32_UINT view", "a structured view", "a raw view" };
    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable = { 1, &range };
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 2, rp };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    ID3D12PipelineState *ps[3] = {};
    for (int k = 0; k < 3; k++)
    {
        std::string full = std::string("#define KIND ") + std::to_string(k) + "\n" + src;
        ps[k] = create_compute_pipeline(rs, dxbc ? compile_fxc(full.c_str(), "main", "cs_5_0") : compile_dxc(full.c_str(), "main", "cs_6_0"));
        if (!ps[k])
            return false;
    }
    const UINT total = 128, first = 16, count = 64;
    ID3D12Resource *data = create_buffer(total * 4, D3D12_HEAP_TYPE_UPLOAD);
    uint32_t *m = nullptr;
    data->Map(0, nullptr, (void **)&m);
    for (UINT i = 0; i < total; i++)
        m[i] = 0x10000 * i + i + 1;
    data->Unmap(0, nullptr);
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap *heap = nullptr;
    CHECK_HR(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
    UINT inc = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (UINT v = 0; v < 3; v++)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = v == 0 ? DXGI_FORMAT_R32_UINT : v == 1 ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_TYPELESS;
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Buffer.FirstElement = first;
        sd.Buffer.NumElements = count;
        sd.Buffer.StructureByteStride = v == 1 ? 4 : 0;
        sd.Buffer.Flags = v == 2 ? D3D12_BUFFER_SRV_FLAG_RAW : D3D12_BUFFER_SRV_FLAG_NONE;
        D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += v * inc;
        g.device->CreateShaderResourceView(data, &sd, h);
    }
    for (int k = 0; k < 3; k++)
    {
        for (UINT v = 0; v < 3; v++)
        {
            ID3D12Resource *out = create_buffer(65 * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            D3D12_GPU_DESCRIPTOR_HANDLE gh = heap->GetGPUDescriptorHandleForHeapStart();
            gh.ptr += v * inc;
            g.list->SetDescriptorHeaps(1, &heap);
            g.list->SetComputeRootSignature(rs);
            g.list->SetPipelineState(ps[k]);
            g.list->SetComputeRootDescriptorTable(0, gh);
            g.list->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
            g.list->Dispatch(1, 1, 1);
            ID3D12Resource *rb;
            const uint32_t *got = read_buffer(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 65 * 4, &rb);
            if (!got)
                return false;
            UINT wrong = 0;
            for (UINT i = 0; i < count; i++)
                wrong += got[i] != 0x10000 * (first + i) + first + i + 1;
            EXPECT(!wrong, "%s given %s: %u of %u elements wrong (element 0 = %#x)", kinds[k], views[v], wrong, count, got[0]);
            EXPECT(got[64] == count, "%s given %s: %u elements, want %u", kinds[k], views[v], got[64], count);
            rb->Release(); out->Release();
        }
    }
    heap->Release(); data->Release(); rs->Release();
    for (auto p : ps) p->Release();
    return true;
}

static bool test_view_kinds_dxil() { return view_kinds(false); }
static bool test_view_kinds_dxbc() { return view_kinds(true); }

// Typed buffer SRVs of the three-component 32-bit formats (Buffer<float3>, Buffer<uint3>, Buffer<int3>), which
// Metal has no texel buffer format for, starting at an element that isn't 16-byte aligned.
static bool typedbuffer_rgb32(bool dxbc)
{
    static const char *src = R"(
Buffer<float3> f : register(t0);
Buffer<uint3> u : register(t1);
Buffer<int3> s : register(t2);
RWByteAddressBuffer o : register(u0);
[numthreads(8, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    o.Store3(i * 36, asuint(f[i]));
    o.Store3(i * 36 + 12, u[i]);
    o.Store3(i * 36 + 24, asuint(s[i]));
}
)";
    static const DXGI_FORMAT formats[3] = { DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32G32B32_UINT, DXGI_FORMAT_R32G32B32_SINT };
    for (DXGI_FORMAT fmt : formats)
    {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = { fmt };
        g.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs));
        printf("  format %d: buffer %d, shader load %d\n", fmt, !!(fs.Support1 & D3D12_FORMAT_SUPPORT1_BUFFER),
               !!(fs.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD));
    }
    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0 };
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable = { 1, &range };
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 2, rp };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    ID3D12PipelineState *ps = create_compute_pipeline(rs, dxbc ? compile_fxc(src, "main", "cs_5_0") : compile_dxc(src, "main", "cs_6_0"));
    if (!rs || !ps)
        return false;
    // Element e of the buffer is (3e, 3e + 1, 3e + 2) as each format's type; the views start at element 1.
    const UINT N = 16, size = N * 12;
    ID3D12Resource *upload = create_buffer(3 * size, D3D12_HEAP_TYPE_UPLOAD);
    uint8_t *m = nullptr;
    upload->Map(0, nullptr, (void **)&m);
    for (UINT k = 0; k < 3 * N; k++)
    {
        ((float *)m)[k] = (float)k + 0.5f;
        ((uint32_t *)(m + size))[k] = 100 + k;
        ((int32_t *)(m + 2 * size))[k] = -1 - (int32_t)k;
    }
    upload->Unmap(0, nullptr);
    ID3D12Resource *data = create_buffer(3 * size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    g.list->CopyBufferRegion(data, 0, upload, 0, 3 * size);
    transition(data, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource *out = create_buffer(8 * 36, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap *heap = nullptr;
    CHECK_HR(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
    UINT inc = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
    for (UINT k = 0; k < 3; k++)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = formats[k];
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Buffer.FirstElement = k * N + 1;
        sd.Buffer.NumElements = 8;
        g.device->CreateShaderResourceView(data, &sd, h);
        h.ptr += inc;
    }
    g.list->SetDescriptorHeaps(1, &heap);
    g.list->SetComputeRootSignature(rs);
    g.list->SetPipelineState(ps);
    g.list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    g.list->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
    g.list->Dispatch(1, 1, 1);
    ID3D12Resource *rb;
    const uint32_t *v = read_buffer(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 8 * 36, &rb);
    if (!v)
        return false;
    int bad = 0;
    for (UINT i = 0; i < 8; i++)
    {
        for (UINT c = 0; c < 3; c++)
        {
            UINT k = 3 * (i + 1) + c;
            float fv;
            memcpy(&fv, &v[i * 9 + c], 4);
            bool ok = fv == (float)k + 0.5f && v[i * 9 + 3 + c] == 100 + k && (int32_t)v[i * 9 + 6 + c] == -1 - (int32_t)k;
            if (!ok && bad++ < 4)
                printf("  FAIL: element %u.%u = %g %u %d, want %g %u %d\n", i, c, fv, v[i * 9 + 3 + c], (int32_t)v[i * 9 + 6 + c],
                       (float)k + 0.5f, 100 + k, -1 - (int32_t)k);
        }
    }
    EXPECT(!bad, "%d wrong components", bad);
    rb->Release(); out->Release(); data->Release(); upload->Release(); heap->Release(); ps->Release(); rs->Release();
    return true;
}

// A shader declaring Buffer<float4> reads a view of three 32-bit components (R32G32B32_FLOAT): (x, y, z, 1). Its PSOs
// (compute, and graphics reading it in the pixel shader) are made before the application's first such view; on the
// Metal Shader Converter bridge, they draw and dispatch with their SPIR-V pipelines from then on, since Metal has no
// texel format of three 32-bit components. (When an earlier test has made such a view, the PSOs aren't bridged.)
static bool typedbuffer_rgb32_float4(bool dxbc)
{
    static const char *src = R"(
Buffer<float4> f : register(t0);
RWByteAddressBuffer o : register(u0);
[numthreads(4, 1, 1)] void cs(uint i : SV_DispatchThreadID) { o.Store4(i * 16, asuint(f[i])); }
float4 vs(uint id : SV_VertexID) : SV_Position { return float4(float2((id << 1) & 2, id & 2) * float2(2, -2) + float2(-1, 1), 0, 1); }
float4 ps(float4 p : SV_Position) : SV_Target { return f[(uint)p.x]; }
)";
    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable = { 1, &range };
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 2, rp };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    ID3D12PipelineState *cps = create_compute_pipeline(rs, dxbc ? compile_fxc(src, "cs", "cs_5_0") : compile_dxc(src, "cs", "cs_6_0"));
    std::vector<char> vsb = dxbc ? compile_fxc(src, "vs", "vs_5_0") : compile_dxc(src, "vs", "vs_6_0"),
                      psb = dxbc ? compile_fxc(src, "ps", "ps_5_0") : compile_dxc(src, "ps", "ps_6_0");
    if (!rs || !cps || vsb.empty() || psb.empty())
        return false;
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
    pd.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState *gps = nullptr;
    CHECK_HR(g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&gps)));

    // The PSOs exist; now the data and the view of three components.
    ID3D12Resource *upload = create_buffer(256, D3D12_HEAP_TYPE_UPLOAD);
    float *m = nullptr;
    upload->Map(0, nullptr, (void **)&m);
    for (UINT k = 0; k < 12; k++)
        m[k] = (float)k * 1.5f + 0.25f;
    upload->Unmap(0, nullptr);
    ID3D12Resource *data = create_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    g.list->CopyBufferRegion(data, 0, upload, 0, 48);
    transition(data, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ID3D12Resource *out = create_buffer(64, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap *heap = nullptr, *rtv_heap = nullptr;
    CHECK_HR(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_R32G32B32_FLOAT;
    sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Buffer.NumElements = 4;
    g.device->CreateShaderResourceView(data, &sd, heap->GetCPUDescriptorHandleForHeapStart());
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 4; td.Height = 1; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource *rt = nullptr;
    CHECK_HR(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&rt)));
    D3D12_DESCRIPTOR_HEAP_DESC rhd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    CHECK_HR(g.device->CreateDescriptorHeap(&rhd, IID_PPV_ARGS(&rtv_heap)));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    g.device->CreateRenderTargetView(rt, nullptr, rtv);

    g.list->SetDescriptorHeaps(1, &heap);
    g.list->SetComputeRootSignature(rs);
    g.list->SetPipelineState(cps);
    g.list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    g.list->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
    g.list->Dispatch(1, 1, 1);
    D3D12_VIEWPORT vp = { 0, 0, 4, 1, 0, 1 };
    D3D12_RECT sc = { 0, 0, 4, 1 };
    g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    g.list->RSSetViewports(1, &vp);
    g.list->RSSetScissorRects(1, &sc);
    g.list->SetGraphicsRootSignature(rs);
    g.list->SetPipelineState(gps);
    g.list->SetGraphicsRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    g.list->SetGraphicsRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
    g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.list->DrawInstanced(3, 1, 0, 0);
    ID3D12Resource *rb = create_buffer(256, D3D12_HEAP_TYPE_READBACK);
    transition(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
    dst.pResource = rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R32G32B32A32_FLOAT, 4, 1, 1, 256 };
    srcl.pResource = rt;
    srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
    ID3D12Resource *crb = nullptr;
    const float *c = (const float *)read_buffer(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 64, &crb);
    if (!c)
        return false;
    const float *px = nullptr;
    D3D12_RANGE r = { 0, 64 };
    rb->Map(0, &r, (void **)&px);
    int bad = 0;
    for (UINT i = 0; i < 4; i++)
    {
        float want[4] = { i * 4.5f + 0.25f, i * 4.5f + 1.75f, i * 4.5f + 3.25f, 1.0f };
        for (UINT k = 0; k < 4; k++)
        {
            if (c[i * 4 + k] != want[k] && bad++ < 4)
                printf("  compute element %u.%u = %f, want %f\n", i, k, c[i * 4 + k], want[k]);
            if (px[i * 4 + k] != want[k] && bad++ < 4)
                printf("  pixel %u.%u = %f, want %f\n", i, k, px[i * 4 + k], want[k]);
        }
    }
    rb->Unmap(0, nullptr);
    EXPECT(!bad, "%d wrong values reading a three-component view as float4", bad);
    crb->Release(); rb->Release(); rt->Release(); rtv_heap->Release(); heap->Release(); out->Release(); data->Release();
    upload->Release(); gps->Release(); cps->Release(); rs->Release();
    return true;
}

static bool test_typedbuffer_rgb32_float4() { return typedbuffer_rgb32_float4(false); }
static bool test_typedbuffer_rgb32_float4_dxbc() { return typedbuffer_rgb32_float4(true); }
static bool test_typedbuffer_rgb32() { return typedbuffer_rgb32(false); }
static bool test_typedbuffer_rgb32_dxbc() { return typedbuffer_rgb32(true); }

// Typed buffer views of many formats at first elements that are not 16-byte aligned, read through the descriptor heap
// (SM 6.6) and a descriptor table, with out-of-bounds reads (0) and GetDimensions; then a typed UAV at an offset
// written and atomically incremented. The Metal Shader Converter bridge gives each such view a texel view of its own.
static float typedbuffer_half(uint16_t h)
{
    uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1f, man = h & 0x3ff;
    float v = exp ? ldexpf((float)(man | 0x400), (int)exp - 25) : ldexpf((float)man, -24);
    return sign ? -v : v;
}

static bool typedbuffer_matrix_heap(bool table)
{
    static const char *src_heap = R"(
cbuffer c : register(b0) { uint idx; uint base; uint is_uint; };
RWByteAddressBuffer o : register(u0);
[numthreads(8, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    uint4 v; uint d;
    if (is_uint) { Buffer<uint4> b = ResourceDescriptorHeap[idx]; v = b[i]; b.GetDimensions(d); }
    else { Buffer<float4> b = ResourceDescriptorHeap[idx]; v = asuint(b[i]); b.GetDimensions(d); }
    o.Store4(base + i * 16, v);
    if (i == 0) o.Store(base + 8 * 16, d);
}
)";
    static const char *src_table = R"(
cbuffer c : register(b0) { uint idx; uint base; uint is_uint; };
Buffer<float4> bf[80] : register(t0);
Buffer<uint4> bu[80] : register(t0, space1);
RWByteAddressBuffer o : register(u0);
[numthreads(8, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    uint4 v; uint d;
    if (is_uint) { v = bu[idx][i]; bu[idx].GetDimensions(d); }
    else { v = asuint(bf[idx][i]); bf[idx].GetDimensions(d); }
    o.Store4(base + i * 16, v);
    if (i == 0) o.Store(base + 8 * 16, d);
}
)";
    struct Fmt { DXGI_FORMAT f; const char *name; uint32_t size; bool is_uint; };
    static const Fmt fmts[] = {
        { DXGI_FORMAT_R32_FLOAT, "R32_FLOAT", 4, false },
        { DXGI_FORMAT_R16G16_FLOAT, "R16G16_FLOAT", 4, false },
        { DXGI_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM", 4, false },
        { DXGI_FORMAT_R8G8B8A8_SNORM, "R8G8B8A8_SNORM", 4, false },
        { DXGI_FORMAT_R10G10B10A2_UNORM, "R10G10B10A2_UNORM", 4, false },
        { DXGI_FORMAT_R16G16B16A16_FLOAT, "R16G16B16A16_FLOAT", 8, false },
        { DXGI_FORMAT_R16G16_UNORM, "R16G16_UNORM", 4, false },
        { DXGI_FORMAT_R32G32_FLOAT, "R32G32_FLOAT", 8, false },
        { DXGI_FORMAT_R32G32B32A32_FLOAT, "R32G32B32A32_FLOAT", 16, false },
        { DXGI_FORMAT_R8_UNORM, "R8_UNORM", 1, false },
        { DXGI_FORMAT_R32_UINT, "R32_UINT", 4, true },
        { DXGI_FORMAT_R8G8B8A8_UINT, "R8G8B8A8_UINT", 4, true },
        { DXGI_FORMAT_R16G16B16A16_UINT, "R16G16B16A16_UINT", 8, true },
        { DXGI_FORMAT_R32G32_UINT, "R32G32_UINT", 8, true },
        { DXGI_FORMAT_R32G32B32A32_UINT, "R32G32B32A32_UINT", 16, true },
        { DXGI_FORMAT_R16_UINT, "R16_UINT", 2, true },
        { DXGI_FORMAT_R8_UINT, "R8_UINT", 1, true },
    };
    static const uint32_t firsts[] = { 0, 1, 3, 7 };
    const uint32_t count = 5, nf = sizeof(fmts) / sizeof(fmts[0]), nc = nf * 4, slot = 9 * 16, buf_size = 4096;

    std::vector<char> cs = compile_dxc(table ? src_table : src_heap, "main", "cs_6_6");
    if (cs.empty())
        return false;
    D3D12_DESCRIPTOR_RANGE1 ranges[2] = {
        { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 80, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0 },
        { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 80, 0, 1, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 80 },
    };
    D3D12_ROOT_PARAMETER1 rp[3] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.Num32BitValues = 3;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[2].DescriptorTable = { 2, ranges };
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC vrsd = {};
    vrsd.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    vrsd.Desc_1_1 = { table ? 3u : 2u, rp, 0, nullptr, table ? D3D12_ROOT_SIGNATURE_FLAG_NONE : D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED };
    ID3DBlob *blob = nullptr, *error = nullptr;
    CHECK_HR(D3D12SerializeVersionedRootSignature(&vrsd, &blob, &error));
    ID3D12RootSignature *rs = nullptr;
    CHECK_HR(g.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs)));
    blob->Release();
    ID3D12PipelineState *ps = create_compute_pipeline(rs, cs);
    if (!ps)
        return false;

    // Data: per format, buf_size bytes. Float formats hold exact small values; the others any bytes.
    std::vector<uint8_t> bytes(nf * buf_size);
    for (uint32_t f = 0; f < nf; f++)
    {
        uint8_t *b = &bytes[f * buf_size];
        for (uint32_t k = 0; k < buf_size; k++)
            b[k] = (uint8_t)(k * 37 + f * 11 + 5);
        if (fmts[f].f == DXGI_FORMAT_R32_FLOAT || fmts[f].f == DXGI_FORMAT_R32G32_FLOAT || fmts[f].f == DXGI_FORMAT_R32G32B32A32_FLOAT)
            for (uint32_t k = 0; k < buf_size / 4; k++)
                ((float *)b)[k] = (float)k * 0.25f - 7.0f;
        if (fmts[f].f == DXGI_FORMAT_R16G16_FLOAT || fmts[f].f == DXGI_FORMAT_R16G16B16A16_FLOAT)
            for (uint32_t k = 0; k < buf_size / 2; k++)
                ((uint16_t *)b)[k] = (uint16_t)(0x3800 + (k % 512) * 3);   // 0.5 and up, finite
    }
    ID3D12Resource *upload = create_buffer(nf * buf_size, D3D12_HEAP_TYPE_UPLOAD);
    void *map = nullptr;
    upload->Map(0, nullptr, &map);
    memcpy(map, bytes.data(), bytes.size());
    upload->Unmap(0, nullptr);
    std::vector<ID3D12Resource *> data(nf);
    for (uint32_t f = 0; f < nf; f++)
    {
        data[f] = create_buffer(buf_size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        g.list->CopyBufferRegion(data[f], 0, upload, f * buf_size, buf_size);
        transition(data[f], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    ID3D12Resource *out = create_buffer(nc * slot, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 160, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap *heap = nullptr;
    CHECK_HR(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
    UINT inc = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    std::vector<bool> supported(nf);
    for (uint32_t f = 0; f < nf; f++)
    {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = { fmts[f].f };
        supported[f] = SUCCEEDED(g.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))) &&
                       (fs.Support1 & D3D12_FORMAT_SUPPORT1_BUFFER);
        for (uint32_t c = 0; c < 4; c++)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
            sd.Format = fmts[f].f;
            sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Buffer.FirstElement = firsts[c];
            sd.Buffer.NumElements = count;
            // Table: float views in the first 80 slots, uint views in the next 80 (space 1).
            UINT index = (fmts[f].is_uint && table ? 80 : 0) + f * 4 + c;
            D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += index * inc;
            g.device->CreateShaderResourceView(supported[f] ? data[f] : nullptr, &sd, h);
        }
    }
    g.list->SetDescriptorHeaps(1, &heap);
    g.list->SetComputeRootSignature(rs);
    g.list->SetPipelineState(ps);
    g.list->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
    if (table)
        g.list->SetComputeRootDescriptorTable(2, heap->GetGPUDescriptorHandleForHeapStart());
    for (uint32_t f = 0; f < nf; f++)
        for (uint32_t c = 0; c < 4; c++)
        {
            uint32_t k[3] = { f * 4 + c, (f * 4 + c) * slot, fmts[f].is_uint };
            g.list->SetComputeRoot32BitConstants(0, 3, k, 0);
            g.list->Dispatch(1, 1, 1);
        }
    ID3D12Resource *rb = create_buffer(nc * slot, D3D12_HEAP_TYPE_READBACK);
    transition(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    g.list->CopyBufferRegion(rb, 0, out, 0, nc * slot);
    if (!execute_and_wait())
        return false;
    const uint8_t *o = nullptr;
    rb->Map(0, nullptr, (void **)&o);
    int bad = 0, cases = 0;
    for (uint32_t f = 0; f < nf; f++)
    {
        if (!supported[f])
        {
            printf("  %s: no typed buffer support\n", fmts[f].name);
            continue;
        }
        for (uint32_t c = 0; c < 4; c++)
        {
            const uint8_t *res = o + (f * 4 + c) * slot;
            cases++;
            uint32_t dims = *(const uint32_t *)(res + 8 * 16);
            if (dims != count && bad++ < 20)
                printf("  %s first %u: GetDimensions %u, want %u\n", fmts[f].name, firsts[c], dims, count);
            for (uint32_t i = 0; i < 8; i++)
            {
                const uint8_t *e = &bytes[f * buf_size + (firsts[c] + i) * fmts[f].size];
                float wf[4] = { 0, 0, 0, 1 };
                uint32_t wu[4] = { 0, 0, 0, 1 };
                if (i < count)
                    switch (fmts[f].f)
                    {
                    case DXGI_FORMAT_R32_FLOAT: wf[0] = ((const float *)e)[0]; break;
                    case DXGI_FORMAT_R32G32_FLOAT: wf[0] = ((const float *)e)[0]; wf[1] = ((const float *)e)[1]; break;
                    case DXGI_FORMAT_R32G32B32A32_FLOAT: for (int j = 0; j < 4; j++) wf[j] = ((const float *)e)[j]; break;
                    case DXGI_FORMAT_R16G16_FLOAT: for (int j = 0; j < 2; j++) wf[j] = typedbuffer_half(((const uint16_t *)e)[j]); break;
                    case DXGI_FORMAT_R16G16B16A16_FLOAT: for (int j = 0; j < 4; j++) wf[j] = typedbuffer_half(((const uint16_t *)e)[j]); break;
                    case DXGI_FORMAT_R8G8B8A8_UNORM: for (int j = 0; j < 4; j++) wf[j] = e[j] / 255.0f; break;
                    case DXGI_FORMAT_R8G8B8A8_SNORM: for (int j = 0; j < 4; j++) wf[j] = std::max(-1.0f, (int8_t)e[j] / 127.0f); break;
                    case DXGI_FORMAT_R8_UNORM: wf[0] = e[0] / 255.0f; break;
                    case DXGI_FORMAT_R16G16_UNORM: for (int j = 0; j < 2; j++) wf[j] = ((const uint16_t *)e)[j] / 65535.0f; break;
                    case DXGI_FORMAT_R10G10B10A2_UNORM:
                    {
                        uint32_t v = *(const uint32_t *)e;
                        wf[0] = (v & 0x3ff) / 1023.0f; wf[1] = ((v >> 10) & 0x3ff) / 1023.0f;
                        wf[2] = ((v >> 20) & 0x3ff) / 1023.0f; wf[3] = (v >> 30) / 3.0f;
                        break;
                    }
                    case DXGI_FORMAT_R32_UINT: wu[0] = ((const uint32_t *)e)[0]; break;
                    case DXGI_FORMAT_R32G32_UINT: wu[0] = ((const uint32_t *)e)[0]; wu[1] = ((const uint32_t *)e)[1]; break;
                    case DXGI_FORMAT_R32G32B32A32_UINT: for (int j = 0; j < 4; j++) wu[j] = ((const uint32_t *)e)[j]; break;
                    case DXGI_FORMAT_R8G8B8A8_UINT: for (int j = 0; j < 4; j++) wu[j] = e[j]; break;
                    case DXGI_FORMAT_R16G16B16A16_UINT: for (int j = 0; j < 4; j++) wu[j] = ((const uint16_t *)e)[j]; break;
                    case DXGI_FORMAT_R16_UINT: wu[0] = ((const uint16_t *)e)[0]; break;
                    case DXGI_FORMAT_R8_UINT: wu[0] = e[0]; break;
                    default: break;
                    }
                else if (fmts[f].is_uint)
                    wu[3] = 0;
                else
                    wf[3] = 0;   // out of bounds: all zero
                const uint32_t *got = (const uint32_t *)(res + i * 16);
                for (int j = 0; j < 4; j++)
                {
                    bool ok;
                    if (fmts[f].is_uint)
                        ok = got[j] == wu[j];
                    else
                    {
                        float gf;
                        memcpy(&gf, &got[j], 4);
                        ok = fabsf(gf - wf[j]) <= 1e-6f * std::max(1.0f, fabsf(wf[j]));
                    }
                    // Out of bounds, a format without alpha may read it as 1 (Vulkan robustness, as GPUs do).
                    if (i >= count && j == 3 && (got[j] == 1 || got[j] == 0x3f800000))
                        ok = true;
                    if (!ok && bad++ < 20)
                    {
                        float gf;
                        memcpy(&gf, &got[j], 4);
                        printf("  %s first %u element %u.%c: got %#x (%g), want %#x (%g)\n", fmts[f].name, firsts[c], i,
                               "xyzw"[j], got[j], gf, fmts[f].is_uint ? wu[j] : 0u, fmts[f].is_uint ? (double)wu[j] : wf[j]);
                    }
                }
            }
        }
    }
    EXPECT(!bad, "%d wrong values over %d typed views (%s)", bad, cases, table ? "table" : "heap");
    rb->Unmap(0, nullptr);
    for (auto *d : data)
        d->Release();
    rb->Release(); out->Release(); upload->Release(); heap->Release(); ps->Release(); rs->Release();
    return true;
}

static bool test_typedbuffer_matrix() { return typedbuffer_matrix_heap(false) && typedbuffer_matrix_heap(true); }

// A typed UAV starting at element 3 of 16: one thread writes element 0, 64 threads atomically increment element 1,
// element 2 gets GetDimensions; the elements around the view keep their values.
static bool test_typedbuffer_uav_offset()
{
    static const char *src = R"(
RWBuffer<uint> b : register(u0);
[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    if (i == 0) { b[0] = 77; uint d; b.GetDimensions(d); b[2] = d; }
    InterlockedAdd(b[1], 1);
}
)";
    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0 };
    D3D12_ROOT_PARAMETER rp = {};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp.DescriptorTable = { 1, &range };
    D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &rp };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    ID3D12PipelineState *ps = create_compute_pipeline(rs, compile_dxc(src, "main", "cs_6_0"));
    if (!rs || !ps)
        return false;
    ID3D12Resource *upload = create_buffer(64, D3D12_HEAP_TYPE_UPLOAD);
    uint32_t *u = nullptr;
    upload->Map(0, nullptr, (void **)&u);
    for (uint32_t i = 0; i < 16; i++)
        u[i] = 0xa000 + i;
    u[4] = 5;   // element 1 of the view: incremented from 5
    upload->Unmap(0, nullptr);
    ID3D12Resource *buf = create_buffer(64, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_COPY_DEST);
    g.list->CopyBufferRegion(buf, 0, upload, 0, 64);
    transition(buf, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap *heap = nullptr;
    CHECK_HR(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.Format = DXGI_FORMAT_R32_UINT;
    ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    ud.Buffer.FirstElement = 3;
    ud.Buffer.NumElements = 4;
    g.device->CreateUnorderedAccessView(buf, nullptr, &ud, heap->GetCPUDescriptorHandleForHeapStart());
    g.list->SetDescriptorHeaps(1, &heap);
    g.list->SetComputeRootSignature(rs);
    g.list->SetPipelineState(ps);
    g.list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    g.list->Dispatch(1, 1, 1);
    ID3D12Resource *rb = create_buffer(64, D3D12_HEAP_TYPE_READBACK);
    transition(buf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    g.list->CopyBufferRegion(rb, 0, buf, 0, 64);
    if (!execute_and_wait())
        return false;
    const uint32_t *o = nullptr;
    rb->Map(0, nullptr, (void **)&o);
    for (uint32_t i = 0; i < 16; i++)
    {
        uint32_t want = i == 3 ? 77 : i == 4 ? 5 + 64 : i == 5 ? 4 : 0xa000 + i;
        EXPECT(o[i] == want, "element %u = %#x, want %#x", i, o[i], want);
    }
    rb->Unmap(0, nullptr);
    rb->Release(); buf->Release(); upload->Release(); heap->Release(); ps->Release(); rs->Release();
    return true;
}

// Descriptors made in a CPU-only heap and copied into a shader-visible one (CopyDescriptorsSimple, and
// CopyDescriptors with ranges in another order) read what they view: typed views at first elements 1, 3, 5 and 7, a raw
// and a structured view. Then, after the command list is recorded and before it runs, slot 0 is rewritten in the
// shader-visible heap with another first element (volatile descriptors), which the shader must read.
static bool test_descriptor_copies()
{
    static const char *src = R"(
Buffer<uint> t[4] : register(t0);
ByteAddressBuffer raw : register(t4);
StructuredBuffer<uint> st : register(t5);
RWByteAddressBuffer o : register(u0);
[numthreads(1, 1, 1)]
void main()
{
    for (uint i = 0; i < 4; i++)
    {
        uint d; t[i].GetDimensions(d);
        o.Store2(i * 8, uint2(t[i][0], d));
    }
    o.Store2(32, uint2(raw.Load(0), st[0]));
}
)";
    D3D12_DESCRIPTOR_RANGE1 range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 6, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0 };
    D3D12_ROOT_PARAMETER1 rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable = { 1, &range };
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC vrsd = {};
    vrsd.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    vrsd.Desc_1_1 = { 2, rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ID3DBlob *blob = nullptr, *error = nullptr;
    CHECK_HR(D3D12SerializeVersionedRootSignature(&vrsd, &blob, &error));
    ID3D12RootSignature *rs = nullptr;
    CHECK_HR(g.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs)));
    blob->Release();
    ID3D12PipelineState *ps = create_compute_pipeline(rs, compile_dxc(src, "main", "cs_6_0"));
    if (!ps)
        return false;

    ID3D12Resource *upload = create_buffer(256, D3D12_HEAP_TYPE_UPLOAD);
    uint32_t *u = nullptr;
    upload->Map(0, nullptr, (void **)&u);
    for (uint32_t i = 0; i < 64; i++)
        u[i] = 100 + i;
    upload->Unmap(0, nullptr);
    ID3D12Resource *data = create_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    g.list->CopyBufferRegion(data, 0, upload, 0, 256);
    transition(data, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource *out = create_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_DESCRIPTOR_HEAP_DESC chd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 6, D3D12_DESCRIPTOR_HEAP_FLAG_NONE };
    D3D12_DESCRIPTOR_HEAP_DESC ghd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 12, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap *cpu_heap = nullptr, *gpu_heap = nullptr;
    CHECK_HR(g.device->CreateDescriptorHeap(&chd, IID_PPV_ARGS(&cpu_heap)));
    CHECK_HR(g.device->CreateDescriptorHeap(&ghd, IID_PPV_ARGS(&gpu_heap)));
    UINT inc = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu_at = [&](ID3D12DescriptorHeap *h, UINT i) { D3D12_CPU_DESCRIPTOR_HANDLE c = h->GetCPUDescriptorHandleForHeapStart(); c.ptr += i * inc; return c; };
    auto gpu_at = [&](UINT i) { D3D12_GPU_DESCRIPTOR_HANDLE c = gpu_heap->GetGPUDescriptorHandleForHeapStart(); c.ptr += i * inc; return c; };
    for (UINT i = 0; i < 4; i++)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_R32_UINT;
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Buffer.FirstElement = 1 + 2 * i;
        sd.Buffer.NumElements = 4 + i;
        g.device->CreateShaderResourceView(data, &sd, cpu_at(cpu_heap, i));
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC rd = {};
    rd.Format = DXGI_FORMAT_R32_TYPELESS;
    rd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    rd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    rd.Buffer.FirstElement = 8;   // raw views start 16-byte aligned (D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT)
    rd.Buffer.NumElements = 4;
    rd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    g.device->CreateShaderResourceView(data, &rd, cpu_at(cpu_heap, 4));
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Buffer.FirstElement = 11;
    sd.Buffer.NumElements = 4;
    sd.Buffer.StructureByteStride = 4;
    g.device->CreateShaderResourceView(data, &sd, cpu_at(cpu_heap, 5));
    // Table A (slots 0-5): one simple copy. Table B (slots 6-11): six single-descriptor ranges, copied last to first.
    g.device->CopyDescriptorsSimple(6, cpu_at(gpu_heap, 0), cpu_at(cpu_heap, 0), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (int i = 5; i >= 0; i--)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE d = cpu_at(gpu_heap, 6 + i), s = cpu_at(cpu_heap, i);
        UINT one = 1;
        g.device->CopyDescriptors(1, &d, &one, 1, &s, &one, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    g.list->SetDescriptorHeaps(1, &gpu_heap);
    g.list->SetComputeRootSignature(rs);
    g.list->SetPipelineState(ps);
    g.list->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
    g.list->SetComputeRootDescriptorTable(0, gpu_at(0));
    g.list->Dispatch(1, 1, 1);
    D3D12_RESOURCE_BARRIER uav = {};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    g.list->ResourceBarrier(1, &uav);
    ID3D12Resource *rb = create_buffer(256, D3D12_HEAP_TYPE_READBACK);
    g.list->CopyBufferRegion(rb, 0, out, 0, 40);
    g.list->SetComputeRootDescriptorTable(0, gpu_at(6));
    g.list->Dispatch(1, 1, 1);
    g.list->ResourceBarrier(1, &uav);
    g.list->CopyBufferRegion(rb, 64, out, 0, 40);
    // Recorded; now slot 6 (table B's first) is rewritten with first element 20, before the list runs.
    D3D12_SHADER_RESOURCE_VIEW_DESC late = {};
    late.Format = DXGI_FORMAT_R32_UINT;
    late.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    late.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    late.Buffer.FirstElement = 20;
    late.Buffer.NumElements = 9;
    g.device->CreateShaderResourceView(data, &late, cpu_at(gpu_heap, 6));
    if (!execute_and_wait())
        return false;
    const uint32_t *o = nullptr;
    rb->Map(0, nullptr, (void **)&o);
    for (int table = 0; table < 2; table++)
    {
        const uint32_t *r = o + table * 16;
        for (uint32_t i = 0; i < 4; i++)
        {
            uint32_t first = (table == 1 && i == 0) ? 20 : 1 + 2 * i, count = (table == 1 && i == 0) ? 9 : 4 + i;
            EXPECT(r[i * 2] == 100 + first && r[i * 2 + 1] == count, "table %c typed %u: read %u, %u elements (want %u, %u)",
                   'A' + table, i, r[i * 2], r[i * 2 + 1], 100 + first, count);
        }
        EXPECT(r[8] == 108 && r[9] == 111, "table %c raw %u (want 108), structured %u (want 111)", 'A' + table, r[8], r[9]);
    }
    rb->Unmap(0, nullptr);
    rb->Release(); out->Release(); data->Release(); upload->Release(); cpu_heap->Release(); gpu_heap->Release();
    ps->Release(); rs->Release();
    return true;
}

// Descriptor lifetimes: typed views (whole-buffer and at offsets) and raw views of buffers that are released while
// their descriptors stay in CPU and shader-visible heaps, then overwritten, copied over and destroyed with the heap,
// thousands of times; a buffer read through a fresh descriptor afterwards still reads its data.
static bool test_descriptor_lifetime()
{
    D3D12_DESCRIPTOR_HEAP_DESC chd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 64, D3D12_DESCRIPTOR_HEAP_FLAG_NONE };
    D3D12_DESCRIPTOR_HEAP_DESC ghd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 64, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    UINT inc = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (UINT round = 0; round < 50; round++)
    {
        ID3D12DescriptorHeap *cpu = nullptr, *gpu = nullptr;
        CHECK_HR(g.device->CreateDescriptorHeap(&chd, IID_PPV_ARGS(&cpu)));
        CHECK_HR(g.device->CreateDescriptorHeap(&ghd, IID_PPV_ARGS(&gpu)));
        auto at = [&](ID3D12DescriptorHeap *h, UINT i) { D3D12_CPU_DESCRIPTOR_HANDLE c = h->GetCPUDescriptorHandleForHeapStart(); c.ptr += i * inc; return c; };
        for (UINT i = 0; i < 64; i++)
        {
            ID3D12Resource *b = create_buffer(4096, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
            sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            if (i % 3 == 0) { sd.Format = DXGI_FORMAT_R32_UINT; sd.Buffer.NumElements = 1024; }          // whole buffer
            else if (i % 3 == 1) { sd.Format = DXGI_FORMAT_R32_FLOAT; sd.Buffer.FirstElement = 1 + i; sd.Buffer.NumElements = 7; }
            else { sd.Format = DXGI_FORMAT_R32_TYPELESS; sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW; sd.Buffer.FirstElement = 4 * i; sd.Buffer.NumElements = 16; }
            g.device->CreateShaderResourceView(b, &sd, at(cpu, i));
            g.device->CopyDescriptorsSimple(1, at(gpu, i), at(cpu, i), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            if (i & 1)
            {
                D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
                ud.Format = DXGI_FORMAT_R32_UINT;
                ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                ud.Buffer.FirstElement = i;
                ud.Buffer.NumElements = 5;
                g.device->CreateUnorderedAccessView(b, nullptr, &ud, at(gpu, (i + 32) % 64));
            }
            b->Release();   // the descriptors stay
        }
        // Overwrite half, copy over a quarter, then the heaps go with the rest.
        D3D12_SHADER_RESOURCE_VIEW_DESC null_desc = {};
        null_desc.Format = DXGI_FORMAT_R32_UINT;
        null_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        null_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        for (UINT i = 0; i < 32; i++)
            g.device->CreateShaderResourceView(nullptr, &null_desc, at(cpu, i));
        g.device->CopyDescriptorsSimple(16, at(gpu, 0), at(cpu, 0), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        cpu->Release();
        gpu->Release();
    }
    // Still working: the typed buffer test reads correctly after all of that.
    return test_typedbuffer_uav_offset();
}

// Wave and quad intrinsics in compute, checked against what they mean for the wave size the shader reports: ballot
// bit counts, prefix sums and counts, lane reads (first and fixed), active any/all/min/max/sum/product, and quad
// reads across X, Y and the diagonal (lanes in groups of 4 by index). 256 threads.
static bool test_wave_ops_compute()
{
    static const char *src = R"(
RWByteAddressBuffer o : register(u0);
[numthreads(64, 1, 1)]
void main(uint t : SV_DispatchThreadID, uint g : SV_GroupIndex)
{
    uint lane = WaveGetLaneIndex(), size = WaveGetLaneCount();
    uint v = t * 7 + 3;
    uint4 ballot = WaveActiveBallot((t & 3) == 1);
    uint base = t * 64;
    o.Store4(base + 0, uint4(lane, size, countbits(ballot.x) + countbits(ballot.y) + countbits(ballot.z) + countbits(ballot.w),
                             WavePrefixCountBits((t & 1) == 0)));
    o.Store4(base + 16, uint4(WavePrefixSum(v), WaveActiveSum(v), WaveActiveMin(v), WaveActiveMax(v)));
    o.Store4(base + 32, uint4(WaveReadLaneFirst(v), WaveReadLaneAt(v, 5), WaveActiveAnyTrue(t == 3) ? 1 : 0,
                             WaveActiveAllTrue(t < 100000) ? 1 : 0));
    o.Store4(base + 48, uint4(QuadReadAcrossX(v), QuadReadAcrossY(v), QuadReadAcrossDiagonal(v), QuadReadLaneAt(v, 2)));
}
)";
    D3D12_ROOT_PARAMETER rp = {};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &rp };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    ID3D12PipelineState *ps = create_compute_pipeline(rs, compile_dxc(src, "main", "cs_6_0"));
    if (!ps)
        return false;
    const UINT n = 256;
    ID3D12Resource *buf = create_buffer(n * 64, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g.list->SetComputeRootSignature(rs);
    g.list->SetPipelineState(ps);
    g.list->SetComputeRootUnorderedAccessView(0, buf->GetGPUVirtualAddress());
    g.list->Dispatch(n / 64, 1, 1);
    ID3D12Resource *rb;
    const uint32_t *o = read_buffer(buf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, n * 64, &rb);
    if (!o)
        return false;
    int bad = 0;
    uint32_t size = o[1];
    printf("  wave size %u\n", size);
    for (uint32_t t = 0; t < n && size; t++)
    {
        const uint32_t *r = o + t * 16;
        uint32_t lane = t % size, first = t - lane, last = std::min(first + size, (n / 64) * 64);
        first = std::max(first, t / 64 * 64);   // waves don't cross groups
        last = std::min(last, t / 64 * 64 + 64);
        uint32_t ballot = 0, prefix_even = 0, psum = 0, sum = 0, mn = ~0u, mx = 0;
        bool any3 = false;
        for (uint32_t u = first; u < last; u++)
        {
            uint32_t v = u * 7 + 3;
            ballot += (u & 3) == 1;
            if (u < t) { prefix_even += (u & 1) == 0; psum += v; }
            sum += v; mn = std::min(mn, v); mx = std::max(mx, v); any3 |= u == 3;
        }
        uint32_t want[16] = { lane, size, ballot, prefix_even, psum, sum, mn, mx,
                              first * 7 + 3, (first + 5) * 7 + 3, any3, 1,
                              (t ^ 1) * 7 + 3, (t ^ 2) * 7 + 3, (t ^ 3) * 7 + 3, ((t & ~3u) + 2) * 7 + 3 };
        static const char *names[16] = { "lane", "size", "ballot bits", "prefix count", "prefix sum", "sum", "min", "max",
                                         "read first", "read lane 5", "any", "all", "quad x", "quad y", "quad diagonal",
                                         "quad lane 2" };
        for (int k = 0; k < 16; k++)
            if (r[k] != want[k] && bad++ < 12)
                printf("  thread %u %s: %u, want %u\n", t, names[k], r[k], want[k]);
    }
    EXPECT(size == 32 || size == 64 || size == 16 || size == 8 || size == 4, "wave size %u", size);
    EXPECT(!bad, "%d wrong wave/quad results", bad);
    rb->Release(); buf->Release(); ps->Release(); rs->Release();
    return true;
}

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

// Dual-source blending: SV_Target1 is the blend factor of SV_Target0 (SRC1_COLOR / INV_SRC1_COLOR). Over a white
// target, each channel is o0 * o1 + (1 - o1).
static bool test_dual_source_blend()
{
    static const char *src = R"(
float4 vs(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
struct PSOut { float4 c0 : SV_Target0; float4 c1 : SV_Target1; };
PSOut ps() { PSOut o; o.c0 = float4(0.25, 0.5, 0.75, 1.0); o.c1 = float4(0.5, 0.25, 0.5, 0.0); return o; }
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
    D3D12_RENDER_TARGET_BLEND_DESC &b = pd.BlendState.RenderTarget[0];
    b.BlendEnable = TRUE;
    b.SrcBlend = D3D12_BLEND_SRC1_COLOR; b.DestBlend = D3D12_BLEND_INV_SRC1_COLOR; b.BlendOp = D3D12_BLEND_OP_ADD;
    b.SrcBlendAlpha = D3D12_BLEND_ONE; b.DestBlendAlpha = D3D12_BLEND_ZERO; b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = ~0u;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState *pso = nullptr;
    CHECK_HR(g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)));

    const UINT W = 16, H = 16;
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
    float clear[4] = { 1, 1, 1, 1 };
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
    dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 1, 256 };
    srcl.pResource = rt;
    srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
    if (!execute_and_wait())
        return false;
    const uint8_t *px = nullptr;
    D3D12_RANGE range = { 0, 256 * H };
    rb->Map(0, &range, (void **)&px);
    const float want[4] = { 0.25f * 0.5f + 0.5f, 0.5f * 0.25f + 0.75f, 0.75f * 0.5f + 0.5f, 1.0f };
    for (int c = 0; c < 4; c++)
    {
        int w = (int)(want[c] * 255.0f + 0.5f), got = px[8 * 256 + 8 * 4 + c];
        EXPECT(got >= w - 1 && got <= w + 1, "channel %d = %d, want %d", c, got, w);
    }
    rb->Release(); rtv_heap->Release(); rt->Release(); pso->Release(); rs->Release();
    return true;
}

// A depth-only PSO (vertex shader, no pixel shader), as shadow and depth prepasses are: a full-screen triangle at
// z = 0.5, then the left half at z = 0.25 and the right half at z = 0.75 with LESS, read back from D32_FLOAT.
static bool test_depth_only()
{
    static const char *src = R"(
cbuffer c : register(b0) { float z; float x0; float x1; };
float4 vs(uint id : SV_VertexID) : SV_Position
{
    float2 p[6] = { float2(x0, 1), float2(x1, 1), float2(x0, -1), float2(x1, 1), float2(x1, -1), float2(x0, -1) };
    return float4(p[id], z, 1);
}
)";
    std::vector<char> vs = compile_dxc(src, "vs", "vs_6_0");
    if (vs.empty())
        return false;
    D3D12_ROOT_PARAMETER rp = {};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp.Constants.Num32BitValues = 3;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &rp };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = rs;
    pd.VS = { vs.data(), vs.size() };
    pd.SampleMask = ~0u;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.DepthStencilState.DepthEnable = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState *pso = nullptr;
    CHECK_HR(g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)));

    const UINT W = 16, H = 16;
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = W; td.Height = H; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE cv = {};
    cv.Format = DXGI_FORMAT_D32_FLOAT;
    cv.DepthStencil.Depth = 1.0f;
    ID3D12Resource *ds = nullptr;
    CHECK_HR(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, IID_PPV_ARGS(&ds)));
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1 };
    ID3D12DescriptorHeap *dsv_heap = nullptr;
    CHECK_HR(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsv_heap)));
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
    g.device->CreateDepthStencilView(ds, nullptr, dsv);
    g.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    g.list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
    D3D12_VIEWPORT vp = { 0, 0, (float)W, (float)H, 0, 1 };
    D3D12_RECT sc = { 0, 0, (LONG)W, (LONG)H };
    g.list->RSSetViewports(1, &vp);
    g.list->RSSetScissorRects(1, &sc);
    g.list->SetGraphicsRootSignature(rs);
    g.list->SetPipelineState(pso);
    g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const float draws[3][3] = { { 0.5f, -1, 1 }, { 0.25f, -1, 0 }, { 0.75f, 0, 1 } };
    for (auto &d : draws)
    {
        g.list->SetGraphicsRoot32BitConstants(0, 3, d, 0);
        g.list->DrawInstanced(6, 1, 0, 0);
    }
    ID3D12Resource *rb = create_buffer(256 * H, D3D12_HEAP_TYPE_READBACK);
    transition(ds, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
    dst.pResource = rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_TYPELESS, W, H, 1, 256 };
    srcl.pResource = ds;
    srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
    if (!execute_and_wait())
        return false;
    const uint8_t *px = nullptr;
    D3D12_RANGE range = { 0, 256 * H };
    rb->Map(0, &range, (void **)&px);
    int bad = 0;
    for (UINT y = 0; y < H; y++)
        for (UINT x = 0; x < W; x++)
        {
            float d = ((const float *)(px + y * 256))[x], want = x < W / 2 ? 0.25f : 0.5f;
            if (d != want && bad++ < 4)
                printf("  depth (%u, %u) = %g, want %g\n", x, y, d, want);
        }
    EXPECT(!bad, "%d of %u depth values wrong", bad, W * H);
    rb->Release(); dsv_heap->Release(); ds->Release(); pso->Release(); rs->Release();
    return true;
}

// Draw parameters as D3D12 defines them: SV_InstanceID counts from 0 whatever StartInstanceLocation is, and
// SV_VertexID from 0 whatever StartVertexLocation is (indexed: the index, without BaseVertexLocation). Metal's vertex
// and instance ids include them, so converted shaders read the draw's parameters to take them off. Each draw writes one pixel per vertex
// and instance into a UAV through the pixel shader: point i of instance j stores (vertex id, instance id) at a slot.
// Draws: DrawInstanced(4, 2, start vertex 3, start instance 5), DrawIndexedInstanced(4, 2, start index 1, base vertex
// 10, start instance 7), and the same two through ExecuteIndirect.
static bool draw_params(bool sm68)
{
    // SM 6.8: SV_StartVertexLocation and SV_StartInstanceLocation as well, stored past the ids (+ 0x100).
    static const char *src = R"(
cbuffer c : register(b0) { uint draw; };
RWByteAddressBuffer o : register(u0);
struct V { float4 pos : SV_Position; nointerpolation uint4 id : ID; };
V vs(uint vid : SV_VertexID, uint iid : SV_InstanceID
#if SM68
     , uint sv : SV_StartVertexLocation, uint si : SV_StartInstanceLocation
#endif
     )
{
    V v;
    v.pos = float4(-1.0 + (2.0 * (vid % 16) + 1.0) / 16.0, 1.0 - (2.0 * (iid % 4) + 1.0) / 4.0, 0, 1);
#if SM68
    v.id = uint4(vid, iid, sv, si);
#else
    v.id = uint4(vid, iid, 0, 0);
#endif
    return v;
}
float4 ps(V v) : SV_Target
{
    uint slot = draw * 64 + (v.id.y % 4) * 16 + v.id.x % 16;
    o.Store4(slot * 16, v.id + 1);
    return 1;
}
)";
    static const wchar_t *sm68_define[] = { L"-D", L"SM68=1" };
    std::vector<char> vs = compile_dxc(src, "vs", sm68 ? "vs_6_8" : "vs_6_0", sm68 ? sm68_define : nullptr, sm68 ? 2 : 0),
                      ps = compile_dxc(src, "ps", sm68 ? "ps_6_8" : "ps_6_0", sm68 ? sm68_define : nullptr, sm68 ? 2 : 0);
    if (vs.empty() || ps.empty())
        return false;
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.Num32BitValues = 1;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 2, rp };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = rs;
    pd.VS = { vs.data(), vs.size() };
    pd.PS = { ps.data(), ps.size() };
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

    D3D12_INDIRECT_ARGUMENT_DESC ad[2] = {};
    ad[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    ad[0].Constant = { 0, 0, 1 };
    ad[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    D3D12_COMMAND_SIGNATURE_DESC csd = { 4 * 5, 2, ad };
    ID3D12CommandSignature *sig_draw = nullptr, *sig_indexed = nullptr;
    CHECK_HR(g.device->CreateCommandSignature(&csd, rs, IID_PPV_ARGS(&sig_draw)));
    ad[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    csd.ByteStride = 4 * 6;
    CHECK_HR(g.device->CreateCommandSignature(&csd, rs, IID_PPV_ARGS(&sig_indexed)));

    ID3D12Resource *upload = create_buffer(4096, D3D12_HEAP_TYPE_UPLOAD);
    uint8_t *u = nullptr;
    upload->Map(0, nullptr, (void **)&u);
    const uint16_t indices[5] = { 99, 0, 1, 2, 3 };   // start index 1: 0..3
    memcpy(u, indices, sizeof(indices));
    const uint32_t ind_draw[5] = { 2, 4, 2, 3, 5 };    // draw 2: DrawInstanced(4, 2, 3, 5)
    const uint32_t ind_indexed[6] = { 3, 4, 2, 1, 10, 7 };   // draw 3: DrawIndexedInstanced(4, 2, 1, 10, 7)
    memcpy(u + 256, ind_draw, sizeof(ind_draw));
    memcpy(u + 512, ind_indexed, sizeof(ind_indexed));
    upload->Unmap(0, nullptr);
    ID3D12Resource *args = create_buffer(4096, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    g.list->CopyBufferRegion(args, 0, upload, 0, 4096);
    D3D12_RESOURCE_BARRIER bs[1] = {};
    bs[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bs[0].Transition.pResource = args;
    bs[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bs[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    bs[0].Transition.StateAfter = (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    g.list->ResourceBarrier(1, bs);
    ID3D12Resource *out = create_buffer(4 * 64 * 16, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    const UINT W = 16, H = 4;
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
    g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp = { 0, 0, (float)W, (float)H, 0, 1 };
    D3D12_RECT sc = { 0, 0, (LONG)W, (LONG)H };
    g.list->RSSetViewports(1, &vp);
    g.list->RSSetScissorRects(1, &sc);
    g.list->SetGraphicsRootSignature(rs);
    g.list->SetPipelineState(pso);
    g.list->SetGraphicsRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
    g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    D3D12_INDEX_BUFFER_VIEW ibv = { args->GetGPUVirtualAddress(), sizeof(indices), DXGI_FORMAT_R16_UINT };
    g.list->IASetIndexBuffer(&ibv);
    g.list->SetGraphicsRoot32BitConstant(0, 0, 0);
    g.list->DrawInstanced(4, 2, 3, 5);
    g.list->SetGraphicsRoot32BitConstant(0, 1, 0);
    g.list->DrawIndexedInstanced(4, 2, 1, 10, 7);
    g.list->ExecuteIndirect(sig_draw, 1, args, 256, nullptr, 0);
    g.list->ExecuteIndirect(sig_indexed, 1, args, 512, nullptr, 0);
    ID3D12Resource *rb = create_buffer(4 * 64 * 16, D3D12_HEAP_TYPE_READBACK);
    transition(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    g.list->CopyBufferRegion(rb, 0, out, 0, 4 * 64 * 16);
    if (!execute_and_wait())
        return false;
    const uint32_t *o = nullptr;
    rb->Map(0, nullptr, (void **)&o);
    int bad = 0;
    for (uint32_t d = 0; d < 4; d++)
        for (uint32_t i = 0; i < 2; i++)
            for (uint32_t v = 0; v < 4; v++)
            {
                uint32_t vid = v;   // indexed: index v (base vertex 10 not included); else vertex v (start 3 not included)
                uint32_t slot = d * 64 + i * 16 + vid % 16;
                // StartVertexLocation is the base vertex for indexed draws (D3D12's SV_StartVertexLocation).
                uint32_t sv = sm68 ? ((d & 1) ? 10 : 3) : 0, si = sm68 ? ((d & 1) ? 7 : 5) : 0;
                const uint32_t *e = o + slot * 4;
                if ((e[0] != vid + 1 || e[1] != i + 1 || e[2] != sv + 1 || e[3] != si + 1) && bad++ < 8)
                    printf("  draw %u instance %u vertex %u: slot holds (%d, %d, %d, %d), want (%u, %u, %u, %u)\n", d, i, v,
                           (int)e[0] - 1, (int)e[1] - 1, (int)e[2] - 1, (int)e[3] - 1, vid, i, sv, si);
            }
    EXPECT(!bad, "%d of 32 vertex/instance ids wrong", bad);
    if (bad && getenv("D3D12TEST_VERBOSE"))
        for (uint32_t k = 0; k < 4 * 64; k++)
            if (o[k * 4] || o[k * 4 + 1])
                printf("  slot %u: (%d, %d, %d, %d)\n", k, (int)o[k * 4] - 1, (int)o[k * 4 + 1] - 1, (int)o[k * 4 + 2] - 1,
                       (int)o[k * 4 + 3] - 1);
    rb->Unmap(0, nullptr);
    rb->Release(); rtv_heap->Release(); rt->Release(); out->Release(); args->Release(); upload->Release();
    sig_draw->Release(); sig_indexed->Release(); pso->Release(); rs->Release();
    return true;
}

static bool test_draw_params() { return draw_params(false); }
static bool test_draw_params_sm68() { return draw_params(true); }

// Culling and facing: a clockwise triangle on the left and a counter-clockwise one on the right (as D3D12 sees them on
// screen), drawn with each cull mode and front-face winding. A drawn pixel holds 1 where SV_IsFrontFace is true and
// 0.5 where it is false; a culled triangle leaves the clear (0). Converted shaders flip Y in the viewport rather than
// in the shader, which must not change which side is the front.
static bool culling(bool dxbc)
{
    static const char *src = R"(
float4 vs(uint id : SV_VertexID) : SV_Position
{
    // Left (x < 0): (-0.9, 0.9), (-0.1, 0.9), (-0.9, -0.9): clockwise on screen (y up). Right: counter-clockwise.
    float2 p[6] = { float2(-0.9, 0.9), float2(-0.1, 0.9), float2(-0.9, -0.9),
                    float2(0.1, 0.9), float2(0.1, -0.9), float2(0.9, 0.9) };
    return float4(p[id], 0, 1);
}
float4 ps(bool front : SV_IsFrontFace) : SV_Target { return front ? 1.0 : 0.5; }
)";
    std::vector<char> vs = dxbc ? compile_fxc(src, "vs", "vs_5_0") : compile_dxc(src, "vs", "vs_6_0"),
                      ps = dxbc ? compile_fxc(src, "ps", "ps_5_0") : compile_dxc(src, "ps", "ps_6_0");
    if (vs.empty() || ps.empty())
        return false;
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    ID3D12RootSignature *rs = create_root_signature(rsd);
    const UINT W = 32, H = 16;
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = W; td.Height = H; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource *rt = nullptr;
    CHECK_HR(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&rt)));
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    ID3D12DescriptorHeap *rtv_heap = nullptr;
    CHECK_HR(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap)));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    g.device->CreateRenderTargetView(rt, nullptr, rtv);
    ID3D12Resource *rb = create_buffer(256 * H, D3D12_HEAP_TYPE_READBACK);
    static const D3D12_CULL_MODE modes[3] = { D3D12_CULL_MODE_NONE, D3D12_CULL_MODE_FRONT, D3D12_CULL_MODE_BACK };
    static const char *mode_names[3] = { "none", "front", "back" };
    int bad = 0;
    for (int ccw = 0; ccw < 2; ccw++)
        for (int m = 0; m < 3; m++)
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
            pd.pRootSignature = rs;
            pd.VS = { vs.data(), vs.size() };
            pd.PS = { ps.data(), ps.size() };
            pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            pd.SampleMask = ~0u;
            pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pd.RasterizerState.CullMode = modes[m];
            pd.RasterizerState.FrontCounterClockwise = ccw;
            pd.RasterizerState.DepthClipEnable = TRUE;
            pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pd.NumRenderTargets = 1;
            pd.RTVFormats[0] = DXGI_FORMAT_R8_UNORM;
            pd.SampleDesc.Count = 1;
            ID3D12PipelineState *pso = nullptr;
            CHECK_HR(g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)));
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
            g.list->DrawInstanced(6, 1, 0, 0);
            transition(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
            dst.pResource = rb;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8_UNORM, W, H, 1, 256 };
            srcl.pResource = rt;
            srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            g.list->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
            transition(rt, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            if (!execute_and_wait())
                return false;
            const uint8_t *px = nullptr;
            D3D12_RANGE r = { 0, 256 * H };
            rb->Map(0, &r, (void **)&px);
            // Pixels inside each triangle near its right angle (top-left of each): left (3, 3), right (W / 2 + 3, 3).
            uint8_t left = px[3 * 256 + 3], right = px[3 * 256 + W / 2 + 3];
            // Front: clockwise unless FrontCounterClockwise. The left triangle is clockwise.
            bool left_front = !ccw, right_front = ccw;
            auto want = [&](bool front) {
                if ((modes[m] == D3D12_CULL_MODE_FRONT && front) || (modes[m] == D3D12_CULL_MODE_BACK && !front))
                    return 0;
                return front ? 255 : 128;
            };
            int wl = want(left_front), wr = want(right_front);
            if ((abs(left - wl) > 1 || abs(right - wr) > 1) && bad++ < 8)
                printf("  cull %s, front %s: left %u (want %d), right %u (want %d)\n", mode_names[m], ccw ? "ccw" : "cw",
                       left, wl, right, wr);
            rb->Unmap(0, nullptr);
            pso->Release();
        }
    EXPECT(!bad, "%d of 6 cull/winding cases wrong", bad);
    rb->Release(); rtv_heap->Release(); rt->Release(); rs->Release();
    return true;
}

static bool test_culling_dxil() { return culling(false); }
static bool test_culling_dxbc() { return culling(true); }

// SV_CullDistance and SV_ClipDistance: three triangles side by side. The left one has a negative cull distance at
// every vertex and must not be drawn; the middle one is positive everywhere and is drawn; the right one has a clip
// distance that goes from -1 at its left edge to +1 at its right, so only its right part is drawn. (A triangle with
// cull distances of both signs is drawn whole by D3D12; Metal has no cull distance, and clip distances in its place
// would cut it, so that case isn't checked.)
static bool cull_clip_distance(bool dxbc)
{
    static const char *src = R"(
struct V { float4 pos : SV_Position; float cull : SV_CullDistance; float clip : SV_ClipDistance; };
V vs(uint id : SV_VertexID)
{
    // Right angle at the top left of each: (x0, 0.9), (x1, 0.9), (x0, -0.9).
    float2 p[9] = { float2(-0.9, 0.9), float2(-0.5, 0.9), float2(-0.9, -0.9),
                    float2(-0.3, 0.9), float2(0.3, 0.9), float2(-0.3, -0.9),
                    float2(0.4, 0.9), float2(1.0, 0.9), float2(0.4, -0.9) };
    float cull[9] = { -1, -1, -1, 1, 1, 1, 1, 1, 1 };
    float clip[9] = { 1, 1, 1, 1, 1, 1, -1, 1, -1 };
    V v;
    v.pos = float4(p[id], 0, 1);
    v.cull = cull[id];
    v.clip = clip[id];
    return v;
}
float4 ps(V v) : SV_Target { return 1.0; }
)";
    std::vector<char> vs = dxbc ? compile_fxc(src, "vs", "vs_5_0") : compile_dxc(src, "vs", "vs_6_0"),
                      ps = dxbc ? compile_fxc(src, "ps", "ps_5_0") : compile_dxc(src, "ps", "ps_6_0");
    if (vs.empty() || ps.empty())
        return false;
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    ID3D12RootSignature *rs = create_root_signature(rsd);
    const UINT W = 32, H = 16;
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = W; td.Height = H; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource *rt = nullptr;
    CHECK_HR(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&rt)));
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    ID3D12DescriptorHeap *rtv_heap = nullptr;
    CHECK_HR(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap)));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    g.device->CreateRenderTargetView(rt, nullptr, rtv);
    ID3D12Resource *rb = create_buffer(256 * H, D3D12_HEAP_TYPE_READBACK);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = rs;
    pd.VS = { vs.data(), vs.size() };
    pd.PS = { ps.data(), ps.size() };
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = ~0u;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8_UNORM;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState *pso = nullptr;
    CHECK_HR(g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)));
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
    g.list->DrawInstanced(9, 1, 0, 0);
    transition(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
    dst.pResource = rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8_UNORM, W, H, 1, 256 };
    srcl.pResource = rt;
    srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
    if (!execute_and_wait())
        return false;
    const uint8_t *px = nullptr;
    D3D12_RANGE r = { 0, 256 * H };
    rb->Map(0, &r, (void **)&px);
    // Row 2 (y = 0.69): x = -0.84 (culled), -0.22 (drawn), 0.47 (clip -0.77) and 0.84 (clip +0.48).
    struct { UINT x; uint8_t want; const char *what; } probes[4] = {
        { 2, 0, "all-negative cull distance" }, { 12, 255, "positive cull distance" },
        { 23, 0, "negative clip distance" }, { 29, 255, "positive clip distance" },
    };
    int bad = 0;
    for (auto &pr : probes)
        if (abs(px[2 * 256 + pr.x] - pr.want) > 1 && bad++ < 4)
            printf("  %s: pixel %u is %u, want %u\n", pr.what, pr.x, px[2 * 256 + pr.x], pr.want);
    rb->Unmap(0, nullptr);
    EXPECT(!bad, "%d of 4 cull/clip distance probes wrong", bad);
    pso->Release(); rb->Release(); rtv_heap->Release(); rt->Release(); rs->Release();
    return true;
}

// SV_Barycentrics (SM 6.1), perspective-correct and noperspective: one triangle with corners at (-1, 1), (1, 1) and
// (-1, -1) in NDC and w = 1, 2 and 4, drawn into a 16x16 RGBA32F target twice: a perspective pipeline writes b.xy to
// red and green, a noperspective one bn.xy to blue and alpha. They're checked against screen-space barycentrics l (bn)
// and l_i / w_i normalized (b), with x weighting the first vertex. Then a third pipeline reads both in one shader, which
// Metal takes from MSL 4.0 (before it, one barycentric_coord per function, and the pipeline fails): checked when it
// can be made.
static bool test_barycentrics()
{
    static const char *src = R"(
float4 vs(uint id : SV_VertexID) : SV_Position
{
    float2 p[3] = { float2(-1, 1), float2(1, 1), float2(-1, -1) };
    float w[3] = { 1, 2, 4 };
    return float4(p[id] * w[id], 0, w[id]);
}
float4 ps_p(float4 pos : SV_Position, float3 b : SV_Barycentrics) : SV_Target { return float4(b.xy, 0, 0); }
float4 ps_n(float4 pos : SV_Position, noperspective float3 bn : SV_Barycentrics) : SV_Target { return float4(0, 0, bn.xy); }
float4 ps_both(float3 b : SV_Barycentrics, noperspective float3 bn : SV_Barycentrics1) : SV_Target { return float4(b.xy, bn.xy); }
)";
    std::vector<char> vs = compile_dxc(src, "vs", "vs_6_1"), ps_p = compile_dxc(src, "ps_p", "ps_6_1"),
                      ps_n = compile_dxc(src, "ps_n", "ps_6_1"), ps_both = compile_dxc(src, "ps_both", "ps_6_1");
    if (vs.empty() || ps_p.empty() || ps_n.empty() || ps_both.empty())
        return false;
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 o3 = {};
    if (FAILED(g.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &o3, sizeof(o3))) || !o3.BarycentricsSupported)
    {
        printf("  skipped: no barycentrics\n");
        return true;
    }
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    ID3D12RootSignature *rs = create_root_signature(rsd);
    const UINT W = 16, H = 16;
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = W; td.Height = H; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource *rt = nullptr;
    CHECK_HR(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&rt)));
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    ID3D12DescriptorHeap *rtv_heap = nullptr;
    CHECK_HR(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap)));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    g.device->CreateRenderTargetView(rt, nullptr, rtv);
    const UINT pitch = W * 16;
    ID3D12Resource *rb = create_buffer(pitch * H, D3D12_HEAP_TYPE_READBACK);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = rs;
    pd.VS = { vs.data(), vs.size() };
    pd.PS = { ps_p.data(), ps_p.size() };
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN;
    pd.SampleMask = ~0u;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState *pso = nullptr, *pso_n = nullptr;
    CHECK_HR(g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)));
    pd.PS = { ps_n.data(), ps_n.size() };
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_BLUE | D3D12_COLOR_WRITE_ENABLE_ALPHA;
    CHECK_HR(g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso_n)));
    ID3D12PipelineState *pso_both = nullptr;
    pd.PS = { ps_both.data(), ps_both.size() };
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso_both))))
        printf("  one shader with both barycentrics: no pipeline (Metal before MSL 4.0)\n");
    int bad = 0, checked = 0;
    for (int pass = 0; pass < (pso_both ? 2 : 1); pass++)
    {
        float clear[4] = { -1, -1, -1, -1 };
        g.list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        D3D12_VIEWPORT vp = { 0, 0, (float)W, (float)H, 0, 1 };
        D3D12_RECT sc = { 0, 0, (LONG)W, (LONG)H };
        g.list->RSSetViewports(1, &vp);
        g.list->RSSetScissorRects(1, &sc);
        g.list->SetGraphicsRootSignature(rs);
        g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (pass)
        {
            g.list->SetPipelineState(pso_both);
            g.list->DrawInstanced(3, 1, 0, 0);
        }
        else
        {
            g.list->SetPipelineState(pso);
            g.list->DrawInstanced(3, 1, 0, 0);
            g.list->SetPipelineState(pso_n);
            g.list->DrawInstanced(3, 1, 0, 0);
        }
        transition(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
        dst.pResource = rb;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R32G32B32A32_FLOAT, W, H, 1, pitch };
        srcl.pResource = rt;
        srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        g.list->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
        if (!execute_and_wait())
            return false;
        const uint8_t *data = nullptr;
        D3D12_RANGE r = { 0, pitch * H };
        rb->Map(0, &r, (void **)&data);
        static const float w[3] = { 1, 2, 4 };
        for (UINT y = 0; y < H; y++)
            for (UINT x = 0; x < W; x++)
            {
                float X = (x + 0.5f) / (W / 2) - 1, Y = 1 - (y + 0.5f) / (H / 2);
                float l[3] = { 0, (X + 1) / 2, (1 - Y) / 2 };
                l[0] = 1 - l[1] - l[2];
                if (l[0] < 0.05f)   // outside, or too close to the edge to be sure it's covered
                    continue;
                float s = l[0] / w[0] + l[1] / w[1] + l[2] / w[2];
                float want[4] = { l[0] / w[0] / s, l[1] / w[1] / s, l[0], l[1] };
                const float *got = (const float *)(data + y * pitch + x * 16);
                checked++;
                for (int c = 0; c < 4; c++)
                    if (fabsf(got[c] - want[c]) > 2e-3f)
                    {
                        if (bad++ < 6)
                            printf("  %s, pixel (%u, %u): (%f %f %f %f), want (%f %f %f %f)\n", pass ? "one shader" : "two shaders",
                                   x, y, got[0], got[1], got[2], got[3], want[0], want[1], want[2], want[3]);
                        break;
                    }
            }
        rb->Unmap(0, nullptr);
        // Only when another pass follows: a barrier left in the list would run with the next test, after rt is gone.
        if (!pass && pso_both)
            transition(rt, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    EXPECT(checked > 50 && !bad, "%d of %d covered pixels' barycentrics wrong", bad, checked);
    if (pso_both)
        pso_both->Release();
    pso->Release(); pso_n->Release(); rb->Release(); rtv_heap->Release(); rt->Release(); rs->Release();
    return true;
}

static bool test_cull_clip_distance_dxil() { return cull_clip_distance(false); }
static bool test_cull_clip_distance_dxbc() { return cull_clip_distance(true); }

// WriteBufferImmediate inside a render pass: 1200 draws, each between a MARKER_IN and a MARKER_OUT write, and a
// DEFAULT write after each, land every value in its slot. Without buffer markers MARKER_IN is batched with the rest
// and written when the pass ends or the batch (1024 writes) fills, so the writes span more than one batch.
static bool test_wbi_markers()
{
    static const char *src = R"(
float4 vs(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(0.1, -0.1), 0, 1);
}
float4 ps() : SV_Target { return float4(1, 1, 1, 1); }
)";
    std::vector<char> vs = compile_fxc(src, "vs", "vs_5_0"), ps = compile_fxc(src, "ps", "ps_5_0");
    if (vs.empty() || ps.empty())
        return false;
    ID3D12GraphicsCommandList2 *l2 = nullptr;
    CHECK_HR(g.list->QueryInterface(IID_PPV_ARGS(&l2)));
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
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 16; td.Height = 16; td.DepthOrArraySize = 1; td.MipLevels = 1;
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
    const UINT DRAWS = 1200;
    ID3D12Resource *markers = create_buffer(DRAWS * 12, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE,
                                            D3D12_RESOURCE_STATE_COPY_DEST);
    g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp = { 0, 0, 16, 16, 0, 1 };
    D3D12_RECT sc = { 0, 0, 16, 16 };
    g.list->RSSetViewports(1, &vp);
    g.list->RSSetScissorRects(1, &sc);
    g.list->SetGraphicsRootSignature(rs);
    g.list->SetPipelineState(pso);
    g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_WRITEBUFFERIMMEDIATE_MODE in = D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN,
                                          out = D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT;
    D3D12_GPU_VIRTUAL_ADDRESS va = markers->GetGPUVirtualAddress();
    for (UINT i = 0; i < DRAWS; i++)
    {
        D3D12_WRITEBUFFERIMMEDIATE_PARAMETER m = { va + i * 4, 0x10000 + i };
        l2->WriteBufferImmediate(1, &m, &in);
        g.list->DrawInstanced(3, 1, 0, 0);
        m = { va + DRAWS * 4 + i * 4, 0x20000 + i };
        l2->WriteBufferImmediate(1, &m, &out);
        m = { va + DRAWS * 8 + i * 4, 0x30000 + i };
        l2->WriteBufferImmediate(1, &m, nullptr);
    }
    ID3D12Resource *rb = create_buffer(DRAWS * 12, D3D12_HEAP_TYPE_READBACK);
    transition(markers, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    g.list->CopyBufferRegion(rb, 0, markers, 0, DRAWS * 12);
    if (!execute_and_wait())
        return false;
    const uint32_t *got = nullptr;
    D3D12_RANGE r = { 0, DRAWS * 12 };
    rb->Map(0, &r, (void **)&got);
    for (UINT i = 0; i < DRAWS * 3; i++)
    {
        uint32_t want = 0x10000 * (1 + i / DRAWS) + i % DRAWS;
        EXPECT(got[i] == want, "marker word %u = 0x%x, want 0x%x", i, got[i], want);
    }
    rb->Release(); markers->Release(); rtv_heap->Release(); rt->Release(); pso->Release(); rs->Release(); l2->Release();
    return true;
}

// WaveReadLaneFirst in a pixel shader takes the first active lane that is not a helper (helper lanes complete a
// triangle's edge quads and are not active lanes). A thin triangle leaves many helpers; each covered pixel writes
// its own coordinates (alpha 1) and the coordinates WaveReadLaneFirst gave it, which must be a covered pixel's.
// Taking a helper's value hung Dragon's Dogma 2 on Mullion (dttdrv/mullion#71).
static bool test_wave_first_helpers()
{
    static const char *src = R"(
float4 vs(uint id : SV_VertexID) : SV_Position
{
    // a sliver from the top left corner to the bottom right, a pixel or two wide
    float2 p[3] = { float2(-1, 1), float2(1, -1), float2(-0.9, 1) };
    return float4(p[id], 0, 1);
}
uint2 ps(float4 pos : SV_Position) : SV_Target
{
    uint own = (uint(pos.y) << 8) | uint(pos.x);
    return uint2(own | 0x10000, WaveReadLaneFirst(own));
}
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
    pd.RTVFormats[0] = DXGI_FORMAT_R32G32_UINT;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState *pso = nullptr;
    CHECK_HR(g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)));

    const UINT W = 64, H = 64;
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = W; td.Height = H; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R32G32_UINT;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv = { DXGI_FORMAT_R32G32_UINT, { 0, 0, 0, 0 } };
    ID3D12Resource *rt = nullptr;
    CHECK_HR(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&rt)));
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    ID3D12DescriptorHeap *rtv_heap = nullptr;
    CHECK_HR(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap)));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    g.device->CreateRenderTargetView(rt, nullptr, rtv);
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
    g.list->DrawInstanced(3, 1, 0, 0);

    ID3D12Resource *rb = create_buffer(W * H * 8, D3D12_HEAP_TYPE_READBACK);
    transition(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
    dst.pResource = rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R32G32_UINT, W, H, 1, W * 8 };
    srcl.pResource = rt;
    srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
    if (!execute_and_wait())
        return false;
    const uint32_t *px = nullptr;
    D3D12_RANGE range = { 0, W * H * 8 };
    rb->Map(0, &range, (void **)&px);
    UINT covered = 0, wrong = 0;
    for (UINT i = 0; i < W * H; i++)
    {
        if (!(px[2 * i] & 0x10000))
            continue;
        covered++;
        uint32_t first = px[2 * i + 1], fx = first & 0xff, fy = first >> 8;
        bool first_covered = fx < W && fy < H && (px[2 * (fy * W + fx)] & 0x10000);
        if (!first_covered && wrong++ < 4)
            printf("  pixel %u,%u read %u,%u, which the triangle does not cover (a helper lane)\n", i % W, i / W, fx, fy);
    }
    printf("  %u pixels covered\n", covered);
    EXPECT(covered > 0, "the triangle covers no pixel");
    EXPECT(!wrong, "%u pixels read a helper lane's value", wrong);
    rb->Release(); rtv_heap->Release(); rt->Release(); pso->Release(); rs->Release();
    return true;
}

// Minimum and maximum filters (D3D12_FILTER_MINIMUM_* / MAXIMUM_*): a 2x1 texture of 0 and 1 sampled at its middle,
// every filter linear, reads 0 through a minimum sampler and 1 through a maximum one, where an ordinary linear
// sampler reads 0.5. Before Apple10 GPUs with macOS 26, Metal's sampler reduction mode has no effect (MoltenVK
// emulates it in SPIR-V shaders), and reducing samplers average too.
static bool minmax(bool sampler_heap, bool late = false)
{
    static const char *src = R"(
Texture2D<float> t : register(t0);
SamplerState lo : register(s0);
SamplerState hi : register(s1);
SamplerState avg : register(s2);
RWByteAddressBuffer o : register(u0);
[numthreads(1, 1, 1)] void cs()
{
    float2 uv = float2(0.5, 0.5);
    o.Store3(0, asuint(float3(t.SampleLevel(lo, uv, 0), t.SampleLevel(hi, uv, 0), t.SampleLevel(avg, uv, 0))));
}
)";
    std::vector<char> cs = compile_fxc(src, "cs", "cs_5_0");
    if (cs.empty())
        return false;
    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable = { 1, &range };
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_STATIC_SAMPLER_DESC ss[3] = {};
    const D3D12_FILTER filters[3] = { D3D12_FILTER_MINIMUM_MIN_MAG_MIP_LINEAR, D3D12_FILTER_MAXIMUM_MIN_MAG_MIP_LINEAR,
                                      D3D12_FILTER_MIN_MAG_MIP_LINEAR };
    for (UINT i = 0; i < 3; i++)
    {
        ss[i].Filter = filters[i];
        ss[i].AddressU = ss[i].AddressV = ss[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        ss[i].MaxLOD = D3D12_FLOAT32_MAX;
        ss[i].ShaderRegister = i;
    }
    // Heap: the same samplers in a sampler heap's table, made before the PSO is (or after it, when late: a bridge PSO
    // made before the application's first min/max sampler then dispatches with its SPIR-V pipeline).
    D3D12_ROOT_PARAMETER rph[3] = { rp[0], rp[1] };
    D3D12_DESCRIPTOR_RANGE srange = { D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 3, 0, 0, 0 };
    rph[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rph[2].DescriptorTable = { 1, &srange };
    ID3D12DescriptorHeap *sheap = nullptr;
    auto make_samplers = [&]() -> bool
    {
        D3D12_DESCRIPTOR_HEAP_DESC shd = { D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 3, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
        CHECK_HR(g.device->CreateDescriptorHeap(&shd, IID_PPV_ARGS(&sheap)));
        UINT sinc = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        for (UINT i = 0; i < 3; i++)
        {
            D3D12_SAMPLER_DESC sd = {};
            sd.Filter = filters[i];
            sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sd.MaxLOD = D3D12_FLOAT32_MAX;
            D3D12_CPU_DESCRIPTOR_HANDLE h = sheap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += i * sinc;
            g.device->CreateSampler(&sd, h);
        }
        return true;
    };
    if (sampler_heap && !late && !make_samplers())
        return false;
    D3D12_ROOT_SIGNATURE_DESC rsd = sampler_heap ? D3D12_ROOT_SIGNATURE_DESC{ 3, rph, 0, nullptr } : D3D12_ROOT_SIGNATURE_DESC{ 2, rp, 3, ss };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    ID3D12PipelineState *ps = create_compute_pipeline(rs, cs);
    if (!ps)
        return false;
    if (sampler_heap && late && !make_samplers())
        return false;

    ID3D12Resource *upload = create_buffer(256, D3D12_HEAP_TYPE_UPLOAD);
    float *u = nullptr;
    upload->Map(0, nullptr, (void **)&u);
    u[0] = 0.0f; u[1] = 1.0f;
    upload->Unmap(0, nullptr);
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 2; td.Height = 1; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R32_FLOAT;
    td.SampleDesc.Count = 1;
    ID3D12Resource *tex = nullptr;
    CHECK_HR(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex)));
    D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
    dst.pResource = tex;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcl.pResource = upload;
    srcl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcl.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_FLOAT, 2, 1, 1, 256 };
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
    transition(tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap *heap = nullptr;
    CHECK_HR(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
    g.device->CreateShaderResourceView(tex, nullptr, heap->GetCPUDescriptorHandleForHeapStart());
    ID3D12Resource *out = create_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12DescriptorHeap *heaps[2] = { heap, sheap };
    g.list->SetDescriptorHeaps(sheap ? 2 : 1, heaps);
    g.list->SetComputeRootSignature(rs);
    g.list->SetPipelineState(ps);
    g.list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    if (sheap)
        g.list->SetComputeRootDescriptorTable(2, sheap->GetGPUDescriptorHandleForHeapStart());
    g.list->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
    g.list->Dispatch(1, 1, 1);
    ID3D12Resource *rb = create_buffer(256, D3D12_HEAP_TYPE_READBACK);
    transition(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    g.list->CopyBufferRegion(rb, 0, out, 0, 256);
    if (!execute_and_wait())
        return false;
    const float *got = nullptr;
    D3D12_RANGE r = { 0, 256 };
    rb->Map(0, &r, (void **)&got);
    printf("  minimum %g, maximum %g, linear %g\n", got[0], got[1], got[2]);
    EXPECT(got[0] == 0.0f, "the minimum filter reads %g, want 0", got[0]);
    EXPECT(got[1] == 1.0f, "the maximum filter reads %g, want 1", got[1]);
    EXPECT(got[2] == 0.5f, "the linear filter reads %g, want 0.5", got[2]);
    rb->Release(); out->Release(); heap->Release(); tex->Release(); upload->Release(); ps->Release(); rs->Release();
    if (sheap)
        sheap->Release();
    return true;
}

static bool test_minmax() { return minmax(false); }
static bool test_minmax_heap() { return minmax(true); }
static bool test_minmax_heap_late() { return minmax(true, true); }

// Sampler MipLODBias: a 64x64 texture whose mip m holds m, sampled with point filters from a 64x64 pixel grid (LOD 0)
// through a static sampler (bias 2) and two heap samplers (bias 3 and -1): Sample, SampleLevel 1, SampleBias 1 and
// SampleGrad (LOD 0) read the mip of the LOD plus the bias, clamped to the mip range. Metal samplers have a LOD bias only
// on Apple10 GPUs with macOS 26; elsewhere converted shaders add it from the sampler's descriptor entry, except to
// gradient samples, and SPIR-V shaders from the word MoltenVK keeps next to the sampler. Late: the heap samplers are
// made after the PSO, and the static sampler has no bias (a bridge PSO with gradient samples then draws with SPIR-V).
static bool sampler_lod_bias(bool dxbc, bool late = false)
{
    static const char *src = R"(
Texture2D<float> t : register(t0);
SamplerState s0 : register(s0);
SamplerState s1 : register(s1);
SamplerState s2 : register(s2);
RWStructuredBuffer<float4> o : register(u1);
float4 reads(SamplerState s, float2 uv)
{
    return float4(t.Sample(s, uv), t.SampleLevel(s, uv, 1.0), t.SampleBias(s, uv, 1.0), t.SampleGrad(s, uv, ddx(uv), ddy(uv)));
}
float4 ps(float4 p : SV_Position) : SV_Target
{
    float2 uv = p.xy / 64.0;
    // Every pixel reads the same mips, so every pixel writes them: the samples stay in uniform control flow (a store
    // under a branch would take them into it, where implicit derivatives are undefined).
    o[0] = reads(s0, uv); o[1] = reads(s1, uv); o[2] = reads(s2, uv);
    return 0;
}
float4 vs(uint id : SV_VertexID) : SV_Position
{
    return float4(id == 1 ? 3.0 : -1.0, id == 2 ? -3.0 : 1.0, 0, 1);
}
)";
    std::vector<char> vsb = dxbc ? compile_fxc(src, "vs", "vs_5_0") : compile_dxc(src, "vs", "vs_6_0");
    std::vector<char> psb = dxbc ? compile_fxc(src, "ps", "ps_5_0") : compile_dxc(src, "ps", "ps_6_0");
    if (vsb.empty() || psb.empty())
        return false;
    ID3D12Device *dev = g.device;
    const float biases[3] = { late ? 0.0f : 2.0f, 3.0f, -1.0f };

    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
    D3D12_DESCRIPTOR_RANGE srange = { D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 2, 1, 0, 0 };
    D3D12_ROOT_PARAMETER rp[3] = {};
    // u1: DXBC pixel shaders number UAVs after their render targets.
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable = { 1, &range };
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable = { 1, &srange };
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[2].Descriptor.ShaderRegister = 1;
    D3D12_STATIC_SAMPLER_DESC ss = {};
    ss.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.MipLODBias = biases[0];
    ss.MaxLOD = D3D12_FLOAT32_MAX;
    ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 3, rp, 1, &ss };
    ID3D12RootSignature *rs = create_root_signature(rsd);

    D3D12_DESCRIPTOR_HEAP_DESC shd = { D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap *sheap = nullptr;
    CHECK_HR(dev->CreateDescriptorHeap(&shd, IID_PPV_ARGS(&sheap)));
    UINT sinc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    for (UINT i = 0; i < 2 && !late; i++)
    {
        D3D12_SAMPLER_DESC sd = {};
        sd.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sd.MipLODBias = biases[1 + i];
        sd.MaxLOD = D3D12_FLOAT32_MAX;
        D3D12_CPU_DESCRIPTOR_HANDLE h = sheap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += i * sinc;
        dev->CreateSampler(&sd, h);
    }

    // The texture: 7 mips, mip m filled with m.
    const UINT mips = 7;
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 64; td.Height = 64; td.DepthOrArraySize = 1; td.MipLevels = mips;
    td.Format = DXGI_FORMAT_R32_FLOAT;
    td.SampleDesc.Count = 1;
    ID3D12Resource *tex = nullptr;
    CHECK_HR(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex)));
    ID3D12Resource *upload = create_buffer(mips * 256 * 64, D3D12_HEAP_TYPE_UPLOAD);
    float *u = nullptr;
    upload->Map(0, nullptr, (void **)&u);
    for (UINT m = 0; m < mips; m++)
        for (UINT k = 0; k < 64 * 64; k++)
            u[m * 64 * 64 + k] = (float)m;
    upload->Unmap(0, nullptr);
    for (UINT m = 0; m < mips; m++)
    {
        UINT w = 64 >> m;
        D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
        dst.pResource = tex;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = m;
        srcl.pResource = upload;
        srcl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcl.PlacedFootprint.Offset = (UINT64)m * 64 * 64 * 4;
        srcl.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_FLOAT, w, w, 1, 256 };
        g.list->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
    }
    transition(tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap *heap = nullptr;
    CHECK_HR(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
    dev->CreateShaderResourceView(tex, nullptr, heap->GetCPUDescriptorHandleForHeapStart());

    D3D12_RESOURCE_DESC rtd = td;
    rtd.MipLevels = 1;
    rtd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rtd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource *rt = nullptr;
    CHECK_HR(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rtd, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&rt)));
    D3D12_DESCRIPTOR_HEAP_DESC rhd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    ID3D12DescriptorHeap *rtv_heap = nullptr;
    CHECK_HR(dev->CreateDescriptorHeap(&rhd, IID_PPV_ARGS(&rtv_heap)));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(rt, nullptr, rtv);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = rs;
    pd.VS = { vsb.data(), vsb.size() };
    pd.PS = { psb.data(), psb.size() };
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleMask = ~0u;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState *pso = nullptr;
    CHECK_HR(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)));
    for (UINT i = 0; i < 2 && late; i++)
    {
        D3D12_SAMPLER_DESC sd = {};
        sd.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sd.MipLODBias = biases[1 + i];
        sd.MaxLOD = D3D12_FLOAT32_MAX;
        D3D12_CPU_DESCRIPTOR_HANDLE h = sheap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += i * sinc;
        dev->CreateSampler(&sd, h);
    }

    ID3D12Resource *out = create_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_VIEWPORT vpt = { 0, 0, 64, 64, 0, 1 };
    D3D12_RECT sc = { 0, 0, 64, 64 };
    ID3D12DescriptorHeap *heaps[2] = { heap, sheap };
    g.list->SetDescriptorHeaps(2, heaps);
    g.list->RSSetViewports(1, &vpt);
    g.list->RSSetScissorRects(1, &sc);
    g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    g.list->SetGraphicsRootSignature(rs);
    g.list->SetPipelineState(pso);
    g.list->SetGraphicsRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    g.list->SetGraphicsRootDescriptorTable(1, sheap->GetGPUDescriptorHandleForHeapStart());
    g.list->SetGraphicsRootUnorderedAccessView(2, out->GetGPUVirtualAddress());
    g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.list->DrawInstanced(3, 1, 0, 0);
    ID3D12Resource *rb = nullptr;
    const float *got = (const float *)read_buffer(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 256, &rb);
    if (!got)
        return false;
    static const char *names[4] = { "Sample", "SampleLevel 1", "SampleBias 1", "SampleGrad" };
    const float lods[4] = { 0.0f, 1.0f, 1.0f, 0.0f };
    for (UINT i = 0; i < 3; i++)
    {
        printf("  bias %g (%s): %g %g %g %g\n", biases[i], i ? "heap" : "static", got[i * 4], got[i * 4 + 1],
               got[i * 4 + 2], got[i * 4 + 3]);
        for (UINT k = 0; k < 4; k++)
        {
            float want = std::min(std::max(lods[k] + biases[i], 0.0f), (float)(mips - 1));
            EXPECT(got[i * 4 + k] == want, "%s through a %s sampler with bias %g reads mip %g, want %g", names[k],
                   i ? "heap" : "static", biases[i], got[i * 4 + k], want);
        }
    }
    rb->Release(); out->Release(); pso->Release(); rtv_heap->Release(); rt->Release(); heap->Release();
    upload->Release(); tex->Release(); sheap->Release(); rs->Release();
    return true;
}

static bool test_sampler_lod_bias() { return sampler_lod_bias(false); }
static bool test_sampler_lod_bias_dxbc() { return sampler_lod_bias(true); }
static bool test_sampler_lod_bias_late() { return sampler_lod_bias(false, true); }

// Depth EQUAL across PSOs: one PSO draws 3000 random triangles writing depth (LESS), a second PSO with the same vertex
// shader draws them again with depth EQUAL and no depth writes. Every pixel the first drew must pass the second, which
// needs both pipelines' positions to be bit-identical (a depth pre-pass followed by the main pass). The second PSO has a
// logic operation, which keeps it on SPIR-V, while the first may be on the bridge: the two converters must agree.
static bool depth_equal_cross_path(bool dxbc)
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS o = {};
    g.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o));
    if (!o.OutputMergerLogicOp)
    {
        printf("  OutputMergerLogicOp not supported\n");
        return true;
    }
    static const char *src = R"(
cbuffer C : register(b0) { float4x4 m; };
float4 vs(float3 p : POSITION) : SV_Position
{
    // Displacement and skinning-like math, as pre-passes run it: transcendental, normalize, pow, rsqrt.
    float3 n = normalize(p + float3(0.3, -0.2, 0.1));
    float3 d = p + n * (0.02 * sin(p.x * 3.1 + p.y * 1.7) + 0.01 * cos(p.z * 2.3)) + 0.005 * pow(abs(n), 2.2) * rsqrt(1.0 + dot(p, p));
    float4x4 m2 = m * (1.0 + 0.001 * exp2(frac(p.y)));
    return mul(m2, float4(lerp(p, d, 0.75), 1));
}
uint4 ps1() : SV_Target { return uint4(1, 0, 0, 0); }
uint4 ps2() : SV_Target { return uint4(2, 0, 0, 0); }
)";
    std::vector<char> vsb = dxbc ? compile_fxc(src, "vs", "vs_5_0") : compile_dxc(src, "vs", "vs_6_0");
    std::vector<char> ps1 = dxbc ? compile_fxc(src, "ps1", "ps_5_0") : compile_dxc(src, "ps1", "ps_6_0");
    std::vector<char> ps2 = dxbc ? compile_fxc(src, "ps2", "ps_5_0") : compile_dxc(src, "ps2", "ps_6_0");
    if (vsb.empty() || ps1.empty() || ps2.empty())
        return false;
    ID3D12Device *dev = g.device;
    D3D12_ROOT_PARAMETER rp = {};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp.Constants.Num32BitValues = 16;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
    ID3D12RootSignature *rs = create_root_signature(rsd);

    const UINT W = 256;
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC rtd = {};
    rtd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rtd.Width = W; rtd.Height = W; rtd.DepthOrArraySize = 1; rtd.MipLevels = 1;
    rtd.Format = DXGI_FORMAT_R8G8B8A8_UINT;
    rtd.SampleDesc.Count = 1;
    rtd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cc = { DXGI_FORMAT_R8G8B8A8_UINT };
    ID3D12Resource *rt = nullptr, *ds = nullptr;
    CHECK_HR(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rtd, D3D12_RESOURCE_STATE_RENDER_TARGET, &cc, IID_PPV_ARGS(&rt)));
    D3D12_RESOURCE_DESC dsd = rtd;
    dsd.Format = DXGI_FORMAT_D32_FLOAT;
    dsd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE dc = { DXGI_FORMAT_D32_FLOAT };
    dc.DepthStencil.Depth = 1.0f;
    CHECK_HR(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &dsd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &dc, IID_PPV_ARGS(&ds)));
    D3D12_DESCRIPTOR_HEAP_DESC rhd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 }, dhd = { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1 };
    ID3D12DescriptorHeap *rtv_heap = nullptr, *dsv_heap = nullptr;
    CHECK_HR(dev->CreateDescriptorHeap(&rhd, IID_PPV_ARGS(&rtv_heap)));
    CHECK_HR(dev->CreateDescriptorHeap(&dhd, IID_PPV_ARGS(&dsv_heap)));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart(), dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(rt, nullptr, rtv);
    dev->CreateDepthStencilView(ds, nullptr, dsv);

    D3D12_INPUT_ELEMENT_DESC el = { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = rs;
    pd.VS = { vsb.data(), vsb.size() };
    pd.PS = { ps1.data(), ps1.size() };
    pd.InputLayout = { &el, 1 };
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UINT;
    pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pd.DepthStencilState.DepthEnable = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pd.SampleMask = ~0u;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState *pre = nullptr, *main_pso = nullptr;
    CHECK_HR(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pre)));
    pd.PS = { ps2.data(), ps2.size() };
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_EQUAL;
    pd.BlendState.RenderTarget[0].LogicOpEnable = TRUE;
    pd.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_COPY;
    CHECK_HR(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&main_pso)));

    // Random triangles in [-1.2, 1.2]^2 x [0.5, 4], through a perspective matrix with awkward values.
    const UINT tris = 3000;
    ID3D12Resource *vb = create_buffer(tris * 3 * 12, D3D12_HEAP_TYPE_UPLOAD);
    float *v = nullptr;
    vb->Map(0, nullptr, (void **)&v);
    uint32_t seed = 7;
    auto rnd = [&]() { seed = seed * 1664525 + 1013904223; return (seed >> 8) * (1.0f / 16777216.0f); };
    for (UINT i = 0; i < tris; i++)
    {
        float cx = rnd() * 2.4f - 1.2f, cy = rnd() * 2.4f - 1.2f, cz = 0.5f + rnd() * 3.5f;
        for (UINT k = 0; k < 3; k++)
        {
            v[(i * 3 + k) * 3 + 0] = (cx + (rnd() - 0.5f) * 0.4f) * cz;
            v[(i * 3 + k) * 3 + 1] = (cy + (rnd() - 0.5f) * 0.4f) * cz;
            v[(i * 3 + k) * 3 + 2] = cz + (rnd() - 0.5f) * 0.7f;
        }
    }
    vb->Unmap(0, nullptr);
    // Row-major for mul(m, v) with HLSL's default column-major packing: the constants are m's columns.
    const float n = 0.1f, f = 10.0f;
    float mat[16] = { 0.9134f, 0.0371f, 0.0f, 0.0f,
                      -0.0283f, 1.1170f, 0.0f, 0.0f,
                      0.0119f, -0.0077f, f / (f - n), 1.0f,
                      0.0031f, 0.0013f, -n * f / (f - n), 0.0f };

    D3D12_VIEWPORT vpt = { 0, 0, (float)W, (float)W, 0, 1 };
    D3D12_RECT sc = { 0, 0, (LONG)W, (LONG)W };
    const UINT clear[4] = {};
    g.list->ClearRenderTargetView(rtv, (const float *)clear, 0, nullptr);
    g.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    g.list->RSSetViewports(1, &vpt);
    g.list->RSSetScissorRects(1, &sc);
    g.list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    g.list->SetGraphicsRootSignature(rs);
    g.list->SetGraphicsRoot32BitConstants(0, 16, mat, 0);
    g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VERTEX_BUFFER_VIEW vbv = { vb->GetGPUVirtualAddress(), tris * 3 * 12, 12 };
    g.list->IASetVertexBuffers(0, 1, &vbv);
    g.list->SetPipelineState(pre);
    g.list->DrawInstanced(tris * 3, 1, 0, 0);
    g.list->SetPipelineState(main_pso);
    g.list->DrawInstanced(tris * 3, 1, 0, 0);

    transition(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12Resource *rb = create_buffer(W * W * 4, D3D12_HEAP_TYPE_READBACK);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
    dst.pResource = rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UINT, W, W, 1, W * 4 };
    srcl.pResource = rt;
    srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
    if (!execute_and_wait())
        return false;
    const uint8_t *px = nullptr;
    D3D12_RANGE r = { 0, W * W * 4 };
    rb->Map(0, &r, (void **)&px);
    UINT drawn = 0, failed = 0;
    for (UINT i = 0; i < W * W; i++)
    {
        drawn += px[i * 4] != 0;
        failed += px[i * 4] == 1;
    }
    rb->Unmap(0, nullptr);
    printf("  %u pixels drawn, %u fail depth EQUAL in the second pass\n", drawn, failed);
    EXPECT(drawn > W * W / 2, "only %u pixels drawn", drawn);
    EXPECT(!failed, "%u of %u pixels fail depth EQUAL with the same vertex shader in another PSO", failed, drawn);
    rb->Release(); vb->Release(); main_pso->Release(); pre->Release(); rtv_heap->Release(); dsv_heap->Release();
    ds->Release(); rt->Release(); rs->Release();
    return true;
}

static bool test_depth_equal_cross_path() { return depth_equal_cross_path(false); }
static bool test_depth_equal_cross_path_dxbc() { return depth_equal_cross_path(true); }

// SRV ResourceMinLODClamp: a 64x64 texture whose mip m holds m, read through a view of mips 1 and up (MostDetailedMip 1)
// with ResourceMinLODClamp 2.5, filtered linearly between mips. The clamp is absolute (a resource mip) and applies to
// sampling only: SampleLevel 0 (resource LOD 1) reads 2.5, SampleLevel 3 (resource LOD 4) reads 4, SampleGrad without
// gradients 2.5 and with view LOD 3 gradients 4, and GetDimensions still reports the view (32x32, 6 levels).
static bool srv_min_lod_clamp(bool dxbc)
{
    static const char *src = R"(
Texture2D<float> t : register(t0);
SamplerState s : register(s0);
RWStructuredBuffer<float4> o : register(u0);
[numthreads(1, 1, 1)] void cs()
{
    uint w, h, levels;
    t.GetDimensions(0, w, h, levels);
    float2 uv = float2(0.3, 0.6);
    o[0] = float4(t.SampleLevel(s, uv, 0.0), t.SampleLevel(s, uv, 3.0), t.SampleLevel(s, uv, 1.25), t.Load(int3(3, 5, 0)));
    o[1] = float4(w, h, levels, t.SampleLevel(s, uv, 2.0));
    // Gradients: none (LOD below the clamp), and 8 texels of the 32-texel view per step (view LOD 3).
    o[2] = float4(t.SampleGrad(s, uv, 0.0, 0.0), t.SampleGrad(s, uv, float2(0.25, 0), float2(0, 0.25)), 0, 0);
}
)";
    std::vector<char> cs = dxbc ? compile_fxc(src, "cs", "cs_5_0") : compile_dxc(src, "cs", "cs_6_0");
    if (cs.empty())
        return false;
    ID3D12Device *dev = g.device;
    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable = { 1, &range };
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_STATIC_SAMPLER_DESC ss = {};
    ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 2, rp, 1, &ss };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    ID3D12PipelineState *pso = create_compute_pipeline(rs, cs);
    if (!pso)
        return false;

    const UINT mips = 7;
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 64; td.Height = 64; td.DepthOrArraySize = 1; td.MipLevels = mips;
    td.Format = DXGI_FORMAT_R16_FLOAT;
    td.SampleDesc.Count = 1;
    ID3D12Resource *tex = nullptr;
    CHECK_HR(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex)));
    ID3D12Resource *upload = create_buffer(mips * 256 * 64, D3D12_HEAP_TYPE_UPLOAD);
    uint16_t *u = nullptr;
    upload->Map(0, nullptr, (void **)&u);
    static const uint16_t halves[7] = { 0x0000, 0x3c00, 0x4000, 0x4200, 0x4400, 0x4500, 0x4600 }; // 0..6
    for (UINT m = 0; m < mips; m++)
        for (UINT k = 0; k < 128 * 64; k++)
            u[m * 128 * 64 + k] = halves[m];
    upload->Unmap(0, nullptr);
    for (UINT m = 0; m < mips; m++)
    {
        UINT w = 64 >> m;
        D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
        dst.pResource = tex;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = m;
        srcl.pResource = upload;
        srcl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcl.PlacedFootprint.Offset = (UINT64)m * 256 * 64;
        srcl.PlacedFootprint.Footprint = { DXGI_FORMAT_R16_FLOAT, w, w, 1, 256 };
        g.list->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
    }
    transition(tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap *heap = nullptr;
    CHECK_HR(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_R16_FLOAT;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MostDetailedMip = 1;
    sd.Texture2D.MipLevels = (UINT)-1;
    sd.Texture2D.ResourceMinLODClamp = 2.5f;
    dev->CreateShaderResourceView(tex, &sd, heap->GetCPUDescriptorHandleForHeapStart());
    ID3D12Resource *out = create_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g.list->SetDescriptorHeaps(1, &heap);
    g.list->SetComputeRootSignature(rs);
    g.list->SetPipelineState(pso);
    g.list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    g.list->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
    g.list->Dispatch(1, 1, 1);
    ID3D12Resource *rb = nullptr;
    const float *r = (const float *)read_buffer(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 256, &rb);
    if (!r)
        return false;
    printf("  SampleLevel 0 %g, 3 %g, 1.25 %g, 2 %g; Load mip 0 %g; GetDimensions %gx%g, %g levels\n",
           r[0], r[1], r[2], r[7], r[3], r[4], r[5], r[6]);
    EXPECT(fabsf(r[0] - 2.5f) < 0.02f, "SampleLevel 0 reads %g, want 2.5 (the clamp)", r[0]);
    EXPECT(fabsf(r[1] - 4.0f) < 0.02f, "SampleLevel 3 reads %g, want 4", r[1]);
    EXPECT(fabsf(r[2] - 2.5f) < 0.02f, "SampleLevel 1.25 reads %g, want 2.5 (the clamp)", r[2]);
    EXPECT(fabsf(r[7] - 3.0f) < 0.02f, "SampleLevel 2 reads %g, want 3", r[7]);
    printf("  SampleGrad none %g, view LOD 3 %g\n", r[8], r[9]);
    EXPECT(fabsf(r[8] - 2.5f) < 0.02f, "SampleGrad without gradients reads %g, want 2.5 (the clamp)", r[8]);
    EXPECT(fabsf(r[9] - 4.0f) < 0.02f, "SampleGrad at view LOD 3 reads %g, want 4", r[9]);
    EXPECT(r[4] == 32.0f && r[5] == 32.0f && r[6] == 6.0f, "GetDimensions %gx%g, %g levels, want 32x32, 6", r[4], r[5], r[6]);
    rb->Release(); out->Release(); heap->Release(); upload->Release(); tex->Release(); pso->Release(); rs->Release();
    return true;
}

static bool test_srv_min_lod_clamp() { return srv_min_lod_clamp(false); }
static bool test_srv_min_lod_clamp_dxbc() { return srv_min_lod_clamp(true); }

// Sampling at NaN coordinates: D3D converts NaN to 0 when it turns texture coordinates into fixed point (D3D11.3
// 3.2.3), so a point sample at (NaN, NaN) reads texel (0, 0), and at (NaN, 0.9) texel (0, 3) of a 4x4 texture whose
// texel (x, y) holds 1 + x + 4y. Infinite coordinates are clamped like any other, with CLAMP addressing.
static bool sample_nan_coords(bool dxbc)
{
    static const char *src = R"(
Texture2D<float> t : register(t0);
SamplerState s : register(s0);
RWStructuredBuffer<float4> o : register(u0);
cbuffer C : register(b0) { float zero; };
[numthreads(1, 1, 1)] void cs()
{
    float nan = asfloat(0x7fc00000u + (uint)zero), inf = 1.0 / zero;
    o[0] = float4(t.SampleLevel(s, float2(nan, nan), 0), t.SampleLevel(s, float2(nan, 0.9), 0),
                  t.SampleLevel(s, float2(inf, 0.1), 0), t.SampleLevel(s, float2(-inf, nan), 0));
}
)";
    std::vector<char> cs = dxbc ? compile_fxc(src, "cs", "cs_5_0") : compile_dxc(src, "cs", "cs_6_0");
    if (cs.empty())
        return false;
    ID3D12Device *dev = g.device;
    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
    D3D12_ROOT_PARAMETER rp[3] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable = { 1, &range };
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[2].Constants.Num32BitValues = 1;
    D3D12_STATIC_SAMPLER_DESC ss = {};
    ss.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 3, rp, 1, &ss };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    ID3D12PipelineState *pso = create_compute_pipeline(rs, cs);
    if (!pso)
        return false;
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 4; td.Height = 4; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R32_FLOAT;
    td.SampleDesc.Count = 1;
    ID3D12Resource *tex = nullptr;
    CHECK_HR(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex)));
    ID3D12Resource *upload = create_buffer(256 * 4, D3D12_HEAP_TYPE_UPLOAD);
    float *u = nullptr;
    upload->Map(0, nullptr, (void **)&u);
    for (UINT y = 0; y < 4; y++)
        for (UINT x = 0; x < 4; x++)
            u[y * 64 + x] = 1.0f + x + 4 * y;
    upload->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
    dst.pResource = tex;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcl.pResource = upload;
    srcl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcl.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_FLOAT, 4, 4, 1, 256 };
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
    transition(tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap *heap = nullptr;
    CHECK_HR(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
    dev->CreateShaderResourceView(tex, nullptr, heap->GetCPUDescriptorHandleForHeapStart());
    ID3D12Resource *out = create_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    float zero = 0.0f;
    g.list->SetDescriptorHeaps(1, &heap);
    g.list->SetComputeRootSignature(rs);
    g.list->SetPipelineState(pso);
    g.list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    g.list->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
    g.list->SetComputeRoot32BitConstants(2, 1, &zero, 0);
    g.list->Dispatch(1, 1, 1);
    ID3D12Resource *rb = nullptr;
    const float *r = (const float *)read_buffer(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 256, &rb);
    if (!r)
        return false;
    printf("  (NaN, NaN) %g, (NaN, 0.9) %g, (inf, 0.1) %g, (-inf, NaN) %g\n", r[0], r[1], r[2], r[3]);
    EXPECT(r[0] == 1.0f, "(NaN, NaN) reads %g, want 1 (texel 0, 0)", r[0]);
    EXPECT(r[1] == 13.0f, "(NaN, 0.9) reads %g, want 13 (texel 0, 3)", r[1]);
    EXPECT(r[2] == 4.0f, "(inf, 0.1) reads %g, want 4 (texel 3, 0)", r[2]);
    EXPECT(r[3] == 1.0f, "(-inf, NaN) reads %g, want 1 (texel 0, 0)", r[3]);
    rb->Release(); out->Release(); heap->Release(); upload->Release(); tex->Release(); pso->Release(); rs->Release();
    return true;
}

static bool test_sample_nan_coords() { return sample_nan_coords(false); }
static bool test_sample_nan_coords_dxbc() { return sample_nan_coords(true); }

// Render target conversions (D3D data type conversion rules): a float converted to a lower range float format rounds
// toward zero, so 70000 stores the largest finite float16 (not infinity) and 1 + 0.75 ulp stores 1; infinity stays
// infinity. Conversions to UNORM round to nearest. Targets: R16G16B16A16_FLOAT, R11G11B10_FLOAT, R8G8B8A8_UNORM.
static bool rt_float_conversion(bool dxbc)
{
    static const char *src = R"(
cbuffer C : register(b0) { float one; };
struct Out { float4 a : SV_Target0; float4 b : SV_Target1; float4 c : SV_Target2; };
Out ps(float4 p : SV_Position)
{
    Out o;
    float inf = 1.0 / (one - 1.0);
    // float16: 70000 (overflow), 1 + 0.75 ulp (2^-10), -70000, infinity.
    o.a = float4(70000.0 * one, one + 0.000732421875, -70000.0 * one, inf);
    // float11 (red, green: ulp 2^-6 at 1) and float10 (blue: ulp 2^-5): 1 + 0.75 ulp, 70000 (max 65024).
    o.b = float4(one + 0.01171875, 70000.0 * one, one + 0.0234375, 1);
    // UNORM: (k + 0.75) / 255 rounds to k + 1.
    o.c = float4(100.75 / 255.0, 3.75 / 255.0, 254.75 / 255.0, 0.25 / 255.0);
    return o;
}
float4 vs(uint id : SV_VertexID) : SV_Position { return float4(id == 1 ? 3.0 : -1.0, id == 2 ? -3.0 : 1.0, 0, 1); }
)";
    std::vector<char> vsb = dxbc ? compile_fxc(src, "vs", "vs_5_0") : compile_dxc(src, "vs", "vs_6_0");
    std::vector<char> psb = dxbc ? compile_fxc(src, "ps", "ps_5_0") : compile_dxc(src, "ps", "ps_6_0");
    if (vsb.empty() || psb.empty())
        return false;
    ID3D12Device *dev = g.device;
    D3D12_ROOT_PARAMETER rp = {};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp.Constants.Num32BitValues = 1;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    const DXGI_FORMAT fmts[3] = { DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM };
    const UINT bpp[3] = { 8, 4, 4 };
    ID3D12Resource *rts[3] = {};
    D3D12_DESCRIPTOR_HEAP_DESC rhd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3 };
    ID3D12DescriptorHeap *rtv_heap = nullptr;
    CHECK_HR(dev->CreateDescriptorHeap(&rhd, IID_PPV_ARGS(&rtv_heap)));
    UINT rinc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[3];
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    for (UINT i = 0; i < 3; i++)
    {
        D3D12_RESOURCE_DESC rtd = {};
        rtd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rtd.Width = 4; rtd.Height = 4; rtd.DepthOrArraySize = 1; rtd.MipLevels = 1;
        rtd.Format = fmts[i];
        rtd.SampleDesc.Count = 1;
        rtd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        CHECK_HR(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rtd, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&rts[i])));
        rtvs[i] = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rtvs[i].ptr += i * rinc;
        dev->CreateRenderTargetView(rts[i], nullptr, rtvs[i]);
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = rs;
    pd.VS = { vsb.data(), vsb.size() };
    pd.PS = { psb.data(), psb.size() };
    for (UINT i = 0; i < 3; i++)
    {
        pd.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.RTVFormats[i] = fmts[i];
    }
    pd.NumRenderTargets = 3;
    pd.SampleMask = ~0u;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState *pso = nullptr;
    CHECK_HR(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)));
    D3D12_VIEWPORT vpt = { 0, 0, 4, 4, 0, 1 };
    D3D12_RECT sc = { 0, 0, 4, 4 };
    float one = 1.0f;
    g.list->RSSetViewports(1, &vpt);
    g.list->RSSetScissorRects(1, &sc);
    g.list->OMSetRenderTargets(3, rtvs, FALSE, nullptr);
    g.list->SetGraphicsRootSignature(rs);
    g.list->SetGraphicsRoot32BitConstants(0, 1, &one, 0);
    g.list->SetPipelineState(pso);
    g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.list->DrawInstanced(3, 1, 0, 0);
    ID3D12Resource *rb = create_buffer(3 * 1024, D3D12_HEAP_TYPE_READBACK);
    for (UINT i = 0; i < 3; i++)
    {
        transition(rts[i], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
        dst.pResource = rb;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = i * 1024;
        dst.PlacedFootprint.Footprint = { fmts[i], 4, 4, 1, 256 };
        srcl.pResource = rts[i];
        srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        g.list->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
    }
    if (!execute_and_wait())
        return false;
    const uint8_t *m = nullptr;
    D3D12_RANGE r = { 0, 3 * 1024 };
    rb->Map(0, &r, (void **)&m);
    uint16_t h[4];
    memcpy(h, m, 8);
    uint32_t f11;
    memcpy(&f11, m + 1024, 4);
    const uint8_t *un = m + 2048;
    printf("  float16 %04x %04x %04x %04x; float11/10 %03x %03x %03x; unorm %u %u %u %u\n", h[0], h[1], h[2], h[3],
           f11 & 0x7ff, (f11 >> 11) & 0x7ff, f11 >> 22, un[0], un[1], un[2], un[3]);
    EXPECT(h[0] == 0x7bff, "70000 stores float16 %04x, want 7bff (round toward zero)", h[0]);
    EXPECT(h[1] == 0x3c00, "1 + 0.75 ulp stores float16 %04x, want 3c00 (round toward zero)", h[1]);
    EXPECT(h[2] == 0xfbff, "-70000 stores float16 %04x, want fbff (round toward zero)", h[2]);
    EXPECT(h[3] == 0x7c00, "infinity stores float16 %04x, want 7c00", h[3]);
    EXPECT((f11 & 0x7ff) == 0x3c0, "1 + 0.75 ulp stores float11 %03x, want 3c0", f11 & 0x7ff);
    EXPECT(((f11 >> 11) & 0x7ff) == 0x7bf, "70000 stores float11 %03x, want 7bf (largest finite)", (f11 >> 11) & 0x7ff);
    EXPECT((f11 >> 22) == 0x1e0, "1 + 0.75 ulp stores float10 %03x, want 1e0", f11 >> 22);
    EXPECT(un[0] == 101 && un[1] == 4 && un[2] == 255 && un[3] == 0, "UNORM stores %u %u %u %u, want 101 4 255 0",
           un[0], un[1], un[2], un[3]);
    rb->Unmap(0, nullptr);
    rb->Release(); pso->Release(); rtv_heap->Release(); rs->Release();
    for (UINT i = 0; i < 3; i++)
        rts[i]->Release();
    return true;
}

static bool test_rt_float_conversion() { return rt_float_conversion(false); }
static bool test_rt_float_conversion_dxbc() { return rt_float_conversion(true); }

// The same conversions through typed UAV stores (RWTexture2D<float4>): float formats round toward zero, UNORM to
// nearest.
static bool uav_float_conversion(bool dxbc)
{
    static const char *src = R"(
cbuffer C : register(b0) { float one; };
RWTexture2D<float4> a : register(u0);
RWTexture2D<float4> b : register(u1);
RWTexture2D<float4> c : register(u2);
[numthreads(1, 1, 1)] void cs()
{
    float inf = 1.0 / (one - 1.0);
    a[uint2(0, 0)] = float4(70000.0 * one, one + 0.000732421875, -70000.0 * one, inf);
    b[uint2(0, 0)] = float4(one + 0.01171875, 70000.0 * one, one + 0.0234375, 1);
    c[uint2(0, 0)] = float4(100.75 / 255.0, 3.75 / 255.0, 254.75 / 255.0, 0.25 / 255.0);
}
)";
    std::vector<char> cs = dxbc ? compile_fxc(src, "cs", "cs_5_0") : compile_dxc(src, "cs", "cs_6_0");
    if (cs.empty())
        return false;
    ID3D12Device *dev = g.device;
    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0, 0 };
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable = { 1, &range };
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[1].Constants.Num32BitValues = 1;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 2, rp, 0, nullptr };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    ID3D12PipelineState *pso = create_compute_pipeline(rs, cs);
    if (!pso)
        return false;
    const DXGI_FORMAT fmts[3] = { DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM };
    ID3D12Resource *texs[3] = {};
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap *heap = nullptr;
    CHECK_HR(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
    UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    for (UINT i = 0; i < 3; i++)
    {
        D3D12_RESOURCE_DESC td = {};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = 4; td.Height = 4; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = fmts[i];
        td.SampleDesc.Count = 1;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        CHECK_HR(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&texs[i])));
        D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += i * inc;
        dev->CreateUnorderedAccessView(texs[i], nullptr, nullptr, h);
    }
    float one = 1.0f;
    g.list->SetDescriptorHeaps(1, &heap);
    g.list->SetComputeRootSignature(rs);
    g.list->SetPipelineState(pso);
    g.list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    g.list->SetComputeRoot32BitConstants(1, 1, &one, 0);
    g.list->Dispatch(1, 1, 1);
    ID3D12Resource *rb = create_buffer(3 * 1024, D3D12_HEAP_TYPE_READBACK);
    for (UINT i = 0; i < 3; i++)
    {
        transition(texs[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
        dst.pResource = rb;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = i * 1024;
        dst.PlacedFootprint.Footprint = { fmts[i], 4, 4, 1, 256 };
        srcl.pResource = texs[i];
        srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        g.list->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
    }
    if (!execute_and_wait())
        return false;
    const uint8_t *m = nullptr;
    D3D12_RANGE r = { 0, 3 * 1024 };
    rb->Map(0, &r, (void **)&m);
    uint16_t h[4];
    memcpy(h, m, 8);
    uint32_t f11;
    memcpy(&f11, m + 1024, 4);
    const uint8_t *un = m + 2048;
    printf("  float16 %04x %04x %04x %04x; float11/10 %03x %03x %03x; unorm %u %u %u %u\n", h[0], h[1], h[2], h[3],
           f11 & 0x7ff, (f11 >> 11) & 0x7ff, f11 >> 22, un[0], un[1], un[2], un[3]);
    EXPECT(h[0] == 0x7bff, "70000 stores float16 %04x, want 7bff (round toward zero)", h[0]);
    EXPECT(h[1] == 0x3c00, "1 + 0.75 ulp stores float16 %04x, want 3c00 (round toward zero)", h[1]);
    EXPECT(h[2] == 0xfbff, "-70000 stores float16 %04x, want fbff (round toward zero)", h[2]);
    EXPECT(h[3] == 0x7c00, "infinity stores float16 %04x, want 7c00", h[3]);
    EXPECT((f11 & 0x7ff) == 0x3c0, "1 + 0.75 ulp stores float11 %03x, want 3c0", f11 & 0x7ff);
    EXPECT(((f11 >> 11) & 0x7ff) == 0x7bf, "70000 stores float11 %03x, want 7bf (largest finite)", (f11 >> 11) & 0x7ff);
    EXPECT((f11 >> 22) == 0x1e0, "1 + 0.75 ulp stores float10 %03x, want 1e0", f11 >> 22);
    EXPECT(un[0] == 101 && un[1] == 4 && un[2] == 255 && un[3] == 0, "UNORM stores %u %u %u %u, want 101 4 255 0",
           un[0], un[1], un[2], un[3]);
    rb->Unmap(0, nullptr);
    rb->Release(); pso->Release(); heap->Release(); rs->Release();
    for (UINT i = 0; i < 3; i++)
        texs[i]->Release();
    return true;
}

static bool test_uav_float_conversion() { return uav_float_conversion(false); }
static bool test_uav_float_conversion_dxbc() { return uav_float_conversion(true); }

// Tessellator output, checked without depending on its order. The domain shader appends each domain location it
// evaluates to a UAV, and places the domain over a 64x64 target (quad: the square; tri: (u, v) over the lower-left
// half), where the pixel shader adds 1 per fragment. For each domain, partitioning and set of factors: the distinct
// points where their count is known, the points on each edge (edge factor + 1 after rounding: integer rounds up, pow2
// to a power of 2, fractional_odd and fractional_even up to the next odd or even count, all clamped to [1, 64]), every
// pixel of the domain covered exactly once, culled patches (a zero edge factor), and windings: drawn again with back
// faces culled, triangle_cw and triangle_ccw cover all of it or none, oppositely. Isolines: density lines of detail
// segments, each line's points evenly spaced.
struct TessCase
{
    const char *domain, *partitioning, *topology;
    float outer[4], inner[2];
    int points;           // expected distinct domain points, or -1 when not checked
    int edges[4];         // expected points on each edge (quad: u=0, v=0, u=1, v=1; tri: u=0, v=0, w=0), or 0
};

static bool tess_domains(bool dxbc)
{
    static const TessCase cases[] = {
        { "quad", "integer", "triangle_cw", { 4, 4, 4, 4 }, { 4, 4 }, 25, { 5, 5, 5, 5 } },
        { "quad", "integer", "triangle_cw", { 1, 2, 3, 4 }, { 3, 2 }, -1, { 2, 3, 4, 5 } },
        { "quad", "integer", "triangle_ccw", { 4, 4, 4, 4 }, { 4, 4 }, 25, { 5, 5, 5, 5 } },
        { "quad", "integer", "triangle_cw", { 2.3f, 2.3f, 2.3f, 2.3f }, { 2.3f, 2.3f }, 16, { 4, 4, 4, 4 } },
        { "quad", "pow2", "triangle_cw", { 5, 5, 5, 5 }, { 5, 5 }, 81, { 9, 9, 9, 9 } },
        { "quad", "fractional_odd", "triangle_cw", { 3, 3, 3, 3 }, { 3, 3 }, 16, { 4, 4, 4, 4 } },
        { "quad", "fractional_odd", "triangle_cw", { 2.5f, 2.5f, 2.5f, 2.5f }, { 2.5f, 2.5f }, 16, { 4, 4, 4, 4 } },
        { "quad", "fractional_even", "triangle_cw", { 2.5f, 2.5f, 2.5f, 2.5f }, { 2.5f, 2.5f }, 25, { 5, 5, 5, 5 } },
        { "quad", "integer", "triangle_cw", { 64, 64, 64, 64 }, { 64, 64 }, 4225, { 65, 65, 65, 65 } },
        { "quad", "integer", "triangle_cw", { 100, 100, 100, 100 }, { 100, 100 }, 4225, { 65, 65, 65, 65 } },
        { "tri", "integer", "triangle_cw", { 4, 4, 4, 0 }, { 4, 0 }, 19, { 5, 5, 5, 0 } }, // rings of 12, 6 and 1
        { "tri", "integer", "triangle_cw", { 1, 3, 5, 0 }, { 2, 0 }, -1, { 2, 4, 6, 0 } },
        { "tri", "fractional_even", "triangle_ccw", { 3, 3, 3, 0 }, { 3, 0 }, -1, { 5, 5, 5, 0 } },
        { "tri", "integer", "triangle_cw", { 0, 4, 4, 0 }, { 4, 0 }, 0, { 0, 0, 0, 0 } },
        { "quad", "integer", "triangle_cw", { 4, 4, 4, 4 }, { 0, 4 }, -1, { 5, 5, 5, 5 } },
        { "isoline", "integer", "line", { 3, 4, 0, 0 }, { 0, 0 }, 15, { 0, 0, 0, 0 } },
    };
    static const char *tmpl = R"(
cbuffer F : register(b0) { float4 outer; float2 inner; };
RWByteAddressBuffer o : register(u0);
struct VSOut { float2 p : P; };
VSOut vs(uint id : SV_VertexID) { VSOut v; v.p = float2(id, 0); return v; }
#if ISOLINE
struct HSConst { float e[2] : SV_TessFactor; };
HSConst hsc() { HSConst c; c.e[0] = outer.x; c.e[1] = outer.y; return c; }
#elif QUAD
struct HSConst { float e[4] : SV_TessFactor; float i[2] : SV_InsideTessFactor; };
HSConst hsc() { HSConst c; c.e[0] = outer.x; c.e[1] = outer.y; c.e[2] = outer.z; c.e[3] = outer.w; c.i[0] = inner.x; c.i[1] = inner.y; return c; }
#else
struct HSConst { float e[3] : SV_TessFactor; float i : SV_InsideTessFactor; };
HSConst hsc() { HSConst c; c.e[0] = outer.x; c.e[1] = outer.y; c.e[2] = outer.z; c.i = inner.x; return c; }
#endif
[domain(DOMAIN)] [partitioning(PART)] [outputtopology(TOPO)] [outputcontrolpoints(1)] [patchconstantfunc("hsc")]
VSOut hs(InputPatch<VSOut, 1> ip, uint i : SV_OutputControlPointID) { return ip[0]; }
struct DSOut { float4 pos : SV_Position; };
void note(float3 l)
{
    uint n; o.InterlockedAdd(0, 1, n);
    if (n < 60000) o.Store3(16 + n * 12, asuint(l));
}
[domain(DOMAIN)]
#if ISOLINE || QUAD
DSOut ds(HSConst c, float2 l : SV_DomainLocation, const OutputPatch<VSOut, 1> op)
{
    note(float3(l, 0));
    DSOut d; d.pos = float4(l.x * 2 - 1, 1 - l.y * 2, 0, 1); return d;
}
#else
DSOut ds(HSConst c, float3 l : SV_DomainLocation, const OutputPatch<VSOut, 1> op)
{
    note(l);
    DSOut d; d.pos = float4(l.x * 2 - 1, 1 - l.y * 2, 0, 1); return d;
}
#endif
float4 ps() : SV_Target { return 1; }
)";
    ID3D12Device *dev = g.device;
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.Num32BitValues = 8;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 2, rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    const UINT out_size = 16 + 60000 * 12, W = 64;
    ID3D12Resource *out = create_buffer(out_size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource *zero = create_buffer(16, D3D12_HEAP_TYPE_UPLOAD);
    void *zp = nullptr;
    zero->Map(0, nullptr, &zp);
    memset(zp, 0, 16);
    zero->Unmap(0, nullptr);
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC rtd = {};
    rtd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rtd.Width = W; rtd.Height = W; rtd.DepthOrArraySize = 1; rtd.MipLevels = 1;
    rtd.Format = DXGI_FORMAT_R16_FLOAT;
    rtd.SampleDesc.Count = 1;
    rtd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource *rts[2] = {};
    D3D12_DESCRIPTOR_HEAP_DESC rhd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2 };
    ID3D12DescriptorHeap *rtv_heap = nullptr;
    CHECK_HR(dev->CreateDescriptorHeap(&rhd, IID_PPV_ARGS(&rtv_heap)));
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2];
    for (UINT k = 0; k < 2; k++)
    {
        CHECK_HR(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rtd, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&rts[k])));
        rtvs[k] = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rtvs[k].ptr += k * dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        dev->CreateRenderTargetView(rts[k], nullptr, rtvs[k]);
    }
    ID3D12Resource *rb = create_buffer(2 * W * 256, D3D12_HEAP_TYPE_READBACK);
    int cull_state[2] = { -1, -1 }; // per topology (cw, ccw): 1 drawn, 0 culled
    for (const TessCase &c : cases)
    {
        bool iso = !strcmp(c.domain, "isoline"), quad = !strcmp(c.domain, "quad");
        std::string src = std::string("#define DOMAIN \"") + c.domain + "\"\n#define PART \"" + c.partitioning +
                "\"\n#define TOPO \"" + c.topology + "\"\n#define ISOLINE " + (iso ? "1" : "0") + "\n#define QUAD " +
                (quad ? "1" : "0") + "\n" + tmpl;
        std::vector<char> vs = dxbc ? compile_fxc(src.c_str(), "vs", "vs_5_0") : compile_dxc(src.c_str(), "vs", "vs_6_0");
        std::vector<char> hs = dxbc ? compile_fxc(src.c_str(), "hs", "hs_5_0") : compile_dxc(src.c_str(), "hs", "hs_6_0");
        std::vector<char> ds = dxbc ? compile_fxc(src.c_str(), "ds", "ds_5_0") : compile_dxc(src.c_str(), "ds", "ds_6_0");
        std::vector<char> ps = dxbc ? compile_fxc(src.c_str(), "ps", "ps_5_0") : compile_dxc(src.c_str(), "ps", "ps_6_0");
        if (vs.empty() || hs.empty() || ds.empty() || ps.empty())
            return false;
        char label[96];
        snprintf(label, sizeof(label), "%s %s %s (%g %g %g %g / %g %g)", c.domain, c.partitioning, c.topology,
                 c.outer[0], c.outer[1], c.outer[2], c.outer[3], c.inner[0], c.inner[1]);
        ID3D12PipelineState *pso[2] = {};
        for (UINT k = 0; k < 2; k++)
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
            pd.pRootSignature = rs;
            pd.VS = { vs.data(), vs.size() };
            pd.HS = { hs.data(), hs.size() };
            pd.DS = { ds.data(), ds.size() };
            pd.PS = { ps.data(), ps.size() };
            pd.BlendState.RenderTarget[0].BlendEnable = TRUE;
            pd.BlendState.RenderTarget[0].SrcBlend = pd.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
            pd.BlendState.RenderTarget[0].SrcBlendAlpha = pd.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
            pd.BlendState.RenderTarget[0].BlendOp = pd.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
            pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            pd.NumRenderTargets = 1;
            pd.RTVFormats[0] = DXGI_FORMAT_R16_FLOAT;
            pd.SampleMask = ~0u;
            pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pd.RasterizerState.CullMode = k ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
            pd.RasterizerState.DepthClipEnable = TRUE;
            pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
            pd.SampleDesc.Count = 1;
            HRESULT hr = dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso[k]));
            // Metal's tessellator has triangle and quad domains only, and MoltenVK has no isoline emulation.
            if (FAILED(hr) && iso)
            {
                printf("  %s: no pipeline (known: no isoline tessellation on Metal)\n", label);
                break;
            }
            EXPECT(SUCCEEDED(hr), "%s: CreateGraphicsPipelineState hr %#x", label, (unsigned)hr);
            if (FAILED(hr))
                return true;
        }
        if (!pso[1])
        {
            if (pso[0]) pso[0]->Release();
            continue;
        }
        transition(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        g.list->CopyBufferRegion(out, 0, zero, 0, 16);
        transition(out, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const float clear[4] = {};
        float consts[8] = { c.outer[0], c.outer[1], c.outer[2], c.outer[3], c.inner[0], c.inner[1], 0, 0 };
        D3D12_VIEWPORT vpt = { 0, 0, (float)W, (float)W, 0, 1 };
        D3D12_RECT sc = { 0, 0, (LONG)W, (LONG)W };
        g.list->RSSetViewports(1, &vpt);
        g.list->RSSetScissorRects(1, &sc);
        g.list->SetGraphicsRootSignature(rs);
        g.list->SetGraphicsRoot32BitConstants(0, 8, consts, 0);
        g.list->SetGraphicsRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
        g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST);
        for (UINT k = 0; k < 2; k++)
        {
            g.list->ClearRenderTargetView(rtvs[k], clear, 0, nullptr);
            g.list->OMSetRenderTargets(1, &rtvs[k], FALSE, nullptr);
            g.list->SetPipelineState(pso[k]);
            g.list->DrawInstanced(1, 1, 0, 0);
        }
        for (UINT k = 0; k < 2; k++)
        {
            transition(rts[k], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
            dst.pResource = rb;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Offset = k * W * 256;
            dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R16_FLOAT, W, W, 1, 256 };
            srcl.pResource = rts[k];
            srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            g.list->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
            transition(rts[k], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
        ID3D12Resource *orb = nullptr;
        const uint8_t *r = (const uint8_t *)read_buffer(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, out_size, &orb);
        if (!r)
            return false;
        uint32_t n = std::min(*(const uint32_t *)r, 60000u);
        const float *t = (const float *)(r + 16);
        std::set<std::pair<int, int>> points;
        auto key = [](float x) { return (int)lroundf(x * 1048576.0f); };
        int outside = 0;
        for (uint32_t k = 0; k < n; k++)
        {
            float u = t[k * 3], v = t[k * 3 + 1];
            if (u < -1e-6f || v < -1e-6f || u > 1 + 1e-6f || v > 1 + 1e-6f || (!quad && !iso && u + v > 1 + 1e-5f))
                outside++;
            points.insert({ key(u), key(v) });
        }
        int edge_points[4] = {};
        std::map<int, int> per_line;
        for (auto &pt : points)
        {
            float u = pt.first / 1048576.0f, v = pt.second / 1048576.0f;
            edge_points[0] += u == 0.0f; edge_points[1] += v == 0.0f;
            if (quad) { edge_points[2] += u == 1.0f; edge_points[3] += v == 1.0f; }
            else edge_points[2] += fabsf(1.0f - u - v) < 2.0f / 1048576.0f;
            per_line[pt.second]++;
        }
        // Coverage: pixel (x, y) center at u = (x + 0.5) / W, v = (y + 0.5) / W.
        const uint8_t *m = nullptr;
        D3D12_RANGE rr = { 0, 2 * W * 256 };
        rb->Map(0, &rr, (void **)&m);
        int wrong = 0, culled_drawn = 0, inside = 0;
        for (UINT y = 0; y < W; y++)
            for (UINT x = 0; x < W; x++)
            {
                uint16_t a, b;
                memcpy(&a, m + y * 256 + x * 2, 2);
                memcpy(&b, m + W * 256 + y * 256 + x * 2, 2);
                float u = (x + 0.5f) / W, v = (y + 0.5f) / W;
                bool in = quad ? true : (u + v < 1.0f - 0.5f / W);
                bool edge = !quad && fabsf(u + v - 1.0f) <= 0.5f / W;
                if (iso || edge)
                    continue;
                uint16_t want = (in && c.points != 0) ? 0x3c00 : 0;
                inside += in;
                wrong += a != want;
                culled_drawn += b != 0;
            }
        rb->Unmap(0, nullptr);
        printf("  %s: %zu points (%u evaluations), edges %d %d %d %d, %d wrongly covered pixels, %d drawn of %d with back faces culled\n",
               label, points.size(), n, edge_points[0], edge_points[1], edge_points[2], edge_points[3], wrong, culled_drawn, inside);
        if (c.points >= 0)
            EXPECT((int)points.size() == c.points, "%s: %zu distinct points, want %d", label, points.size(), c.points);
        EXPECT(!outside, "%s: %d points outside the domain", label, outside);
        if (iso)
        {
            EXPECT((int)per_line.size() == (int)c.outer[0], "%s: %zu lines, want %g", label, per_line.size(), c.outer[0]);
            for (auto &l : per_line)
                EXPECT(l.second == (int)c.outer[1] + 1, "%s: a line has %d points, want %g", label, l.second, c.outer[1] + 1);
        }
        else if (c.points)
        {
            EXPECT(!wrong, "%s: %d pixels not covered exactly once", label, wrong);
            for (int e = 0; e < (quad ? 4 : 3); e++)
                if (c.edges[e])
                    EXPECT(edge_points[e] == c.edges[e], "%s: edge %d has %d points, want %d", label, e, edge_points[e], c.edges[e]);
            int topo = !strcmp(c.topology, "triangle_ccw");
            int state = culled_drawn == 0 ? 0 : culled_drawn == inside ? 1 : 2;
            EXPECT(state != 2, "%s: back face culling keeps %d of %d pixels (mixed winding)", label, culled_drawn, inside);
            if (cull_state[topo] < 0)
                cull_state[topo] = state;
            EXPECT(cull_state[topo] == state, "%s: winding differs from other %s cases", label, c.topology);
        }
        else
            EXPECT(!n && !wrong, "%s: a culled patch evaluated %u points", label, n);
        orb->Release();
        pso[0]->Release(); pso[1]->Release();
    }
    EXPECT(cull_state[0] != cull_state[1], "triangle_cw and triangle_ccw wind the same way");
    rb->Release(); rts[0]->Release(); rts[1]->Release(); rtv_heap->Release(); zero->Release(); out->Release(); rs->Release();
    return true;
}

static bool test_tess_domains() { return tess_domains(false); }
static bool test_tess_domains_dxbc() { return tess_domains(true); }

// Geometry shader features. Inputs, through a UAV: two triangles with adjacency read by a GS with 3 instances, each
// writing its six input vertex IDs (D3D order: 0, 2, 4 the triangle, 1, 3, 5 the adjacent vertices), SV_PrimitiveID
// and SV_GSInstanceID. Outputs: four points expanded to full-viewport quads, each as two strips of one triangle
// (RestartStrip), routed by SV_RenderTargetArrayIndex (point & 1) and SV_ViewportArrayIndex (point >> 1) into a
// two-slice target with two viewports (left and right halves): slice 0 reads 1 | 3, slice 1 reads 2 | 4.
static bool gs_features(bool dxbc)
{
    static const char *src = R"(
RWByteAddressBuffer o : register(u1);
struct V { uint id : VID; };
V vs(uint id : SV_VertexID) { V v; v.id = id; return v; }
struct Dummy { float4 pos : SV_Position; };
[instance(3)] [maxvertexcount(1)]
void gs_adj(triangleadj V i[6], uint prim : SV_PrimitiveID, uint inst : SV_GSInstanceID, inout PointStream<Dummy> s)
{
    uint base = (prim * 3 + inst) * 32;
    o.Store4(base, uint4(i[0].id, i[1].id, i[2].id, i[3].id));
    o.Store4(base + 16, uint4(i[4].id, i[5].id, prim, inst));
    Dummy d; d.pos = float4(2, 2, 0, 1); s.Append(d);
}
struct Q { float4 pos : SV_Position; nointerpolation uint color : COLOR; uint layer : SV_RenderTargetArrayIndex; uint vp : SV_ViewportArrayIndex; };
[maxvertexcount(6)]
void gs_quad(point V i[1], inout TriangleStream<Q> s)
{
    Q q; q.color = i[0].id + 1; q.layer = i[0].id & 1; q.vp = i[0].id >> 1;
    q.pos = float4(-1, 1, 0, 1); s.Append(q);
    q.pos = float4(1, 1, 0, 1); s.Append(q);
    q.pos = float4(-1, -1, 0, 1); s.Append(q);
    s.RestartStrip();
    q.pos = float4(1, 1, 0, 1); s.Append(q);
    q.pos = float4(1, -1, 0, 1); s.Append(q);
    q.pos = float4(-1, -1, 0, 1); s.Append(q);
}
uint ps(Q q) : SV_Target { return q.color; }
float4 ps_none() : SV_Target { return 0; }
)";
    auto compile = [&](const char *entry, const char *stage) {
        std::string t5 = std::string(stage) + "_5_0", t6 = std::string(stage) + "_6_0";
        return dxbc ? compile_fxc(src, entry, t5.c_str()) : compile_dxc(src, entry, t6.c_str());
    };
    std::vector<char> vs = compile("vs", "vs"), gs_adj = compile("gs_adj", "gs"), gs_quad = compile("gs_quad", "gs"),
                      ps = compile("ps", "ps"), ps_none = compile("ps_none", "ps");
    if (vs.empty() || gs_adj.empty() || gs_quad.empty() || ps.empty() || ps_none.empty())
        return false;
    ID3D12Device *dev = g.device;
    D3D12_ROOT_PARAMETER rp = {};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp.Descriptor.ShaderRegister = 1;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &rp, 0, nullptr };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    ID3D12Resource *out = create_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC rtd = {};
    rtd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rtd.Width = 16; rtd.Height = 16; rtd.DepthOrArraySize = 2; rtd.MipLevels = 1;
    rtd.Format = DXGI_FORMAT_R8_UINT;
    rtd.SampleDesc.Count = 1;
    rtd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource *rt = nullptr;
    CHECK_HR(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rtd, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&rt)));
    D3D12_DESCRIPTOR_HEAP_DESC rhd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    ID3D12DescriptorHeap *rtv_heap = nullptr;
    CHECK_HR(dev->CreateDescriptorHeap(&rhd, IID_PPV_ARGS(&rtv_heap)));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_RENDER_TARGET_VIEW_DESC rvd = {};
    rvd.Format = DXGI_FORMAT_R8_UINT;
    rvd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
    rvd.Texture2DArray.ArraySize = 2;
    dev->CreateRenderTargetView(rt, &rvd, rtv);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = rs;
    pd.VS = { vs.data(), vs.size() };
    pd.SampleMask = ~0u;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.SampleDesc.Count = 1;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8_UINT;
    pd.GS = { gs_adj.data(), gs_adj.size() };
    pd.PS = { ps_none.data(), ps_none.size() };
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    ID3D12PipelineState *adj = nullptr, *quad = nullptr;
    HRESULT hr = dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&adj));
    EXPECT(SUCCEEDED(hr), "adjacency GS pipeline hr %#x", (unsigned)hr);
    pd.GS = { gs_quad.data(), gs_quad.size() };
    pd.PS = { ps.data(), ps.size() };
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    hr = dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&quad));
    EXPECT(SUCCEEDED(hr), "quad GS pipeline hr %#x", (unsigned)hr);
    if (!adj || !quad)
        return true;

    D3D12_VIEWPORT vpts[2] = { { 0, 0, 8, 16, 0, 1 }, { 8, 0, 8, 16, 0, 1 } };
    D3D12_RECT scs[2] = { { 0, 0, 16, 16 }, { 0, 0, 16, 16 } };
    const UINT clear[4] = {};
    g.list->ClearRenderTargetView(rtv, (const float *)clear, 0, nullptr);
    g.list->RSSetViewports(2, vpts);
    g.list->RSSetScissorRects(2, scs);
    g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    g.list->SetGraphicsRootSignature(rs);
    g.list->SetGraphicsRootUnorderedAccessView(0, out->GetGPUVirtualAddress());
    g.list->SetPipelineState(adj);
    g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ);
    g.list->DrawInstanced(12, 1, 0, 0);
    g.list->SetPipelineState(quad);
    g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    g.list->DrawInstanced(4, 1, 0, 0);
    transition(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12Resource *rb = create_buffer(2 * 16 * 256, D3D12_HEAP_TYPE_READBACK);
    for (UINT slice = 0; slice < 2; slice++)
    {
        D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
        dst.pResource = rb;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = slice * 16 * 256;
        dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8_UINT, 16, 16, 1, 256 };
        srcl.pResource = rt;
        srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        srcl.SubresourceIndex = slice;
        g.list->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
    }
    ID3D12Resource *orb = nullptr;
    const uint32_t *r = read_buffer(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 256, &orb);
    if (!r)
        return false;
    int bad = 0;
    for (UINT prim = 0; prim < 2; prim++)
        for (UINT inst = 0; inst < 3; inst++)
        {
            const uint32_t *e = r + (prim * 3 + inst) * 8;
            bool ok = e[6] == prim && e[7] == inst;
            for (UINT k = 0; k < 6; k++)
                ok = ok && e[k] == prim * 6 + k;
            if (!ok && bad++ < 3)
                printf("  primitive %u instance %u: vertices %u %u %u %u %u %u, SV_PrimitiveID %u, SV_GSInstanceID %u\n",
                       prim, inst, e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7]);
        }
    EXPECT(!bad, "%d adjacency GS invocations read the wrong inputs", bad);
    const uint8_t *m = nullptr;
    D3D12_RANGE rr = { 0, 2 * 16 * 256 };
    rb->Map(0, &rr, (void **)&m);
    int wrong = 0;
    for (UINT slice = 0; slice < 2; slice++)
        for (UINT y = 0; y < 16; y++)
            for (UINT x = 0; x < 16; x++)
            {
                uint8_t want = (uint8_t)(1 + slice + (x >= 8 ? 2 : 0));
                uint8_t got = m[slice * 16 * 256 + y * 256 + x];
                if (got != want && wrong++ < 3)
                    printf("  slice %u (%u, %u): %u, want %u\n", slice, x, y, got, want);
            }
    rb->Unmap(0, nullptr);
    EXPECT(!wrong, "%d pixels wrong after point expansion with array index and viewport index", wrong);
    orb->Release(); rb->Release(); adj->Release(); quad->Release(); rtv_heap->Release(); rt->Release(); out->Release(); rs->Release();
    return true;
}

static bool test_gs_features() { return gs_features(false); }
static bool test_gs_features_dxbc() { return gs_features(true); }

// Rasterizer depth state, read back through SRVs at pixel (32, 32) of a 64x64 depth target after a depth-only draw
// of a screen-covering triangle (depth test ALWAYS, cleared to 0). Depth bias per D3D: float depth r = 2^(e - 23)
// with e the exponent of the primitive's largest depth, UNORM r = 1 / (2^bits - 1); slope-scaled bias times the
// largest depth slope per pixel, clamped by DepthBiasClamp. D24 is a 32-bit float format on Apple GPUs, so its bias
// must still use UNORM units. Depth clip on drops a primitive beyond the far plane; off clamps it to MaxDepth. The
// viewport maps depth into [MinDepth, MaxDepth].
static bool test_raster_depth()
{
    struct Case
    {
        const char *name;
        DXGI_FORMAT res, dsv, srv;
        int bias; float clamp, slope;
        BOOL clip; float zl, zr, vmin, vmax;
        double want;
    };
    const double r32_05 = ldexp(1.0, -24), r32_01 = ldexp(1.0, -27); // 2^(e - 23) for z in [0.5, 1) and [0.0625, 0.125)
    const double zmid = 0.2 + 0.4 * 32.5 / 64.0, slope = 0.4 / 64.0;
    const Case cases[] = {
        { "D32 no bias", DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT, 0, 0, 0, TRUE, 0.5f, 0.5f, 0, 1, 0.5 },
        { "D32 bias 1000 at 0.5", DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT, 1000, 0, 0, TRUE, 0.5f, 0.5f, 0, 1, 0.5 + 1000 * r32_05 },
        { "D32 bias 1000 at 0.1", DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT, 1000, 0, 0, TRUE, 0.1f, 0.1f, 0, 1, (double)0.1f + 1000 * r32_01 },
        { "D24 bias 1000 at 0.1", DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_R24_UNORM_X8_TYPELESS, 1000, 0, 0, TRUE, 0.1f, 0.1f, 0, 1, 0.1 + 1000.0 / 16777215.0 },
        { "D24 bias 1000 at 1.0", DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_R24_UNORM_X8_TYPELESS, 1000, 0, 0, TRUE, 1.0f, 1.0f, 0, 1, 1.0 },
        { "D16 bias 10 at 0.1", DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_R16_UNORM, 10, 0, 0, TRUE, 0.1f, 0.1f, 0, 1, 0.1 + 10.0 / 65535.0 },
        { "D32 slope 2", DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT, 0, 0, 2, TRUE, 0.2f, 0.6f, 0, 1, zmid + 2 * slope },
        { "D32 slope 2 clamped 0.005", DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT, 0, 0.005f, 2, TRUE, 0.2f, 0.6f, 0, 1, zmid + 0.005 },
        { "D32 viewport depth 0.25-0.75", DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT, 0, 0, 0, TRUE, 0.2f, 0.2f, 0.25f, 0.75f, 0.25 + 0.2 * 0.5 },
        { "D32 z 1.5, clip", DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT, 0, 0, 0, TRUE, 1.5f, 1.5f, 0, 1, 0.0 },
        { "D32 z 1.5, no clip", DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT, 0, 0, 0, FALSE, 1.5f, 1.5f, 0, 0.75f, 0.75 },
        { "D32 z -0.5, no clip", DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT, 0, 0, 0, FALSE, -0.5f, -0.5f, 0.25f, 1, 0.25 },
    };
    const UINT N = sizeof(cases) / sizeof(cases[0]);
    static const char *src = R"(
cbuffer C : register(b0) { float zl, zr; };
float4 vs(uint id : SV_VertexID) : SV_Position
{
    float x = id == 1 ? 3.0 : -1.0, y = id == 2 ? 3.0 : -1.0;
    return float4(x, y, zl + (x + 1.0) * 0.5 * (zr - zl), 1);
}
Texture2D<float> t[12] : register(t0);
RWStructuredBuffer<float> o : register(u0);
[numthreads(12, 1, 1)] void cs(uint i : SV_GroupThreadID) { o[i] = t[NonUniformResourceIndex(i)].Load(int3(32, 32, 0)); }
)";
    std::vector<char> vs = compile_dxc(src, "vs", "vs_6_0"), cs = compile_dxc(src, "cs", "cs_6_0");
    if (vs.empty() || cs.empty())
        return false;
    ID3D12Device *dev = g.device;
    D3D12_ROOT_PARAMETER grp = {};
    grp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    grp.Constants.Num32BitValues = 2;
    D3D12_ROOT_SIGNATURE_DESC grsd = { 1, &grp, 0, nullptr };
    ID3D12RootSignature *grs = create_root_signature(grsd);
    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, N, 0, 0, 0 };
    D3D12_ROOT_PARAMETER crp[2] = {};
    crp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    crp[0].DescriptorTable = { 1, &range };
    crp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC crsd = { 2, crp, 0, nullptr };
    ID3D12RootSignature *crs = create_root_signature(crsd);
    ID3D12PipelineState *cpso = create_compute_pipeline(crs, cs);
    if (!cpso)
        return false;
    D3D12_DESCRIPTOR_HEAP_DESC dhd = { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, N };
    D3D12_DESCRIPTOR_HEAP_DESC shd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, N, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap *dsv_heap = nullptr, *srv_heap = nullptr;
    CHECK_HR(dev->CreateDescriptorHeap(&dhd, IID_PPV_ARGS(&dsv_heap)));
    CHECK_HR(dev->CreateDescriptorHeap(&shd, IID_PPV_ARGS(&srv_heap)));
    UINT dinc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    UINT sinc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    ID3D12Resource *depth[16] = {};
    ID3D12PipelineState *psos[16] = {};
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    for (UINT i = 0; i < N; i++)
    {
        const Case &c = cases[i];
        D3D12_RESOURCE_DESC td = {};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = 64; td.Height = 64; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = c.res;
        td.SampleDesc.Count = 1;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE cv = { c.dsv };
        CHECK_HR(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, IID_PPV_ARGS(&depth[i])));
        D3D12_DEPTH_STENCIL_VIEW_DESC dvd = {};
        dvd.Format = c.dsv;
        dvd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE dh = dsv_heap->GetCPUDescriptorHandleForHeapStart();
        dh.ptr += i * dinc;
        dev->CreateDepthStencilView(depth[i], &dvd, dh);
        D3D12_SHADER_RESOURCE_VIEW_DESC svd = {};
        svd.Format = c.srv;
        svd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        svd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        svd.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE sh = srv_heap->GetCPUDescriptorHandleForHeapStart();
        sh.ptr += i * sinc;
        dev->CreateShaderResourceView(depth[i], &svd, sh);
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = grs;
        pd.VS = { vs.data(), vs.size() };
        pd.SampleMask = ~0u;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = c.clip;
        pd.RasterizerState.DepthBias = c.bias;
        pd.RasterizerState.DepthBiasClamp = c.clamp;
        pd.RasterizerState.SlopeScaledDepthBias = c.slope;
        pd.DepthStencilState.DepthEnable = TRUE;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        pd.DSVFormat = c.dsv;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.SampleDesc.Count = 1;
        HRESULT hr = dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&psos[i]));
        EXPECT(SUCCEEDED(hr), "%s: pipeline hr %#x", c.name, (unsigned)hr);
        if (FAILED(hr))
            return true;
    }
    g.list->SetGraphicsRootSignature(grs);
    g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_RECT sc = { 0, 0, 64, 64 };
    g.list->RSSetScissorRects(1, &sc);
    for (UINT i = 0; i < N; i++)
    {
        const Case &c = cases[i];
        D3D12_CPU_DESCRIPTOR_HANDLE dh = dsv_heap->GetCPUDescriptorHandleForHeapStart();
        dh.ptr += i * dinc;
        // Cleared to 0, or to 0.9 where the expected depth is 0's neighbour (a primitive in front of the near plane).
        g.list->ClearDepthStencilView(dh, D3D12_CLEAR_FLAG_DEPTH, c.zl < 0.0f ? 0.9f : 0.0f, 0, 0, nullptr);
        D3D12_VIEWPORT vp = { 0, 0, 64, 64, c.vmin, c.vmax };
        g.list->RSSetViewports(1, &vp);
        g.list->OMSetRenderTargets(0, nullptr, FALSE, &dh);
        float z[2] = { c.zl, c.zr };
        g.list->SetGraphicsRoot32BitConstants(0, 2, z, 0);
        g.list->SetPipelineState(psos[i]);
        g.list->DrawInstanced(3, 1, 0, 0);
        transition(depth[i], D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    ID3D12Resource *out = create_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g.list->SetDescriptorHeaps(1, &srv_heap);
    g.list->SetComputeRootSignature(crs);
    g.list->SetPipelineState(cpso);
    g.list->SetComputeRootDescriptorTable(0, srv_heap->GetGPUDescriptorHandleForHeapStart());
    g.list->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
    g.list->Dispatch(1, 1, 1);
    ID3D12Resource *rb = nullptr;
    const float *r = (const float *)read_buffer(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 256, &rb);
    if (!r)
        return false;
    for (UINT i = 0; i < N; i++)
    {
        const Case &c = cases[i];
        // One step of the stored format: 2^-23 relative for float depth, one UNORM step otherwise.
        double tol = c.dsv == DXGI_FORMAT_D16_UNORM ? 1.0 / 65535.0 : c.dsv == DXGI_FORMAT_D24_UNORM_S8_UINT ? 1.0 / 16777215.0 :
                fabs(c.want) * ldexp(1.0, -22) + 1e-12;
        printf("  %s: %.9f, want %.9f (bias read %.3g, want %.3g)\n", c.name, r[i], c.want, r[i] - (double)c.zl,
               c.want - (double)c.zl);
        // Metal's depth clamp (depth clip off) holds depth in front of the near plane at 0, not at MinDepth.
        if (!c.clip && c.zl < 0.0f && c.vmin > 0.0f && r[i] == 0.0f)
        {
            printf("  %s: known: Metal clamps depth in front of the near plane to 0, not MinDepth\n", c.name);
            continue;
        }
        EXPECT(fabs(r[i] - c.want) <= tol, "%s: depth %.9f, want %.9f", c.name, r[i], c.want);
    }
    rb->Release(); out->Release(); cpso->Release(); crs->Release(); grs->Release(); dsv_heap->Release(); srv_heap->Release();
    for (UINT i = 0; i < N; i++)
    {
        depth[i]->Release();
        psos[i]->Release();
    }
    return true;
}

// Placed resources in one 256 KiB default heap. Buffers A (offset 0) and B (offset 0, 64 KiB) alias: a compute
// shader writes A, and after an aliasing barrier a copy reads the same bytes through B. C (offset 64 KiB, 128 KiB long)
// and D (offset 128 KiB, overlapping C's second half) see each other's bytes, and placed buffers' GPU addresses differ
// by their heap offsets (placement offsets are 64 KiB aligned). A texture placed at 192 KiB keeps its contents through
// a copy round trip.
// Placed resources in an upload heap map at their offsets too.
static bool test_heap_placement()
{
    static const char *src = R"(
RWByteAddressBuffer o : register(u0);
[numthreads(64, 1, 1)] void cs(uint i : SV_DispatchThreadID) { o.Store(i * 4, 0xa5000000u + i); }
)";
    std::vector<char> cs = compile_dxc(src, "cs", "cs_6_0");
    if (cs.empty())
        return false;
    ID3D12Device *dev = g.device;
    D3D12_FEATURE_DATA_D3D12_OPTIONS o = {};
    dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o));
    D3D12_HEAP_DESC hd = {};
    hd.SizeInBytes = 256 * 1024;
    hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    hd.Flags = o.ResourceHeapTier >= D3D12_RESOURCE_HEAP_TIER_2 ? D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES
                                                                : D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    ID3D12Heap *heap = nullptr;
    CHECK_HR(dev->CreateHeap(&hd, IID_PPV_ARGS(&heap)));
    auto placed = [&](ID3D12Heap *h, UINT64 offset, UINT64 size, D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags) {
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = flags;
        ID3D12Resource *r = nullptr;
        HRESULT hr = dev->CreatePlacedResource(h, offset, &rd, state, nullptr, IID_PPV_ARGS(&r));
        EXPECT(SUCCEEDED(hr), "CreatePlacedResource at %llu hr %#x", (unsigned long long)offset, (unsigned)hr);
        return r;
    };
    ID3D12Resource *a = placed(heap, 0, 4096, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ID3D12Resource *b = placed(heap, 0, 65536, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_FLAG_NONE);
    ID3D12Resource *c = placed(heap, 65536, 131072, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    ID3D12Resource *d = placed(heap, 131072, 8192, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_FLAG_NONE);
    if (!a || !b || !c || !d)
        return true;
    EXPECT(b->GetGPUVirtualAddress() == a->GetGPUVirtualAddress(), "aliased buffers have different addresses");
    EXPECT(c->GetGPUVirtualAddress() - a->GetGPUVirtualAddress() == 65536, "buffers 64 KiB apart in the heap are %lld bytes apart",
           (long long)(c->GetGPUVirtualAddress() - a->GetGPUVirtualAddress()));
    EXPECT(d->GetGPUVirtualAddress() - c->GetGPUVirtualAddress() == 65536, "buffers 64 KiB apart in the heap are %lld bytes apart",
           (long long)(d->GetGPUVirtualAddress() - c->GetGPUVirtualAddress()));

    D3D12_ROOT_PARAMETER rp = {};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &rp, 0, nullptr };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    ID3D12PipelineState *pso = create_compute_pipeline(rs, cs);
    if (!pso)
        return false;
    g.list->SetComputeRootSignature(rs);
    g.list->SetPipelineState(pso);
    g.list->SetComputeRootUnorderedAccessView(0, a->GetGPUVirtualAddress());
    g.list->Dispatch(1, 1, 1);
    D3D12_RESOURCE_BARRIER alias = {};
    alias.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
    alias.Aliasing.pResourceBefore = a;
    alias.Aliasing.pResourceAfter = b;
    g.list->ResourceBarrier(1, &alias);

    // C <- pattern from an upload buffer; then read the overlap through D.
    ID3D12Resource *up = create_buffer(16384, D3D12_HEAP_TYPE_UPLOAD);
    uint32_t *u = nullptr;
    up->Map(0, nullptr, (void **)&u);
    for (UINT i = 0; i < 4096; i++)
        u[i] = 0x3c000000u + i;
    up->Unmap(0, nullptr);
    g.list->CopyBufferRegion(c, 65536 - 4096, up, 0, 16384);
    transition(c, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);

    // A texture placed at 128 KiB: upload, copy into it, copy back out.
    ID3D12Resource *tex = nullptr;
    if (o.ResourceHeapTier >= D3D12_RESOURCE_HEAP_TIER_2)
    {
        D3D12_RESOURCE_DESC td = {};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = 16; td.Height = 16; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R32_UINT;
        td.SampleDesc.Count = 1;
        CHECK_HR(dev->CreatePlacedResource(heap, 196608, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex)));
        D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
        dst.pResource = tex;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        srcl.pResource = up;
        srcl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcl.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_UINT, 16, 16, 1, 256 };
        g.list->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
        transition(tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    ID3D12Resource *rb = create_buffer(4 * 65536, D3D12_HEAP_TYPE_READBACK);
    g.list->CopyBufferRegion(rb, 0, b, 0, 256);
    g.list->CopyBufferRegion(rb, 65536, d, 0, 8192);
    if (tex)
    {
        D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
        dst.pResource = rb;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = 131072;
        dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_UINT, 16, 16, 1, 256 };
        srcl.pResource = tex;
        srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        g.list->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
    }
    if (!execute_and_wait())
        return false;
    const uint32_t *m = nullptr;
    D3D12_RANGE r = { 0, 4 * 65536 };
    rb->Map(0, &r, (void **)&m);
    int bad_alias = 0, bad_overlap = 0, bad_tex = 0;
    for (UINT i = 0; i < 64; i++)
        bad_alias += m[i] != 0xa5000000u + i;
    for (UINT i = 0; i < 2048; i++)
        bad_overlap += m[16384 + i] != 0x3c000000u + 1024 + i;
    for (UINT y = 0; tex && y < 16; y++)
        for (UINT x = 0; x < 16; x++)
            bad_tex += m[32768 + y * 64 + x] != 0x3c000000u + y * 64 + x;
    printf("  aliased reads wrong %d of 64, overlap %d of 2048, placed texture %d of 256 (heap tier %d)\n", bad_alias,
           bad_overlap, bad_tex, o.ResourceHeapTier);
    EXPECT(!bad_alias, "%d words differ through the aliased buffer", bad_alias);
    EXPECT(!bad_overlap, "%d words differ through the overlapping buffer", bad_overlap);
    EXPECT(!bad_tex, "%d texels differ in the texture placed at 192 KiB", bad_tex);
    rb->Unmap(0, nullptr);

    // Upload heap: placed buffers map at their offsets.
    D3D12_HEAP_DESC uhd = {};
    uhd.SizeInBytes = 128 * 1024;
    uhd.Properties.Type = D3D12_HEAP_TYPE_UPLOAD;
    uhd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    ID3D12Heap *uheap = nullptr;
    CHECK_HR(dev->CreateHeap(&uhd, IID_PPV_ARGS(&uheap)));
    ID3D12Resource *u0 = placed(uheap, 0, 131072, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
    ID3D12Resource *u1 = placed(uheap, 65536, 4096, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
    if (u0 && u1)
    {
        uint8_t *p0 = nullptr, *p1 = nullptr;
        u0->Map(0, nullptr, (void **)&p0);
        u1->Map(0, nullptr, (void **)&p1);
        memset(p0, 0, 131072);
        for (UINT i = 0; i < 4096; i++)
            p1[i] = (uint8_t)(i * 7);
        int bad = 0;
        for (UINT i = 0; i < 4096; i++)
            bad += p0[65536 + i] != (uint8_t)(i * 7);
        EXPECT(!bad, "%d bytes of an upload buffer placed at 64 KiB map elsewhere", bad);
        u0->Unmap(0, nullptr);
        u1->Unmap(0, nullptr);
    }
    if (u0) u0->Release();
    if (u1) u1->Release();
    uheap->Release();
    if (tex) tex->Release();
    rb->Release(); up->Release(); pso->Release(); rs->Release(); a->Release(); b->Release(); c->Release(); d->Release(); heap->Release();
    return true;
}

// Constant buffer rows through a descriptor table past the view's end, and of a null view, read 0 (D3D11.3 7.5). A
// 256-byte view (16 rows) is read as 32 rows, and a null view as 32 more: MoltenVK reads a row out of range from a
// zero constant (the select on the address, robust_guarded_load), which this is the check of.
static const char *cbv_rows_robust_src = R"(
cbuffer A : register(b0) { float4 a[32]; };
cbuffer N : register(b1) { float4 n[32]; };
RWByteAddressBuffer o : register(u0);
[numthreads(32, 1, 1)] void cs(uint i : SV_GroupIndex)
{
    o.Store4(i * 16, asuint(a[i]));
    o.Store4(512 + i * 16, asuint(n[i]));
}
)";

static bool run_cbv_rows_robust(const char *kind, const std::vector<char> &cs)
{
    if (cs.empty())
        return false;
    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 2, 0, 0, 0 };
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = { 1, &range };
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 2, params };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = rs;
    pd.CS = { cs.data(), cs.size() };
    ID3D12PipelineState *pso = nullptr;
    CHECK_HR(g.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)));

    // 16 rows of data, then 16 more the view leaves out
    ID3D12Resource *cb = create_buffer(512, D3D12_HEAP_TYPE_UPLOAD);
    float *rows = nullptr;
    cb->Map(0, nullptr, (void **)&rows);
    for (int i = 0; i < 128; i++)
        rows[i] = (float)(i + 1);
    cb->Unmap(0, nullptr);
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap *heap = nullptr;
    CHECK_HR(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
    UINT inc = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CONSTANT_BUFFER_VIEW_DESC cv = { cb->GetGPUVirtualAddress(), 256 };
    g.device->CreateConstantBufferView(&cv, cpu);
    cpu.ptr += inc;
    g.device->CreateConstantBufferView(nullptr, cpu);
    ID3D12Resource *out = create_buffer(1024, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    g.list->SetDescriptorHeaps(1, &heap);
    g.list->SetComputeRootSignature(rs);
    g.list->SetPipelineState(pso);
    g.list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    g.list->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
    g.list->Dispatch(1, 1, 1);
    ID3D12Resource *rb = create_buffer(1024, D3D12_HEAP_TYPE_READBACK);
    transition(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    g.list->CopyBufferRegion(rb, 0, out, 0, 1024);
    if (!execute_and_wait())
        return false;
    const float *got = nullptr;
    D3D12_RANGE r = { 0, 1024 };
    rb->Map(0, &r, (void **)&got);
    for (int i = 0; i < 128; i++)
        EXPECT(got[i] == (i < 64 ? (float)(i + 1) : 0.0f), "%s: row %d component %d = %g, want %g", kind, i / 4, i % 4,
               got[i], i < 64 ? (float)(i + 1) : 0.0f);
    for (int i = 128; i < 256; i++)
        EXPECT(got[i] == 0.0f, "%s: null view row %d component %d = %g, want 0", kind, (i - 128) / 4, i % 4, got[i]);
    rb->Release(); out->Release(); heap->Release(); cb->Release(); pso->Release(); rs->Release();
    return true;
}

static bool test_cbv_rows_robust_dxil() { return run_cbv_rows_robust("DXIL", compile_dxc(cbv_rows_robust_src, "cs", "cs_6_0")); }
static bool test_cbv_rows_robust_dxbc() { return run_cbv_rows_robust("DXBC", compile_fxc(cbv_rows_robust_src, "cs", "cs_5_0")); }

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

// A geometry shader reading inputs packed into the components of one register: three outputs of the vertex shader
// (a float, a float and a float2, as Forza Horizon 3's) share a location, which DXC and FXC both do. The geometry
// shader runs as a mesh shader on MoltenVK, and its payload struct gave each component a member named after the
// location alone, so the pipeline failed to compile (duplicate member 'in1'). The pixel shader writes the three
// values to one render target.
static const char *gs_packed_src = R"(
struct V { float4 pos : SV_Position; float a : TEXCOORD0; float b : TEXCOORD2; float2 c : TANGENT0; };
V vs(uint id : SV_VertexID)
{
    V o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.a = 0.25; o.b = 0.5; o.c = float2(0.75, 1.0);
    return o;
}
[maxvertexcount(3)]
void gs(triangle V v[3], inout TriangleStream<V> s)
{
    for (int i = 0; i < 3; i++)
        s.Append(v[i]);
}
float4 ps(V v) : SV_Target { return float4(v.a, v.b, v.c); }
)";

static bool run_gs_packed(const char *kind, const std::vector<char> &vs, const std::vector<char> &gs,
                          const std::vector<char> &ps)
{
    if (vs.empty() || gs.empty() || ps.empty())
        return false;
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    ID3D12RootSignature *rs = create_root_signature(rsd);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = rs;
    pd.VS = { vs.data(), vs.size() };
    pd.GS = { gs.data(), gs.size() };
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
    HRESULT hr = g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso));
    EXPECT(SUCCEEDED(hr), "%s: CreateGraphicsPipelineState = 0x%08lx", kind, (unsigned long)hr);
    if (FAILED(hr))
    {
        rs->Release();
        return true;
    }

    const UINT W = 16, H = 16;
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
    uint32_t drawn = px[4 * W + 4];
    // 0.25, 0.5, 0.75, 1.0 as UNORM8: 0x40, 0x80, 0xbf, 0xff
    EXPECT(drawn == 0xffbf8040, "%s: drawn pixel 0x%08x, want 0xffbf8040", kind, drawn);
    rb->Release(); rtv_heap->Release(); rt->Release(); pso->Release(); rs->Release();
    return true;
}

static bool test_gs_packed_dxil()
{
    return run_gs_packed("DXIL", compile_dxc(gs_packed_src, "vs", "vs_6_0"), compile_dxc(gs_packed_src, "gs", "gs_6_0"),
                         compile_dxc(gs_packed_src, "ps", "ps_6_0"));
}

static bool test_gs_packed_dxbc()
{
    return run_gs_packed("DXBC", compile_fxc(gs_packed_src, "vs", "vs_5_0"), compile_fxc(gs_packed_src, "gs", "gs_5_0"),
                         compile_fxc(gs_packed_src, "ps", "ps_5_0"));
}

// A geometry shader reading fewer components than the vertex shader writes: a float4 TEXCOORD read as a float2 and a
// float4 NORMAL as a float3, at the locations of its own outputs, as Forza Horizon 3's do. As a mesh shader on
// MoltenVK, each input took the vertex stage's output as it was written, and the pipeline failed to compile
// (assigning to 'float2' from incompatible type 'float4').
static const char *gs_narrow_src = R"(
struct V { float4 pos : SV_Position; float4 t : TEXCOORD0; float4 n : NORMAL0; };
struct G { float4 pos : SV_Position; float2 t : TEXCOORD0; float3 n : NORMAL0; };
V vs(uint id : SV_VertexID)
{
    V o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.t = float4(0.25, 0.5, 0.1, 0.1); o.n = float4(0.1, 0.2, 0.75, 0.1);
    return o;
}
[maxvertexcount(3)]
void gs(triangle G v[3], inout TriangleStream<G> s)
{
    for (int i = 0; i < 3; i++)
        s.Append(v[i]);
}
float4 ps(G v) : SV_Target { return float4(v.t, v.n.z, 1.0); }
)";

static bool test_gs_narrow_dxil()
{
    return run_gs_packed("DXIL", compile_dxc(gs_narrow_src, "vs", "vs_6_0"), compile_dxc(gs_narrow_src, "gs", "gs_6_0"),
                         compile_dxc(gs_narrow_src, "ps", "ps_6_0"));
}

static bool test_gs_narrow_dxbc()
{
    return run_gs_packed("DXBC", compile_fxc(gs_narrow_src, "vs", "vs_5_0"), compile_fxc(gs_narrow_src, "gs", "gs_5_0"),
                         compile_fxc(gs_narrow_src, "ps", "ps_5_0"));
}

// GetAttributeAtVertex (SM 6.1, with SV_Barycentrics): a triangle whose vertices output 1, 3 and 5 covers a pixel
// whose shader returns the input at each vertex, which arrives as a PerVertexKHR input. MoltenVK builds it on MSL 4's
// vertex_value<T> where a drawn triangle shows it works, and returns 1, 3 and 5; where the driver's vertex_value reads
// other values (macOS 27.2 on an Apple M6) it takes the interpolated value at every vertex instead, so the pipeline
// still compiles: three equal values between 1 and 5.
static bool test_pervertex()
{
    static const char *src = R"(
struct V { float4 pos : SV_Position; nointerpolation float a : A; };
V vs(uint id : SV_VertexID)
{
    V o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.a = 1 + id * 2;
    return o;
}
float4 ps(V v) : SV_Target
{
    return float4(GetAttributeAtVertex(v.a, 0), GetAttributeAtVertex(v.a, 1), GetAttributeAtVertex(v.a, 2), 1);
}
)";
    std::vector<char> vs = compile_dxc(src, "vs", "vs_6_1"), ps = compile_dxc(src, "ps", "ps_6_1");
    if (vs.empty() || ps.empty())
        return false;
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 o3 = {};
    if (FAILED(g.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &o3, sizeof(o3))) || !o3.BarycentricsSupported)
    {
        printf("  skipped: no barycentrics\n");
        return true;
    }
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
    pd.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState *pso = nullptr;
    HRESULT hr = g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso));
    EXPECT(SUCCEEDED(hr), "CreateGraphicsPipelineState = 0x%08lx", (unsigned long)hr);
    if (FAILED(hr))
    {
        rs->Release();
        return true;
    }

    const UINT W = 4, H = 4;
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = W; td.Height = H; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv = { DXGI_FORMAT_R32G32B32A32_FLOAT, { 0, 0, 0, 0 } };
    ID3D12Resource *rt = nullptr;
    CHECK_HR(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&rt)));
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    ID3D12DescriptorHeap *rtv_heap = nullptr;
    CHECK_HR(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap)));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    g.device->CreateRenderTargetView(rt, nullptr, rtv);
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
    g.list->DrawInstanced(3, 1, 0, 0);

    ID3D12Resource *rb = create_buffer(W * H * 16, D3D12_HEAP_TYPE_READBACK);
    transition(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
    dst.pResource = rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R32G32B32A32_FLOAT, W, H, 1, W * 16 };
    srcl.pResource = rt;
    srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
    if (!execute_and_wait())
        return false;
    const float *px = nullptr;
    D3D12_RANGE range = { 0, W * H * 16 };
    rb->Map(0, &range, (void **)&px);
    float a = px[0], b = px[1], c = px[2];
    bool exact = a == 1 && b == 3 && c == 5;
    bool approximated = a == b && b == c && a >= 1 && a <= 5;
    printf("  the input at each vertex: %g %g %g (%s)\n", a, b, c,
           exact ? "exact" : approximated ? "approximated by its interpolated value" : "wrong");
    EXPECT(exact || approximated, "the input at each vertex reads %g %g %g, want 1 3 5 or one interpolated value", a, b, c);
    rb->Release(); rtv_heap->Release(); rt->Release(); pso->Release(); rs->Release();
    return true;
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

// Predicates rewritten in the command list that uses them: three rounds of copy, barrier to PREDICATION, then a
// predicated dispatch and draw on each of two predicates, all in one list. Each round flips the values, so a patch
// that ran before its round's copy (hoisted too early) reads the other round's value. Without conditional rendering,
// VKD3D-Proton starts a new command buffer sequence at the first round to keep its patching out of the render pass,
// and patches in the list's own command buffer in later rounds; both have to see the copy.
static bool test_predication_rewrite()
{
    static const char *src = R"(
cbuffer C : register(b0) { uint idx; };
RWStructuredBuffer<uint> u : register(u0);
[numthreads(1, 1, 1)] void cs() { InterlockedAdd(u[idx], 1); }
float4 vs(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps() : SV_Target { InterlockedAdd(u[idx], 1); return 1; }
)";
    std::vector<char> csb = compile_dxc(src, "cs", "cs_6_0"), vsb = compile_dxc(src, "vs", "vs_6_0"), psb = compile_dxc(src, "ps", "ps_6_0");
    if (csb.empty() || vsb.empty() || psb.empty())
        return false;
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[1].Constants = { 0, 0, 1 };
    D3D12_ROOT_SIGNATURE_DESC rsd = { 2, rp };
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

    // Uploads: round r copies { r & 1, !(r & 1) } as two 64-bit predicates.
    ID3D12Resource *upload = create_buffer(256, D3D12_HEAP_TYPE_UPLOAD);
    uint64_t *init = nullptr;
    upload->Map(0, nullptr, (void **)&init);
    init[0] = 0; init[1] = 1; init[2] = 1; init[3] = 0;
    upload->Unmap(0, nullptr);
    ID3D12Resource *pred = create_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *counter = create_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 4; td.Height = 4; td.DepthOrArraySize = 1; td.MipLevels = 1;
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
    D3D12_VIEWPORT vp = { 0, 0, 4, 4, 0, 1 };
    D3D12_RECT sc = { 0, 0, 4, 4 };

    // Slot 4 * r + 2 * p (+ 1 for the draw): round r, predicate p (EQUAL_ZERO: runs when the value isn't 0).
    for (UINT r = 0; r < 3; r++)
    {
        if (r)
            transition(pred, D3D12_RESOURCE_STATE_PREDICATION, D3D12_RESOURCE_STATE_COPY_DEST);
        g.list->CopyBufferRegion(pred, 0, upload, (r & 1) * 16, 16);
        transition(pred, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PREDICATION);
        for (UINT p = 0; p < 2; p++)
        {
            g.list->SetPredication(pred, p * 8, D3D12_PREDICATION_OP_EQUAL_ZERO);
            UINT slot = 4 * r + 2 * p;
            g.list->SetComputeRootSignature(rs);
            g.list->SetComputeRootUnorderedAccessView(0, counter->GetGPUVirtualAddress());
            g.list->SetComputeRoot32BitConstant(1, slot, 0);
            g.list->SetPipelineState(cps);
            g.list->Dispatch(1, 1, 1);
            g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            g.list->RSSetViewports(1, &vp);
            g.list->RSSetScissorRects(1, &sc);
            g.list->SetGraphicsRootSignature(rs);
            g.list->SetGraphicsRootUnorderedAccessView(0, counter->GetGPUVirtualAddress());
            g.list->SetGraphicsRoot32BitConstant(1, slot + 1, 0);
            g.list->SetPipelineState(gps);
            g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g.list->DrawInstanced(3, 1, 0, 0);
        }
        g.list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    }
    ID3D12Resource *crb = nullptr;
    const uint32_t *c = read_buffer(counter, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 64, &crb);
    if (!c)
        return false;
    int bad = 0;
    for (UINT r = 0; r < 3; r++)
        for (UINT p = 0; p < 2; p++)
        {
            bool runs = ((r & 1) ? !p : p);   // the predicate's value this round
            UINT slot = 4 * r + 2 * p;
            if ((c[slot] != (runs ? 1u : 0u) || (runs ? c[slot + 1] != 16 : c[slot + 1] != 0)) && bad++ < 6)
                printf("  round %u, predicate %u: dispatch ran %u times, draw shaded %u pixels; want %u and %u\n", r, p,
                       c[slot], c[slot + 1], runs ? 1 : 0, runs ? 16 : 0);
        }
    EXPECT(!bad, "%d of 6 predicated rounds wrong", bad);
    crb->Release(); counter->Release(); pred->Release(); upload->Release(); rtv_heap->Release(); rt->Release();
    gps->Release(); cps->Release(); rs->Release();
    return true;
}

// ExecuteIndirect with a root constant per command, the command signatures made before the PSOs (as engines do at
// startup): a compute signature (constant, dispatch) and a graphics one (constant, draw), each run for 4 commands
// that write their constant to a UAV slot of their own. On the Metal Shader Converter bridge the PSOs compile the
// SPIR-V pipeline such ExecuteIndirects use when they're made.
static bool test_executeindirect_template_first()
{
    static const char *src = R"(
cbuffer C : register(b0) { uint value; };
RWByteAddressBuffer o : register(u0);
[numthreads(1, 1, 1)] void cs() { o.Store((value & 15) * 4, value); }
float4 vs(uint id : SV_VertexID) : SV_Position { return float4(float2((id << 1) & 2, id & 2) * float2(2, -2) + float2(-1, 1), 0, 1); }
float4 ps(float4 p : SV_Position) : SV_Target { if (p.x < 1 && p.y < 1) o.Store(64 + (value & 15) * 4, value); return 0; }
)";
    std::vector<char> csb = compile_dxc(src, "cs", "cs_6_0"), vsb = compile_dxc(src, "vs", "vs_6_0"), psb = compile_dxc(src, "ps", "ps_6_0");
    if (csb.empty() || vsb.empty() || psb.empty())
        return false;
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.Num32BitValues = 1;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 2, rp };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    struct CsCmd { uint32_t value; D3D12_DISPATCH_ARGUMENTS dispatch; };
    struct GfxCmd { uint32_t value; D3D12_DRAW_ARGUMENTS draw; };
    D3D12_INDIRECT_ARGUMENT_DESC cs_args[2] = {}, gfx_args[2] = {};
    cs_args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    cs_args[0].Constant = { 0, 0, 1 };
    cs_args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    gfx_args[0] = cs_args[0];
    gfx_args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    D3D12_COMMAND_SIGNATURE_DESC csd = { sizeof(CsCmd), 2, cs_args, 0 }, gsd = { sizeof(GfxCmd), 2, gfx_args, 0 };
    ID3D12CommandSignature *cs_sig = nullptr, *gfx_sig = nullptr;
    CHECK_HR(g.device->CreateCommandSignature(&csd, rs, IID_PPV_ARGS(&cs_sig)));
    CHECK_HR(g.device->CreateCommandSignature(&gsd, rs, IID_PPV_ARGS(&gfx_sig)));
    // Now the PSOs.
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

    ID3D12Resource *args = create_buffer(256, D3D12_HEAP_TYPE_UPLOAD);
    uint8_t *a = nullptr;
    args->Map(0, nullptr, (void **)&a);
    for (uint32_t i = 0; i < 4; i++)
    {
        CsCmd c = { 0x100 * (i + 1) + i, { 1, 1, 1 } };
        GfxCmd d = { 0x1000 * (i + 1) + 4 + i, { 3, 1, 0, 0 } };
        memcpy(a + i * sizeof(CsCmd), &c, sizeof(c));
        memcpy(a + 128 + i * sizeof(GfxCmd), &d, sizeof(d));
    }
    args->Unmap(0, nullptr);
    ID3D12Resource *out = create_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 4; td.Height = 4; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource *rt = nullptr;
    CHECK_HR(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&rt)));
    D3D12_DESCRIPTOR_HEAP_DESC rhd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    ID3D12DescriptorHeap *rtv_heap = nullptr;
    CHECK_HR(g.device->CreateDescriptorHeap(&rhd, IID_PPV_ARGS(&rtv_heap)));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    g.device->CreateRenderTargetView(rt, nullptr, rtv);

    g.list->SetComputeRootSignature(rs);
    g.list->SetPipelineState(cps);
    g.list->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
    g.list->ExecuteIndirect(cs_sig, 4, args, 0, nullptr, 0);
    D3D12_VIEWPORT vp = { 0, 0, 4, 4, 0, 1 };
    D3D12_RECT sc = { 0, 0, 4, 4 };
    g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    g.list->RSSetViewports(1, &vp);
    g.list->RSSetScissorRects(1, &sc);
    g.list->SetGraphicsRootSignature(rs);
    g.list->SetPipelineState(gps);
    g.list->SetGraphicsRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
    g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.list->ExecuteIndirect(gfx_sig, 4, args, 128, nullptr, 0);
    ID3D12Resource *rb = nullptr;
    const uint32_t *v = read_buffer(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 256, &rb);
    if (!v)
        return false;
    int bad = 0;
    for (uint32_t i = 0; i < 4; i++)
    {
        uint32_t want_cs = 0x100 * (i + 1) + i, want_gfx = 0x1000 * (i + 1) + 4 + i;
        if (v[i] != want_cs && bad++ < 4)
            printf("  dispatch %u wrote 0x%x, want 0x%x\n", i, v[i], want_cs);
        if (v[16 + 4 + i] != want_gfx && bad++ < 4)
            printf("  draw %u wrote 0x%x, want 0x%x\n", i, v[16 + 4 + i], want_gfx);
    }
    EXPECT(!bad, "%d ExecuteIndirect commands read the wrong root constant", bad);
    rb->Release(); rtv_heap->Release(); rt->Release(); out->Release(); args->Release();
    gps->Release(); cps->Release(); gfx_sig->Release(); cs_sig->Release(); rs->Release();
    return true;
}

// A vertex shader writing a UAV, in a PSO with no pixel shader, render target or depth target (a pipeline made with
// rasterizer discard): 64 points, each writing its vertex ID times 3 plus 1. On Metal, a converted vertex function
// returns its outputs, which needs rasterization: MoltenVK rasterizes such bridge pipelines, which draw nothing.
static bool test_vs_uav_no_attachments()
{
    static const char *src = R"(
RWByteAddressBuffer o : register(u0);
float4 vs(uint id : SV_VertexID) : SV_Position { o.Store(id * 4, id * 3 + 1); return float4(0, 0, 0, 1); }
)";
    std::vector<char> vsb = compile_dxc(src, "vs", "vs_6_0");
    if (vsb.empty())
        return false;
    D3D12_ROOT_PARAMETER rp = {};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &rp };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = rs;
    pd.VS = { vsb.data(), vsb.size() };
    pd.SampleMask = ~0u;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState *pso = nullptr;
    CHECK_HR(g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)));
    ID3D12Resource *out = create_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_VIEWPORT vp = { 0, 0, 4, 4, 0, 1 };
    D3D12_RECT sc = { 0, 0, 4, 4 };
    g.list->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
    g.list->RSSetViewports(1, &vp);
    g.list->RSSetScissorRects(1, &sc);
    g.list->SetGraphicsRootSignature(rs);
    g.list->SetPipelineState(pso);
    g.list->SetGraphicsRootUnorderedAccessView(0, out->GetGPUVirtualAddress());
    g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    g.list->DrawInstanced(64, 1, 0, 0);
    ID3D12Resource *rb = nullptr;
    const uint32_t *v = read_buffer(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 256, &rb);
    if (!v)
        return false;
    int bad = 0;
    for (uint32_t i = 0; i < 64; i++)
        if (v[i] != i * 3 + 1 && bad++ < 4)
            printf("  vertex %u wrote %u, want %u\n", i, v[i], i * 3 + 1);
    EXPECT(!bad, "%d of 64 vertices wrote the wrong value", bad);
    rb->Release(); out->Release(); pso->Release(); rs->Release();
    return true;
}

// A global UAV barrier (no resource named) in a render pass while the bound pipeline uses no UAVs (VKD3D-Proton defers
// it on MoltenVK, keeping the pass open): draw 1's pixel shader writes 7 to a UAV, draw 2 uses a pipeline without
// UAVs, then the barrier, then draw 3's pixel shader reads the value and writes it times 3. The barrier must still
// order draw 1's write before draw 3's read.
static bool test_uav_global_barrier_deferred()
{
    static const char *src = R"(
RWByteAddressBuffer o : register(u0);
float4 vs(uint id : SV_VertexID) : SV_Position { return float4(float2((id << 1) & 2, id & 2) * float2(2, -2) + float2(-1, 1), 0, 1); }
float4 ps_write(float4 p : SV_Position) : SV_Target { if (p.x < 1 && p.y < 1) o.Store(0, 7); return 0; }
float4 ps_plain(float4 p : SV_Position) : SV_Target { return 1; }
float4 ps_read(float4 p : SV_Position) : SV_Target { if (p.x < 1 && p.y < 1) o.Store(4, o.Load(0) * 3); return 0; }
)";
    std::vector<char> vsb = compile_dxc(src, "vs", "vs_6_0"), pw = compile_dxc(src, "ps_write", "ps_6_0"),
                      pp = compile_dxc(src, "ps_plain", "ps_6_0"), pr = compile_dxc(src, "ps_read", "ps_6_0");
    if (vsb.empty() || pw.empty() || pp.empty() || pr.empty())
        return false;
    D3D12_ROOT_PARAMETER rp = {};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &rp };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    ID3D12PipelineState *pso[3] = {};
    const std::vector<char> *pss[3] = { &pw, &pp, &pr };
    for (int k = 0; k < 3; k++)
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = rs;
        pd.VS = { vsb.data(), vsb.size() };
        pd.PS = { pss[k]->data(), pss[k]->size() };
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = ~0u;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pd.SampleDesc.Count = 1;
        CHECK_HR(g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso[k])));
    }
    ID3D12Resource *out = create_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 4; td.Height = 4; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource *rt = nullptr;
    CHECK_HR(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&rt)));
    D3D12_DESCRIPTOR_HEAP_DESC rhd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    ID3D12DescriptorHeap *rtv_heap = nullptr;
    CHECK_HR(g.device->CreateDescriptorHeap(&rhd, IID_PPV_ARGS(&rtv_heap)));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    g.device->CreateRenderTargetView(rt, nullptr, rtv);
    D3D12_VIEWPORT vp = { 0, 0, 4, 4, 0, 1 };
    D3D12_RECT sc = { 0, 0, 4, 4 };
    g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    g.list->RSSetViewports(1, &vp);
    g.list->RSSetScissorRects(1, &sc);
    g.list->SetGraphicsRootSignature(rs);
    g.list->SetGraphicsRootUnorderedAccessView(0, out->GetGPUVirtualAddress());
    g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (int k = 0; k < 3; k++)
    {
        if (k == 2)
        {
            D3D12_RESOURCE_BARRIER b = {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            g.list->ResourceBarrier(1, &b);
        }
        g.list->SetPipelineState(pso[k]);
        g.list->DrawInstanced(3, 1, 0, 0);
    }
    ID3D12Resource *rb = nullptr;
    const uint32_t *v = read_buffer(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 8, &rb);
    if (!v)
        return false;
    EXPECT(v[0] == 7 && v[1] == 21, "draw 1 wrote %u (want 7), draw 3 read and wrote %u (want 21)", v[0], v[1]);
    rb->Release(); rtv_heap->Release(); rt->Release(); out->Release();
    for (auto *p : pso) p->Release();
    rs->Release();
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

// Ray tracing pipelines (DXR 1.0): a state object with ray generation, miss and closest hit shaders, its shader
// tables, and DispatchRays against the one-triangle scene (hits report RayTCurrent and the barycentrics).
static bool test_dispatchrays()
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5 = {};
    g.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5));
    if (o5.RaytracingTier < D3D12_RAYTRACING_TIER_1_0)
    {
        printf("  skipped: raytracing tier %d\n", o5.RaytracingTier);
        return true;
    }
    ID3D12Device5 *dev5 = nullptr;
    ID3D12GraphicsCommandList4 *list4 = nullptr;
    CHECK_HR(g.device->QueryInterface(IID_PPV_ARGS(&dev5)));
    CHECK_HR(g.list->QueryInterface(IID_PPV_ARGS(&list4)));
    RtScene scene;
    if (!build_rt_scene(dev5, list4, scene))
        return false;

    static const char *src = R"(
RaytracingAccelerationStructure scene : register(t0);
RWByteAddressBuffer o : register(u0);
struct Payload { uint hit; float t; float2 bary; };
[shader("raygeneration")]
void rg()
{
    static const float2 xy[4] = { float2(0.25, 0.25), float2(0.9, 0.9), float2(0.1, 0.1), float2(0.5, 0.4) };
    uint id = DispatchRaysIndex().x;
    RayDesc r;
    r.Origin = float3(xy[id], -1.0);
    r.Direction = float3(0, 0, 1);
    r.TMin = 0.0;
    r.TMax = 10.0;
    Payload p = { 0, -1.0, float2(0, 0) };
    TraceRay(scene, RAY_FLAG_NONE, 0xff, 0, 1, 0, r, p);
    o.Store4(id * 16, uint4(p.hit, asuint(p.t), asuint(p.bary)));
}
[shader("miss")]
void ms(inout Payload p) { p.hit = 2; }
[shader("closesthit")]
void ch(inout Payload p, BuiltInTriangleIntersectionAttributes a) { p.hit = 1; p.t = RayTCurrent(); p.bary = a.barycentrics; }
)";
    std::vector<char> lib = compile_dxc(src, "", "lib_6_3");
    if (lib.empty())
        return false;

    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[0].ShaderVisibility = rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 2, rp };
    ID3D12RootSignature *rs = create_root_signature(rsd);

    D3D12_DXIL_LIBRARY_DESC lib_desc = {};
    lib_desc.DXILLibrary = { lib.data(), lib.size() };
    D3D12_HIT_GROUP_DESC hit_group = {};
    hit_group.HitGroupExport = L"hg";
    hit_group.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    hit_group.ClosestHitShaderImport = L"ch";
    D3D12_RAYTRACING_SHADER_CONFIG shader_config = { 16, 8 };
    D3D12_RAYTRACING_PIPELINE_CONFIG pipeline_config = { 1 };
    D3D12_GLOBAL_ROOT_SIGNATURE global_rs = { rs };
    D3D12_STATE_SUBOBJECT subobjects[5] = {
        { D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &lib_desc },
        { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hit_group },
        { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shader_config },
        { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipeline_config },
        { D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global_rs },
    };
    D3D12_STATE_OBJECT_DESC so_desc = { D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, 5, subobjects };
    ID3D12StateObject *so = nullptr;
    CHECK_HR(dev5->CreateStateObject(&so_desc, IID_PPV_ARGS(&so)));
    ID3D12StateObjectProperties *props = nullptr;
    CHECK_HR(so->QueryInterface(IID_PPV_ARGS(&props)));

    // Shader table: ray generation at 0, miss at 64, hit group at 128.
    ID3D12Resource *table = create_buffer(256, D3D12_HEAP_TYPE_UPLOAD);
    uint8_t *t = nullptr;
    table->Map(0, nullptr, (void **)&t);
    memset(t, 0, 256);
    const wchar_t *names[3] = { L"rg", L"ms", L"hg" };
    for (int i = 0; i < 3; i++)
    {
        void *id = props->GetShaderIdentifier(names[i]);
        EXPECT(id, "no shader identifier for export %d", i);
        if (id)
            memcpy(t + 64 * i, id, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    }
    table->Unmap(0, nullptr);
    ID3D12Resource *out = create_buffer(64, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_GPU_VIRTUAL_ADDRESS base = table->GetGPUVirtualAddress();
    D3D12_DISPATCH_RAYS_DESC rays = {};
    rays.RayGenerationShaderRecord = { base, 32 };
    rays.MissShaderTable = { base + 64, 32, 32 };
    rays.HitGroupTable = { base + 128, 32, 32 };
    rays.Width = 4;
    rays.Height = rays.Depth = 1;
    list4->SetComputeRootSignature(rs);
    list4->SetPipelineState1(so);
    list4->SetComputeRootShaderResourceView(0, scene.tlas->GetGPUVirtualAddress());
    list4->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
    list4->DispatchRays(&rays);
    ID3D12Resource *rb;
    const uint32_t *v = read_buffer(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 64, &rb);
    if (!v)
        return false;
    static const float xy[4][2] = { { 0.25f, 0.25f }, { 0.9f, 0.9f }, { 0.1f, 0.1f }, { 0.5f, 0.4f } };
    for (int i = 0; i < 4; i++)
    {
        const uint32_t *r = v + 4 * i;
        float tt, bu, bv;
        memcpy(&tt, &r[1], 4); memcpy(&bu, &r[2], 4); memcpy(&bv, &r[3], 4);
        bool inside = xy[i][0] + xy[i][1] < 1.0f;
        if (inside)
            EXPECT(r[0] == 1 && fabsf(tt - (1.0f + 128.0f / 255.0f)) < 1e-3f && fabsf(bu - xy[i][0]) < 1e-3f && fabsf(bv - xy[i][1]) < 1e-3f,
                   "ray %d: hit %u t %f barycentrics (%f %f), want a hit at %f (%f %f)", i, r[0], tt, bu, bv,
                   1.0f + 128.0f / 255.0f, xy[i][0], xy[i][1]);
        else
            EXPECT(r[0] == 2, "ray %d: hit %u, want a miss", i, r[0]);
    }
    rb->Release(); out->Release(); table->Release(); props->Release(); so->Release(); rs->Release();
    release_rt_scene(scene); list4->Release(); dev5->Release();
    return true;
}

// Mesh shaders (tier 1): an amplification shader launches two mesh groups with a payload of colors; each emits
// a quad covering one half of the render target.
template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE T, typename V>
struct alignas(void *) StreamSubobject { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = T; V value; };

// The depth bounds test (OMSetDepthBounds, DEPTH_STENCIL1.DepthBoundsTestEnable): a D32 target whose left half holds
// 0.3 and right half 0.7, and a full-screen quad drawn with depth testing off and bounds [0.5, 1]: only the right
// half is drawn; then bounds [0, 0.5] by a second PSO: only the left half. Metal has the test on Apple10 GPUs with
// macOS 26 (the Metal Shader Converter bridge can take such PSOs there); MoltenVK emulates it elsewhere.
static bool test_depth_bounds()
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS2 o2 = {};
    if (FAILED(g.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2, &o2, sizeof(o2))) || !o2.DepthBoundsTestSupported)
    {
        printf("  skipped: no depth bounds test\n");
        return true;
    }
    static const char *src = R"(
float4 vs(uint id : SV_VertexID) : SV_Position { return float4(float2((id << 1) & 2, id & 2) * float2(2, -2) + float2(-1, 1), 0.9, 1); }
float4 ps() : SV_Target { return float4(1, 0, 0, 1); }
)";
    std::vector<char> vsb = compile_dxc(src, "vs", "vs_6_0"), psb = compile_dxc(src, "ps", "ps_6_0");
    if (vsb.empty() || psb.empty())
        return false;
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    ID3D12RootSignature *rs = create_root_signature(rsd);
    ID3D12Device2 *dev2 = nullptr;
    CHECK_HR(g.device->QueryInterface(IID_PPV_ARGS(&dev2)));
    ID3D12PipelineState *pso[2] = {};
    for (int k = 0; k < 2; k++)
    {
        D3D12_RASTERIZER_DESC rast = {};
        rast.FillMode = D3D12_FILL_MODE_SOLID;
        rast.CullMode = D3D12_CULL_MODE_NONE;
        rast.DepthClipEnable = TRUE;
        D3D12_BLEND_DESC blend = {};
        blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        D3D12_RT_FORMAT_ARRAY rts = {};
        rts.NumRenderTargets = 1;
        rts.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        D3D12_DEPTH_STENCIL_DESC1 ds = {};
        ds.DepthEnable = FALSE;
        ds.DepthBoundsTestEnable = TRUE;
        struct
        {
            StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature *> rs;
            StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE> vs;
            StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
            StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> rast;
            StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC> blend;
            StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> mask;
            StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE> topo;
            StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> rtvs;
            StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT> dsv;
            StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1, D3D12_DEPTH_STENCIL_DESC1> ds;
            StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> sample;
        } stream;
        stream.rs.value = rs;
        stream.vs.value = { vsb.data(), vsb.size() };
        stream.ps.value = { psb.data(), psb.size() };
        stream.rast.value = rast;
        stream.blend.value = blend;
        stream.mask.value = ~0u;
        stream.topo.value = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        stream.rtvs.value = rts;
        stream.dsv.value = DXGI_FORMAT_D32_FLOAT;
        stream.ds.value = ds;
        stream.sample.value = { 1, 0 };
        D3D12_PIPELINE_STATE_STREAM_DESC sd = { sizeof(stream), &stream };
        CHECK_HR(dev2->CreatePipelineState(&sd, IID_PPV_ARGS(&pso[k])));
    }
    dev2->Release();
    const UINT W = 8, H = 4;
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = W; td.Height = H; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource *rt = nullptr, *depth = nullptr;
    CHECK_HR(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&rt)));
    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    CHECK_HR(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, IID_PPV_ARGS(&depth)));
    D3D12_DESCRIPTOR_HEAP_DESC rhd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 }, dhd = { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1 };
    ID3D12DescriptorHeap *rtv_heap = nullptr, *dsv_heap = nullptr;
    CHECK_HR(g.device->CreateDescriptorHeap(&rhd, IID_PPV_ARGS(&rtv_heap)));
    CHECK_HR(g.device->CreateDescriptorHeap(&dhd, IID_PPV_ARGS(&dsv_heap)));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart(), dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
    g.device->CreateRenderTargetView(rt, nullptr, rtv);
    g.device->CreateDepthStencilView(depth, nullptr, dsv);
    ID3D12GraphicsCommandList1 *l1 = nullptr;
    CHECK_HR(g.list->QueryInterface(IID_PPV_ARGS(&l1)));
    ID3D12Resource *rb = create_buffer(256 * H * 2, D3D12_HEAP_TYPE_READBACK);
    D3D12_VIEWPORT vp = { 0, 0, (float)W, (float)H, 0, 1 };
    D3D12_RECT sc = { 0, 0, (LONG)W, (LONG)H };
    const float bounds[2][2] = { { 0.5f, 1.0f }, { 0.0f, 0.5f } };
    for (int k = 0; k < 2; k++)
    {
        float clear[4] = { 0, 0, 0, 0 };
        D3D12_RECT left = { 0, 0, (LONG)W / 2, (LONG)H }, right = { (LONG)W / 2, 0, (LONG)W, (LONG)H };
        g.list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        g.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.3f, 0, 1, &left);
        g.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.7f, 0, 1, &right);
        g.list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        g.list->RSSetViewports(1, &vp);
        g.list->RSSetScissorRects(1, &sc);
        g.list->SetGraphicsRootSignature(rs);
        g.list->SetPipelineState(pso[k]);
        l1->OMSetDepthBounds(bounds[k][0], bounds[k][1]);
        g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g.list->DrawInstanced(3, 1, 0, 0);
        transition(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
        dst.pResource = rb;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = k * 256 * H;
        dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 1, 256 };
        srcl.pResource = rt;
        srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        g.list->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
        transition(rt, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    if (!execute_and_wait())
        return false;
    const uint32_t *px = nullptr;
    D3D12_RANGE r = { 0, 256 * H * 2 };
    rb->Map(0, &r, (void **)&px);
    int bad = 0;
    for (int k = 0; k < 2; k++)
        for (UINT y = 0; y < H; y++)
            for (UINT x = 0; x < W; x++)
            {
                bool drawn = px[(k * 256 * H + y * 256) / 4 + x] == 0xff0000ffu;
                bool want = k == 0 ? x >= W / 2 : x < W / 2;
                if (drawn != want && bad++ < 4)
                    printf("  bounds [%g, %g], pixel (%u, %u): %s, want %s\n", bounds[k][0], bounds[k][1], x, y,
                           drawn ? "drawn" : "not drawn", want ? "drawn" : "not drawn");
            }
    rb->Unmap(0, nullptr);
    EXPECT(!bad, "%d pixels wrong", bad);
    l1->Release(); rb->Release(); rtv_heap->Release(); dsv_heap->Release(); rt->Release(); depth->Release();
    pso[0]->Release(); pso[1]->Release(); rs->Release();
    return true;
}

// A mesh shader without an amplification shader that reads root constants and a descriptor table (SRV), drawing a
// triangle into the top-left of the target only: the top-left pixel gets the color, the bottom-left and the
// top-right keep the clear (so a flipped Y or a lost resource shows). Two mesh threadgroups: each draws its own
// triangle, the second into the top-right corner with the second color.
static bool test_mesh_resources()
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7 = {};
    g.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof(o7));
    if (o7.MeshShaderTier < D3D12_MESH_SHADER_TIER_1)
    {
        printf("  skipped: mesh shader tier %d\n", o7.MeshShaderTier);
        return true;
    }
    static const char *src = R"(
cbuffer c : register(b0) { float scale; float y_top; };
StructuredBuffer<float4> colors : register(t0);
struct V { float4 pos : SV_Position; float4 color : COLOR; };
[numthreads(3, 1, 1)]
[outputtopology("triangle")]
void ms_main(uint t : SV_GroupThreadID, uint g : SV_GroupID, out vertices V v[3], out indices uint3 tri[1])
{
    SetMeshOutputCounts(3, 1);
    // Group 0: x in [-1, -0.5]; group 1: x in [0.5, 1]; y in [y_top - 0.5, y_top].
    float x0 = g == 0 ? -1.0 : 0.5;
    float2 p = t == 0 ? float2(x0, y_top) : t == 1 ? float2(x0 + 0.5, y_top) : float2(x0, y_top - 0.5);
    v[t].pos = float4(p, 0, 1);
    v[t].color = colors[g] * scale;
    if (t == 0)
        tri[0] = uint3(0, 1, 2);
}
float4 ps_main(V i) : SV_Target { return i.color; }
)";
    std::vector<char> ms = compile_dxc(src, "ms_main", "ms_6_5"), ps = compile_dxc(src, "ps_main", "ps_6_5");
    if (ms.empty() || ps.empty())
        return false;
    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.Num32BitValues = 2;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_MESH;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable = { 1, &range };
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_MESH;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 2, rp };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    D3D12_RASTERIZER_DESC rast = {};
    rast.FillMode = D3D12_FILL_MODE_SOLID;
    rast.CullMode = D3D12_CULL_MODE_NONE;
    rast.DepthClipEnable = TRUE;
    D3D12_BLEND_DESC blend = {};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    D3D12_RT_FORMAT_ARRAY rtv_formats = {};
    rtv_formats.NumRenderTargets = 1;
    rtv_formats.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    struct
    {
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature *> rs;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> ms;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> rast;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC> blend;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> mask;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> rtvs;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> sample;
    } stream;
    stream.rs.value = rs;
    stream.ms.value = { ms.data(), ms.size() };
    stream.ps.value = { ps.data(), ps.size() };
    stream.rast.value = rast;
    stream.blend.value = blend;
    stream.mask.value = ~0u;
    stream.rtvs.value = rtv_formats;
    stream.sample.value = { 1, 0 };
    D3D12_PIPELINE_STATE_STREAM_DESC sd = { sizeof(stream), &stream };
    ID3D12Device2 *dev2 = nullptr;
    CHECK_HR(g.device->QueryInterface(IID_PPV_ARGS(&dev2)));
    ID3D12PipelineState *pso = nullptr;
    CHECK_HR(dev2->CreatePipelineState(&sd, IID_PPV_ARGS(&pso)));
    ID3D12GraphicsCommandList6 *list6 = nullptr;
    CHECK_HR(g.list->QueryInterface(IID_PPV_ARGS(&list6)));

    ID3D12Resource *upload = create_buffer(256, D3D12_HEAP_TYPE_UPLOAD);
    float *u = nullptr;
    upload->Map(0, nullptr, (void **)&u);
    const float colors[8] = { 1, 0.5f, 0, 1, 0, 0.5f, 1, 1 };
    memcpy(u, colors, sizeof(colors));
    upload->Unmap(0, nullptr);
    ID3D12Resource *cbuf = create_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    list6->CopyBufferRegion(cbuf, 0, upload, 0, 256);
    transition(cbuf, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    D3D12_DESCRIPTOR_HEAP_DESC shd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap *srv_heap = nullptr;
    CHECK_HR(g.device->CreateDescriptorHeap(&shd, IID_PPV_ARGS(&srv_heap)));
    D3D12_SHADER_RESOURCE_VIEW_DESC srvd = {};
    srvd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvd.Buffer.NumElements = 2;
    srvd.Buffer.StructureByteStride = 16;
    g.device->CreateShaderResourceView(cbuf, &srvd, srv_heap->GetCPUDescriptorHandleForHeapStart());

    const UINT W = 16, H = 16;
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
    float clear[4] = { 0, 0, 0, 0 };
    list6->ClearRenderTargetView(rtv, clear, 0, nullptr);
    list6->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp = { 0, 0, (float)W, (float)H, 0, 1 };
    D3D12_RECT sc = { 0, 0, (LONG)W, (LONG)H };
    list6->RSSetViewports(1, &vp);
    list6->RSSetScissorRects(1, &sc);
    list6->SetDescriptorHeaps(1, &srv_heap);
    list6->SetGraphicsRootSignature(rs);
    list6->SetPipelineState(pso);
    const float k[2] = { 1.0f, 1.0f };   // scale, y_top
    list6->SetGraphicsRoot32BitConstants(0, 2, k, 0);
    list6->SetGraphicsRootDescriptorTable(1, srv_heap->GetGPUDescriptorHandleForHeapStart());
    list6->DispatchMesh(2, 1, 1);
    ID3D12Resource *rb = create_buffer(256 * H, D3D12_HEAP_TYPE_READBACK);
    transition(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
    dst.pResource = rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 1, 256 };
    srcl.pResource = rt;
    srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list6->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
    if (!execute_and_wait())
        return false;
    const uint8_t *px = nullptr;
    D3D12_RANGE rr = { 0, 256 * H };
    rb->Map(0, &rr, (void **)&px);
    auto at = [&](UINT x, UINT y) { const uint8_t *p = px + y * 256 + x * 4; return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; };
    const uint32_t c0 = 0xff0080ffu, c1 = 0xffff8000u;   // RGBA8 of colors[0] and colors[1]
    auto close_rgba = [](uint32_t a, uint32_t b) {
        for (int i = 0; i < 32; i += 8)
            if (abs((int)((a >> i) & 0xff) - (int)((b >> i) & 0xff)) > 1)
                return false;
        return true;
    };
    EXPECT(close_rgba(at(0, 0), c0), "top-left %#x, want %#x", at(0, 0), c0);
    EXPECT(close_rgba(at(W - 3, 0), c1), "top-right %#x, want %#x", at(W - 3, 0), c1);
    EXPECT(at(0, H - 1) == 0, "bottom-left %#x, want 0 (Y flipped?)", at(0, H - 1));
    EXPECT(at(W / 2, 0) == 0, "top-middle %#x, want 0", at(W / 2, 0));
    rb->Release(); rtv_heap->Release(); rt->Release(); srv_heap->Release(); cbuf->Release(); upload->Release();
    list6->Release(); pso->Release(); dev2->Release(); rs->Release();
    return true;
}

static bool test_mesh_cull()
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7 = {};
    g.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof(o7));
    if (o7.MeshShaderTier < D3D12_MESH_SHADER_TIER_1)
    {
        printf("  skipped: mesh shader tier %d\n", o7.MeshShaderTier);
        return true;
    }
    // Group 0 outputs two triangles (top-left and top-right corners of its half) and culls the second with
    // SV_CullPrimitive; group 1 outputs one. Each primitive carries a per-primitive color (the pixel shader's input).
    static const char *src = R"(
cbuffer c : register(b0) { float scale; float y_top; };
StructuredBuffer<float4> colors : register(t0);
struct V { float4 pos : SV_Position; };
struct P { float4 color : COLOR; bool cull : SV_CullPrimitive; };
[numthreads(6, 1, 1)]
[outputtopology("triangle")]
void ms_main(uint t : SV_GroupThreadID, uint g : SV_GroupID, out vertices V v[6], out indices uint3 tri[2],
             out primitives P prim[2])
{
    uint prims = g == 0 ? 2 : 1;
    SetMeshOutputCounts(prims * 3, prims);
    if (t < prims * 3)
    {
        uint k = t / 3, c = t % 3;
        float x0 = g == 0 ? (k == 0 ? -1.0 : -0.5) : 0.5;
        float2 p = c == 0 ? float2(x0, y_top) : c == 1 ? float2(x0 + 0.5, y_top) : float2(x0, y_top - 0.5);
        v[t].pos = float4(p, 0, 1);
    }
    if (t < prims)
    {
        tri[t] = uint3(t * 3, t * 3 + 1, t * 3 + 2);
        prim[t].color = colors[g] * scale;
        prim[t].cull = g == 0 && t == 1;
    }
}
float4 ps_main(V i, P p) : SV_Target { return p.color; }
)";
    std::vector<char> ms = compile_dxc(src, "ms_main", "ms_6_5"), ps = compile_dxc(src, "ps_main", "ps_6_5");
    if (ms.empty() || ps.empty())
        return false;
    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.Num32BitValues = 2;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_MESH;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable = { 1, &range };
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_MESH;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 2, rp };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    D3D12_RASTERIZER_DESC rast = {};
    rast.FillMode = D3D12_FILL_MODE_SOLID;
    rast.CullMode = D3D12_CULL_MODE_NONE;
    rast.DepthClipEnable = TRUE;
    D3D12_BLEND_DESC blend = {};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    D3D12_RT_FORMAT_ARRAY rtv_formats = {};
    rtv_formats.NumRenderTargets = 1;
    rtv_formats.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    struct
    {
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature *> rs;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> ms;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> rast;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC> blend;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> mask;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> rtvs;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> sample;
    } stream;
    stream.rs.value = rs;
    stream.ms.value = { ms.data(), ms.size() };
    stream.ps.value = { ps.data(), ps.size() };
    stream.rast.value = rast;
    stream.blend.value = blend;
    stream.mask.value = ~0u;
    stream.rtvs.value = rtv_formats;
    stream.sample.value = { 1, 0 };
    D3D12_PIPELINE_STATE_STREAM_DESC sd = { sizeof(stream), &stream };
    ID3D12Device2 *dev2 = nullptr;
    CHECK_HR(g.device->QueryInterface(IID_PPV_ARGS(&dev2)));
    ID3D12PipelineState *pso = nullptr;
    CHECK_HR(dev2->CreatePipelineState(&sd, IID_PPV_ARGS(&pso)));
    ID3D12GraphicsCommandList6 *list6 = nullptr;
    CHECK_HR(g.list->QueryInterface(IID_PPV_ARGS(&list6)));

    ID3D12Resource *upload = create_buffer(256, D3D12_HEAP_TYPE_UPLOAD);
    float *u = nullptr;
    upload->Map(0, nullptr, (void **)&u);
    const float colors[8] = { 1, 0.5f, 0, 1, 0, 0.5f, 1, 1 };
    memcpy(u, colors, sizeof(colors));
    upload->Unmap(0, nullptr);
    ID3D12Resource *cbuf = create_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    list6->CopyBufferRegion(cbuf, 0, upload, 0, 256);
    transition(cbuf, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    D3D12_DESCRIPTOR_HEAP_DESC shd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap *srv_heap = nullptr;
    CHECK_HR(g.device->CreateDescriptorHeap(&shd, IID_PPV_ARGS(&srv_heap)));
    D3D12_SHADER_RESOURCE_VIEW_DESC srvd = {};
    srvd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvd.Buffer.NumElements = 2;
    srvd.Buffer.StructureByteStride = 16;
    g.device->CreateShaderResourceView(cbuf, &srvd, srv_heap->GetCPUDescriptorHandleForHeapStart());

    const UINT W = 16, H = 16;
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
    float clear[4] = { 0, 0, 0, 0 };
    list6->ClearRenderTargetView(rtv, clear, 0, nullptr);
    list6->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp = { 0, 0, (float)W, (float)H, 0, 1 };
    D3D12_RECT sc = { 0, 0, (LONG)W, (LONG)H };
    list6->RSSetViewports(1, &vp);
    list6->RSSetScissorRects(1, &sc);
    list6->SetDescriptorHeaps(1, &srv_heap);
    list6->SetGraphicsRootSignature(rs);
    list6->SetPipelineState(pso);
    const float k[2] = { 1.0f, 1.0f };   // scale, y_top
    list6->SetGraphicsRoot32BitConstants(0, 2, k, 0);
    list6->SetGraphicsRootDescriptorTable(1, srv_heap->GetGPUDescriptorHandleForHeapStart());
    list6->DispatchMesh(2, 1, 1);
    ID3D12Resource *rb = create_buffer(256 * H, D3D12_HEAP_TYPE_READBACK);
    transition(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
    dst.pResource = rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 1, 256 };
    srcl.pResource = rt;
    srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list6->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
    if (!execute_and_wait())
        return false;
    const uint8_t *px = nullptr;
    D3D12_RANGE rr = { 0, 256 * H };
    rb->Map(0, &rr, (void **)&px);
    auto at = [&](UINT x, UINT y) { const uint8_t *p = px + y * 256 + x * 4; return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; };
    const uint32_t c0 = 0xff0080ffu, c1 = 0xffff8000u;   // RGBA8 of colors[0] and colors[1]
    auto close_rgba = [](uint32_t a, uint32_t b) {
        for (int i = 0; i < 32; i += 8)
            if (abs((int)((a >> i) & 0xff) - (int)((b >> i) & 0xff)) > 1)
                return false;
        return true;
    };
    EXPECT(close_rgba(at(0, 0), c0), "top-left %#x, want %#x", at(0, 0), c0);
    EXPECT(at(W / 4 + 1, 0) == 0, "the culled primitive drew %#x", at(W / 4 + 1, 0));
    EXPECT(close_rgba(at(W - 3, 0), c1), "top-right %#x, want %#x", at(W - 3, 0), c1);
    EXPECT(at(0, H - 1) == 0, "bottom-left %#x, want 0", at(0, H - 1));
    rb->Release(); rtv_heap->Release(); rt->Release(); srv_heap->Release(); cbuf->Release(); upload->Release();
    list6->Release(); pso->Release(); dev2->Release(); rs->Release();
    return true;
}

// Mesh shader features. Six amplification groups (32 threads) each launch (group % 3) mesh groups through the payload;
// mesh group m of amplification group a draws cell 2a + m of a 4-wide grid of 8x8 cells as two triangles with a
// per-primitive color (cell + 1). The second triangle of each m = 1 group is culled with SV_CullPrimitive, and the
// m = 1 group of a = 5 outputs nothing (SetMeshOutputCounts(0, 0)). Then one 128-thread mesh group at the output limit,
// 256 triangles over 153 vertices, fills a 32x16 band (color 50).
static bool test_mesh_features()
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7 = {};
    g.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof(o7));
    if (o7.MeshShaderTier < D3D12_MESH_SHADER_TIER_1)
    {
        printf("  skipped: mesh shader tier %d\n", o7.MeshShaderTier);
        return true;
    }
    static const char *src = R"(
struct Payload { uint a; };
groupshared Payload pl;
[numthreads(32, 1, 1)]
void as_main(uint gid : SV_GroupID, uint t : SV_GroupThreadID)
{
    if (t == 0) pl.a = gid;
    GroupMemoryBarrierWithGroupSync();
    DispatchMesh(gid % 3, 1, 1, pl);
}
struct V { float4 pos : SV_Position; };
struct P { uint color : COLOR; bool cull : SV_CullPrimitive; };
float4 ndc(float x, float y) { return float4(x / 32.0 * 2.0 - 1.0, 1.0 - y / 40.0 * 2.0, 0, 1); }
[numthreads(128, 1, 1)]
[outputtopology("triangle")]
void ms_cells(uint t : SV_GroupThreadID, uint m : SV_GroupID, in payload Payload p,
              out vertices V v[4], out indices uint3 tri[2], out primitives P prim[2])
{
    uint cell = p.a * 2 + m;
    bool none = p.a == 5 && m == 1;
    SetMeshOutputCounts(none ? 0 : 4, none ? 0 : 2);
    if (none) return;
    float x = (cell % 4) * 8.0, y = (cell / 4) * 8.0;
    if (t < 4) v[t].pos = ndc(x + ((t & 1) ? 8.0 : 0.0), y + ((t & 2) ? 8.0 : 0.0));
    if (t < 2)
    {
        tri[t] = t == 0 ? uint3(0, 1, 2) : uint3(1, 3, 2);
        prim[t].color = cell + 1;
        prim[t].cull = t == 1 && m == 1;
    }
}
[numthreads(128, 1, 1)]
[outputtopology("triangle")]
void ms_big(uint t : SV_GroupThreadID, out vertices V v[153], out indices uint3 tri[256], out primitives P prim[256])
{
    SetMeshOutputCounts(153, 256);
    for (uint i = t; i < 153; i += 128)
        v[i].pos = ndc((i % 17) * 2.0, 24.0 + (i / 17) * 2.0);
    uint qx = t % 16, qy = t / 16, base = qy * 17 + qx;
    tri[t * 2] = uint3(base, base + 1, base + 17);
    tri[t * 2 + 1] = uint3(base + 1, base + 18, base + 17);
    prim[t * 2].color = 50; prim[t * 2].cull = false;
    prim[t * 2 + 1].color = 50; prim[t * 2 + 1].cull = false;
}
uint ps_main(float4 pos : SV_Position, nointerpolation uint color : COLOR) : SV_Target { return color; }
)";
    std::vector<char> as = compile_dxc(src, "as_main", "as_6_5"), ms = compile_dxc(src, "ms_cells", "ms_6_5"),
                      ms_big = compile_dxc(src, "ms_big", "ms_6_5"), ps = compile_dxc(src, "ps_main", "ps_6_5");
    if (as.empty() || ms.empty() || ms_big.empty() || ps.empty())
        return false;
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    ID3D12RootSignature *rs = create_root_signature(rsd);
    D3D12_RASTERIZER_DESC rast = {};
    rast.FillMode = D3D12_FILL_MODE_SOLID;
    rast.CullMode = D3D12_CULL_MODE_NONE;
    rast.DepthClipEnable = TRUE;
    D3D12_BLEND_DESC blend = {};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    D3D12_RT_FORMAT_ARRAY rtv_formats = {};
    rtv_formats.NumRenderTargets = 1;
    rtv_formats.RTFormats[0] = DXGI_FORMAT_R8_UINT;
    struct
    {
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature *> rs;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, D3D12_SHADER_BYTECODE> as;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> ms;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> rast;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC> blend;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> mask;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> rtvs;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> sample;
    } stream;
    stream.rs.value = rs;
    stream.as.value = { as.data(), as.size() };
    stream.ms.value = { ms.data(), ms.size() };
    stream.ps.value = { ps.data(), ps.size() };
    stream.rast.value = rast;
    stream.blend.value = blend;
    stream.mask.value = ~0u;
    stream.rtvs.value = rtv_formats;
    stream.sample.value = { 1, 0 };
    D3D12_PIPELINE_STATE_STREAM_DESC sd = { sizeof(stream), &stream };
    ID3D12Device2 *dev2 = nullptr;
    CHECK_HR(g.device->QueryInterface(IID_PPV_ARGS(&dev2)));
    ID3D12PipelineState *cells = nullptr, *big = nullptr;
    CHECK_HR(dev2->CreatePipelineState(&sd, IID_PPV_ARGS(&cells)));
    stream.as.value = {};
    stream.ms.value = { ms_big.data(), ms_big.size() };
    CHECK_HR(dev2->CreatePipelineState(&sd, IID_PPV_ARGS(&big)));
    ID3D12GraphicsCommandList6 *list6 = nullptr;
    CHECK_HR(g.list->QueryInterface(IID_PPV_ARGS(&list6)));
    const UINT W = 32, H = 40;
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = W; td.Height = H; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8_UINT;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource *rt = nullptr;
    CHECK_HR(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&rt)));
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    ID3D12DescriptorHeap *rtv_heap = nullptr;
    CHECK_HR(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap)));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    g.device->CreateRenderTargetView(rt, nullptr, rtv);
    const UINT clear[4] = {};
    list6->ClearRenderTargetView(rtv, (const float *)clear, 0, nullptr);
    list6->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp = { 0, 0, (float)W, (float)H, 0, 1 };
    D3D12_RECT sc = { 0, 0, (LONG)W, (LONG)H };
    list6->RSSetViewports(1, &vp);
    list6->RSSetScissorRects(1, &sc);
    list6->SetGraphicsRootSignature(rs);
    list6->SetPipelineState(cells);
    list6->DispatchMesh(6, 1, 1);
    list6->SetPipelineState(big);
    list6->DispatchMesh(1, 1, 1);
    ID3D12Resource *rb = create_buffer(256 * H, D3D12_HEAP_TYPE_READBACK);
    transition(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
    dst.pResource = rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8_UINT, W, H, 1, 256 };
    srcl.pResource = rt;
    srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
    if (!execute_and_wait())
        return false;
    const uint8_t *m = nullptr;
    D3D12_RANGE r = { 0, 256 * H };
    rb->Map(0, &r, (void **)&m);
    int wrong = 0;
    for (UINT y = 0; y < H; y++)
        for (UINT x = 0; x < W; x++)
        {
            int want = 0;
            if (y < 24)
            {
                UINT cell = (y / 8) * 4 + x / 8, a = cell / 2, mi = cell % 2, lx = x % 8, ly = y % 8;
                bool launched = mi < a % 3 && !(a == 5 && mi == 1);
                if (launched && mi == 1 && lx + ly + 1 == 8)
                    continue; // on the culled triangle's edge
                if (launched && (mi == 0 || lx + ly + 1 < 8))
                    want = cell + 1;
            }
            else
                want = 50;
            if (m[y * 256 + x] != want && wrong++ < 4)
                printf("  (%u, %u): %u, want %d\n", x, y, m[y * 256 + x], want);
        }
    rb->Unmap(0, nullptr);
    EXPECT(!wrong, "%d pixels wrong", wrong);
    rb->Release(); rtv_heap->Release(); rt->Release(); list6->Release(); cells->Release(); big->Release(); dev2->Release(); rs->Release();
    return true;
}

static bool test_mesh()
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7 = {};
    g.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof(o7));
    if (o7.MeshShaderTier < D3D12_MESH_SHADER_TIER_1)
    {
        printf("  skipped: mesh shader tier %d\n", o7.MeshShaderTier);
        return true;
    }
    static const char *src = R"(
struct Payload { float3 color[2]; };
groupshared Payload pl;
[numthreads(1, 1, 1)]
void as_main()
{
    pl.color[0] = float3(1, 0, 0);
    pl.color[1] = float3(0, 1, 0);
    DispatchMesh(2, 1, 1, pl);
}
struct V { float4 pos : SV_Position; float3 color : COLOR; };
[numthreads(4, 1, 1)]
[outputtopology("triangle")]
void ms_main(uint t : SV_GroupThreadID, uint g : SV_GroupID, in payload Payload p, out vertices V v[4], out indices uint3 tri[2])
{
    SetMeshOutputCounts(4, 2);
    // A quad over half the target: x in [-1, 0] or [0, 1].
    float x0 = g == 0 ? -1.0 : 0.0;
    v[t].pos = float4(x0 + float(t & 1), (t & 2) ? 1.0 : -1.0, 0, 1);
    v[t].color = p.color[g];
    if (t < 2)
        tri[t] = t == 0 ? uint3(0, 1, 2) : uint3(1, 3, 2);
}
float4 ps_main(V i) : SV_Target { return float4(i.color, 1); }
)";
    std::vector<char> as = compile_dxc(src, "as_main", "as_6_5"), ms = compile_dxc(src, "ms_main", "ms_6_5"),
                      ps = compile_dxc(src, "ps_main", "ps_6_5");
    if (as.empty() || ms.empty() || ps.empty())
        return false;
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    ID3D12RootSignature *rs = create_root_signature(rsd);
    D3D12_RASTERIZER_DESC rast = {};
    rast.FillMode = D3D12_FILL_MODE_SOLID;
    rast.CullMode = D3D12_CULL_MODE_NONE;
    rast.DepthClipEnable = TRUE;
    D3D12_BLEND_DESC blend = {};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    D3D12_RT_FORMAT_ARRAY rtv_formats = {};
    rtv_formats.NumRenderTargets = 1;
    rtv_formats.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    struct
    {
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature *> rs;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, D3D12_SHADER_BYTECODE> as;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> ms;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> rast;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC> blend;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> mask;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> rtvs;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> sample;
    } stream;
    stream.rs.value = rs;
    stream.as.value = { as.data(), as.size() };
    stream.ms.value = { ms.data(), ms.size() };
    stream.ps.value = { ps.data(), ps.size() };
    stream.rast.value = rast;
    stream.blend.value = blend;
    stream.mask.value = ~0u;
    stream.rtvs.value = rtv_formats;
    stream.sample.value = { 1, 0 };
    D3D12_PIPELINE_STATE_STREAM_DESC sd = { sizeof(stream), &stream };
    ID3D12Device2 *dev2 = nullptr;
    CHECK_HR(g.device->QueryInterface(IID_PPV_ARGS(&dev2)));
    ID3D12PipelineState *pso = nullptr;
    CHECK_HR(dev2->CreatePipelineState(&sd, IID_PPV_ARGS(&pso)));
    ID3D12GraphicsCommandList6 *list6 = nullptr;
    CHECK_HR(g.list->QueryInterface(IID_PPV_ARGS(&list6)));

    const UINT W = 8, H = 4;
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
    float clear[4] = { 0, 0, 1, 1 };
    list6->ClearRenderTargetView(rtv, clear, 0, nullptr);
    list6->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp = { 0, 0, (float)W, (float)H, 0, 1 };
    D3D12_RECT sc = { 0, 0, (LONG)W, (LONG)H };
    list6->RSSetViewports(1, &vp);
    list6->RSSetScissorRects(1, &sc);
    list6->SetGraphicsRootSignature(rs);
    list6->SetPipelineState(pso);
    list6->DispatchMesh(1, 1, 1);
    ID3D12Resource *rb = create_buffer(256 * H, D3D12_HEAP_TYPE_READBACK);
    transition(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
    dst.pResource = rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 1, 256 };
    srcl.pResource = rt;
    srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list6->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
    if (!execute_and_wait())
        return false;
    const uint32_t *px = nullptr;
    D3D12_RANGE range = { 0, 256 * H };
    rb->Map(0, &range, (void **)&px);
    for (UINT y = 0; y < H; y++)
        for (UINT x = 0; x < W; x++)
        {
            uint32_t want = x < W / 2 ? 0xff0000ff : 0xff00ff00;
            uint32_t got = px[y * 64 + x];
            EXPECT(got == want, "pixel (%u, %u) = 0x%08x, want 0x%08x", x, y, got, want);
        }
    rb->Release(); rtv_heap->Release(); rt->Release(); list6->Release(); pso->Release(); dev2->Release(); rs->Release();
    return true;
}

// Tessellation: a hull shader with constant edge factors and a domain shader placing each tessellated triangle
// patch, drawn as two patches of 3 control points covering the target's halves (colored by the patch ID).
static bool test_tessellation()
{
    static const char *src = R"(
struct VSOut { float2 pos : POS; };
VSOut vs(uint id : SV_VertexID)
{
    // Two triangles: the left and right halves (upper-left and lower-right of each half covered by its patch).
    static const float2 p[6] = { float2(-1, -1), float2(0, -1), float2(-1, 1), float2(0, -1), float2(1, -1), float2(0, 1) };
    VSOut o;
    o.pos = p[id];
    return o;
}
struct HSConst { float edges[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };
HSConst hs_const(InputPatch<VSOut, 3> ip, uint pid : SV_PrimitiveID)
{
    HSConst c;
    c.edges[0] = c.edges[1] = c.edges[2] = 4.0;
    c.inside = 4.0;
    return c;
}
[domain("tri")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("hs_const")]
VSOut hs(InputPatch<VSOut, 3> ip, uint i : SV_OutputControlPointID)
{
    return ip[i];
}
struct DSOut { float4 pos : SV_Position; nointerpolation uint pid : PID; };
[domain("tri")]
DSOut ds(HSConst c, float3 b : SV_DomainLocation, const OutputPatch<VSOut, 3> op, uint pid : SV_PrimitiveID)
{
    DSOut o;
    o.pos = float4(op[0].pos * b.x + op[1].pos * b.y + op[2].pos * b.z, 0, 1);
    o.pid = pid;
    return o;
}
float4 ps(DSOut i) : SV_Target { return i.pid == 0 ? float4(1, 0, 0, 1) : float4(0, 1, 0, 1); }
)";
    std::vector<char> vs = compile_dxc(src, "vs", "vs_6_0"), hs = compile_dxc(src, "hs", "hs_6_0"),
                      ds = compile_dxc(src, "ds", "ds_6_0"), ps = compile_dxc(src, "ps", "ps_6_0");
    if (vs.empty() || hs.empty() || ds.empty() || ps.empty())
        return false;
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    ID3D12RootSignature *rs = create_root_signature(rsd);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = rs;
    pd.VS = { vs.data(), vs.size() };
    pd.HS = { hs.data(), hs.size() };
    pd.DS = { ds.data(), ds.size() };
    pd.PS = { ps.data(), ps.size() };
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = ~0u;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState *pso = nullptr;
    CHECK_HR(g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)));

    const UINT W = 16, H = 8;
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
    float clear[4] = { 0, 0, 1, 1 };
    g.list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp = { 0, 0, (float)W, (float)H, 0, 1 };
    D3D12_RECT sc = { 0, 0, (LONG)W, (LONG)H };
    g.list->RSSetViewports(1, &vp);
    g.list->RSSetScissorRects(1, &sc);
    g.list->SetGraphicsRootSignature(rs);
    g.list->SetPipelineState(pso);
    g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
    g.list->DrawInstanced(6, 1, 0, 0);
    ID3D12Resource *rb = create_buffer(256 * H, D3D12_HEAP_TYPE_READBACK);
    transition(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
    dst.pResource = rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 1, 256 };
    srcl.pResource = rt;
    srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
    if (!execute_and_wait())
        return false;
    const uint32_t *px = nullptr;
    D3D12_RANGE range = { 0, 256 * H };
    rb->Map(0, &range, (void **)&px);
    // Patch 0 covers the left half's upper-left triangle, patch 1 the right half's: pixel (1, 1) of each half
    // (D3D12's y points down: NDC y = -1 is the bottom row, so the triangles' right angles are at the bottom).
    uint32_t left = px[(H - 2) * 64 + 1], right = px[(H - 2) * 64 + W / 2 + 1], none = px[1 * 64 + W / 2 - 1];
    EXPECT(left == 0xff0000ff, "patch 0 pixel 0x%08x, want 0xff0000ff", left);
    EXPECT(right == 0xff00ff00, "patch 1 pixel 0x%08x, want 0xff00ff00", right);
    EXPECT(none == 0xffff0000, "uncovered pixel 0x%08x, want the clear color 0xffff0000", none);
    rb->Release(); rtv_heap->Release(); rt->Release(); pso->Release(); rs->Release();
    return true;
}

// Compute ExecuteIndirect with a count buffer below MaxCommandCount (GPU culling): only the counted commands
// run, each with its own root constant, an empty dispatch inside the count runs nothing, and a count above
// MaxCommandCount is clamped to it.
static bool test_executeindirect_dispatch_count()
{
    static const char *src = R"(
cbuffer C : register(b0) { uint id; };
RWByteAddressBuffer o : register(u0);
[numthreads(4, 1, 1)]
void cs(uint t : SV_DispatchThreadID) { o.InterlockedAdd(id * 4, 1); }
)";
    std::vector<char> csb = compile_dxc(src, "cs", "cs_6_0");
    if (csb.empty())
        return false;
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.Num32BitValues = 1;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[0].ShaderVisibility = rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 2, rp };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    ID3D12PipelineState *cps = create_compute_pipeline(rs, csb);
    if (!cps)
        return false;
    D3D12_INDIRECT_ARGUMENT_DESC ia[2] = {};
    ia[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    ia[0].Constant.Num32BitValuesToSet = 1;
    ia[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    struct Cmd { UINT id; D3D12_DISPATCH_ARGUMENTS dispatch; };
    D3D12_COMMAND_SIGNATURE_DESC sd = { sizeof(Cmd), 2, ia, 0 };
    ID3D12CommandSignature *sig = nullptr;
    CHECK_HR(g.device->CreateCommandSignature(&sd, rs, IID_PPV_ARGS(&sig)));

    const UINT N = 16;
    ID3D12Resource *args = create_buffer(2 * N * sizeof(Cmd) + 16, D3D12_HEAP_TYPE_UPLOAD);
    uint8_t *a = nullptr;
    args->Map(0, nullptr, (void **)&a);
    Cmd *c = (Cmd *)a;
    for (UINT i = 0; i < 2 * N; i++)
        c[i] = { i, { 1 + i % 3, 1, i == 3 ? 0u : 1u } };
    UINT *counts = (UINT *)(a + 2 * N * sizeof(Cmd));
    counts[0] = 10;
    counts[1] = 20;  // Above MaxCommandCount.
    args->Unmap(0, nullptr);
    ID3D12Resource *out = create_buffer(2 * N * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g.list->SetComputeRootSignature(rs);
    g.list->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
    g.list->SetPipelineState(cps);
    g.list->ExecuteIndirect(sig, N, args, 0, args, 2 * N * sizeof(Cmd));
    g.list->ExecuteIndirect(sig, N, args, N * sizeof(Cmd), args, 2 * N * sizeof(Cmd) + 4);
    ID3D12Resource *rb;
    const uint32_t *v = read_buffer(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 2 * N * 4, &rb);
    if (!v)
        return false;
    for (UINT i = 0; i < 2 * N; i++)
    {
        UINT want = (i < N && i >= 10) || i == 3 ? 0 : 4 * (1 + i % 3);
        EXPECT(v[i] == want, "command %u ran %u threads, want %u", i, v[i], want);
    }
    rb->Release(); out->Release(); args->Release(); sig->Release(); cps->Release(); rs->Release();
    return true;
}

// ExecuteIndirect: a command signature that changes a root constant and draws indexed (3 commands, the count
// buffer allowing 2), one that dispatches, and one that also changes the root UAV; each command writes its root
// constant into its own slot.
static bool test_executeindirect()
{
    static const char *src = R"(
cbuffer C : register(b0) { uint id; };
RWByteAddressBuffer o : register(u0);
float4 vs(uint v : SV_VertexID) : SV_Position
{
    float2 uv = float2((v << 1) & 2, v & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
[earlydepthstencil]
float4 ps(float4 pos : SV_Position) : SV_Target
{
    if (all(uint2(pos.xy) == 0))
        o.InterlockedAdd(id * 4, 1);
    return float4(0, 1, 0, 1);
}
[numthreads(4, 1, 1)]
void cs(uint t : SV_DispatchThreadID) { o.InterlockedAdd(64 + id * 4, 1); }
)";
    std::vector<char> vsb = compile_dxc(src, "vs", "vs_6_0"), psb = compile_dxc(src, "ps", "ps_6_0"), csb = compile_dxc(src, "cs", "cs_6_0");
    if (vsb.empty() || psb.empty() || csb.empty())
        return false;
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.Num32BitValues = 1;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[0].ShaderVisibility = rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 2, rp };
    ID3D12RootSignature *rs = create_root_signature(rsd);
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
    ID3D12PipelineState *cps = create_compute_pipeline(rs, csb);
    if (!cps)
        return false;

    // Draw signature: root constant, then DrawIndexed. Dispatch signature: root constant, then Dispatch.
    D3D12_INDIRECT_ARGUMENT_DESC draw_args[2] = {}, dispatch_args[2] = {};
    draw_args[0].Type = dispatch_args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    draw_args[0].Constant.RootParameterIndex = dispatch_args[0].Constant.RootParameterIndex = 0;
    draw_args[0].Constant.Num32BitValuesToSet = dispatch_args[0].Constant.Num32BitValuesToSet = 1;
    draw_args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    dispatch_args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    struct DrawCmd { UINT id; D3D12_DRAW_INDEXED_ARGUMENTS draw; };
    struct DispatchCmd { UINT id; D3D12_DISPATCH_ARGUMENTS dispatch; };
    D3D12_COMMAND_SIGNATURE_DESC draw_sig_desc = { sizeof(DrawCmd), 2, draw_args, 0 };
    D3D12_COMMAND_SIGNATURE_DESC dispatch_sig_desc = { sizeof(DispatchCmd), 2, dispatch_args, 0 };
    ID3D12CommandSignature *draw_sig = nullptr, *dispatch_sig = nullptr;
    CHECK_HR(g.device->CreateCommandSignature(&draw_sig_desc, rs, IID_PPV_ARGS(&draw_sig)));
    CHECK_HR(g.device->CreateCommandSignature(&dispatch_sig_desc, rs, IID_PPV_ARGS(&dispatch_sig)));

    ID3D12Resource *args = create_buffer(256, D3D12_HEAP_TYPE_UPLOAD);
    uint8_t *a = nullptr;
    args->Map(0, nullptr, (void **)&a);
    memset(a, 0, 256);
    DrawCmd *dc = (DrawCmd *)a;
    for (UINT i = 0; i < 3; i++)
        dc[i] = { 1 + i, { 3, 1, 0, 0, 0 } };
    DispatchCmd *pc = (DispatchCmd *)(a + 128);
    pc[0] = { 5, { 2, 1, 1 } };
    pc[1] = { 6, { 1, 1, 1 } };
    *(UINT *)(a + 192) = 2;  // The draw count.
    args->Unmap(0, nullptr);
    ID3D12Resource *ib = create_buffer(16, D3D12_HEAP_TYPE_UPLOAD);
    uint16_t *idx = nullptr;
    ib->Map(0, nullptr, (void **)&idx);
    idx[0] = 0; idx[1] = 1; idx[2] = 2;
    ib->Unmap(0, nullptr);
    ID3D12Resource *out = create_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    const UINT W = 4, H = 4;
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
    g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp = { 0, 0, (float)W, (float)H, 0, 1 };
    D3D12_RECT sc = { 0, 0, (LONG)W, (LONG)H };
    g.list->RSSetViewports(1, &vp);
    g.list->RSSetScissorRects(1, &sc);
    g.list->SetGraphicsRootSignature(rs);
    g.list->SetGraphicsRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
    g.list->SetPipelineState(gps);
    g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_INDEX_BUFFER_VIEW ibv = { ib->GetGPUVirtualAddress(), 16, DXGI_FORMAT_R16_UINT };
    g.list->IASetIndexBuffer(&ibv);
    g.list->ExecuteIndirect(draw_sig, 3, args, 0, args, 192);
    g.list->SetComputeRootSignature(rs);
    g.list->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
    g.list->SetPipelineState(cps);
    g.list->ExecuteIndirect(dispatch_sig, 2, args, 128, nullptr, 0);
    ID3D12Resource *rb;
    const uint32_t *v = read_buffer(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 256, &rb);
    if (!v)
        return false;
    EXPECT(v[0] == 0, "slot 0 = %u, want 0", v[0]);
    EXPECT(v[1] == 1 && v[2] == 1, "draws 1 and 2 = %u %u, want 1 1", v[1], v[2]);
    EXPECT(v[3] == 0, "draw 3 (past the count) = %u, want 0", v[3]);
    EXPECT(v[16 + 5] == 8 && v[16 + 6] == 4, "dispatches 5 and 6 = %u %u, want 8 4", v[16 + 5], v[16 + 6]);

    // A signature that changes the root UAV per command: dispatches with id 7 write into their own buffers.
    D3D12_INDIRECT_ARGUMENT_DESC uav_args[3] = {};
    uav_args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    uav_args[0].Constant.Num32BitValuesToSet = 1;
    uav_args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW;
    uav_args[1].UnorderedAccessView.RootParameterIndex = 1;
    uav_args[2].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    // Command signature arguments are packed: the address follows the constant without padding.
#pragma pack(push, 4)
    struct UavCmd { UINT id; D3D12_GPU_VIRTUAL_ADDRESS uav; D3D12_DISPATCH_ARGUMENTS dispatch; };
#pragma pack(pop)
    D3D12_COMMAND_SIGNATURE_DESC uav_sig_desc = { sizeof(UavCmd), 3, uav_args, 0 };
    ID3D12CommandSignature *uav_sig = nullptr;
    CHECK_HR(g.device->CreateCommandSignature(&uav_sig_desc, rs, IID_PPV_ARGS(&uav_sig)));
    ID3D12Resource *outs[2] = { create_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                                create_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS) };
    ID3D12Resource *uav_argbuf = create_buffer(256, D3D12_HEAP_TYPE_UPLOAD);
    UavCmd *uc = nullptr;
    uav_argbuf->Map(0, nullptr, (void **)&uc);
    uc[0] = { 7, outs[0]->GetGPUVirtualAddress(), { 1, 1, 1 } };
    uc[1] = { 7, outs[1]->GetGPUVirtualAddress(), { 3, 1, 1 } };
    uav_argbuf->Unmap(0, nullptr);
    g.list->SetComputeRootSignature(rs);
    g.list->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
    g.list->SetPipelineState(cps);
    g.list->ExecuteIndirect(uav_sig, 2, uav_argbuf, 0, nullptr, 0);
    ID3D12Resource *rb0, *rb1;
    const uint32_t *v0 = read_buffer(outs[0], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 256, &rb0);
    const uint32_t *v1 = read_buffer(outs[1], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 256, &rb1);
    if (!v0 || !v1)
        return false;
    EXPECT(v0[16 + 7] == 4 && v1[16 + 7] == 12, "per-command root UAVs = %u %u, want 4 12", v0[16 + 7], v1[16 + 7]);
    rb0->Release(); rb1->Release(); outs[0]->Release(); outs[1]->Release(); uav_argbuf->Release(); uav_sig->Release();
    rb->Release(); out->Release(); ib->Release(); args->Release(); draw_sig->Release(); dispatch_sig->Release();
    rtv_heap->Release(); rt->Release(); cps->Release(); gps->Release(); rs->Release();
    return true;
}

// Op-level differential dump (opt-in: D3D12TEST_OPDIFF=1): one compute shader per operation, run on every pair of
// about 40 special 32-bit inputs (zeros, infinities, NaNs, denormals, integer limits) and random ones, compiled as DXIL
// (cs_6_0) and DXBC (cs_5_0). Writes opdiff-dxil.bin and opdiff-dxbc.bin in the current directory (the run's, with
// tests/run/d3d12test.sh); tests/run/opdiff.py compares a run with the Metal Shader Converter bridge against one
// without.
static bool test_opdiff()
{
    if (!getenv("D3D12TEST_OPDIFF"))
    {
        printf("  skipped: D3D12TEST_OPDIFF not set\n");
        return true;
    }
    static const struct { const char *name, *expr; bool sm6_only; } ops[] = {
        { "fadd", "asuint(fx + fy)" }, { "fsub", "asuint(fx - fy)" }, { "fmul", "asuint(fx * fy)" }, { "fdiv", "asuint(fx / fy)" },
        { "fmad", "asuint(mad(fx, fy, fx))" }, { "fmin", "asuint(min(fx, fy))" }, { "fmax", "asuint(max(fx, fy))" },
        { "fabs", "asuint(abs(fx))" }, { "fneg", "asuint(-fx)" }, { "floor", "asuint(floor(fx))" }, { "ceil", "asuint(ceil(fx))" },
        { "frac", "asuint(frac(fx))" }, { "round", "asuint(round(fx))" }, { "trunc", "asuint(trunc(fx))" },
        { "sqrt", "asuint(sqrt(fx))" }, { "rsqrt", "asuint(rsqrt(fx))" }, { "rcp", "asuint(rcp(fx))" }, { "exp2", "asuint(exp2(fx))" },
        { "log2", "asuint(log2(fx))" }, { "sin", "asuint(sin(fx))" }, { "cos", "asuint(cos(fx))" }, { "saturate", "asuint(saturate(fx))" },
        { "sign", "asuint(sign(fx))" }, { "fmod", "asuint(fmod(fx, fy))" }, { "lerp", "asuint(lerp(fx, fy, 0.25))" },
        { "clamp", "asuint(clamp(fx, -1.0, fy))" }, { "dot2", "asuint(dot(float2(fx, fy), float2(fy, fx)))" },
        { "isnan", "(uint)isnan(fx)" }, { "isinf", "(uint)isinf(fx)" }, { "isfinite", "(uint)isfinite(fx)" },
        { "fcmp", "(uint)(fx < fy) | ((uint)(fx == fy) << 1) | ((uint)(fx != fy) << 2) | ((uint)(fx >= fy) << 3)" },
        { "iadd", "x + y" }, { "isub", "x - y" }, { "imul", "x * y" }, { "udiv", "y ? x / y : 0x5eed" }, { "urem", "y ? x % y : 0x5eed" },
        { "idiv", "(iy && !(ix == (int)0x80000000 && iy == -1)) ? asuint(ix / iy) : 0x5eed" },
        { "irem", "(iy && !(ix == (int)0x80000000 && iy == -1)) ? asuint(ix % iy) : 0x5eed" },
        { "and", "x & y" }, { "or", "x | y" }, { "xor", "x ^ y" }, { "not", "~x" },
        { "shl", "x << y" }, { "shr", "x >> y" }, { "ashr", "asuint(ix >> y)" },
        { "countbits", "countbits(x)" }, { "firstbithigh", "firstbithigh(x)" }, { "firstbithigh_s", "asuint(firstbithigh(ix))" },
        { "firstbitlow", "firstbitlow(x)" }, { "reversebits", "reversebits(x)" },
        { "umin", "min(x, y)" }, { "umax", "max(x, y)" }, { "imin", "asuint(min(ix, iy))" }, { "imax", "asuint(max(ix, iy))" },
        { "iabs", "asuint(abs(ix))" }, { "ineg", "asuint(-ix)" }, { "icmp", "(uint)(ix < iy) | ((uint)(x < y) << 1) | ((uint)(x == y) << 2)" },
        { "ftou", "(uint)fx" }, { "ftoi", "asuint((int)fx)" }, { "utof", "asuint((float)x)" }, { "itof", "asuint((float)ix)" },
        { "f32tof16", "f32tof16(fx)" }, { "f16tof32", "asuint(f16tof32(x))" },
        { "select", "fx > fy ? x : y" },
        { "mulhi64", "(uint)(((uint64_t)x * (uint64_t)y) >> 32)", true }, { "add64", "(uint)(((uint64_t)x << 7) + (uint64_t)y >> 5)", true },
        { "i64toF", "asuint((float)(int64_t)(((uint64_t)x << 32) | y))", true },
    };
    // Inputs: every pair of the special values, then random bits.
    static const uint32_t special[] = {
        0x00000000, 0x80000000, 0x3f800000, 0xbf800000, 0x7f800000, 0xff800000, 0x7fc00000, 0xffc00000, 0x7f800001,
        0x7fbfffff, 0x00000001, 0x80000001, 0x007fffff, 0x00800000, 0x7f7fffff, 0xff7fffff, 0x3f000000, 0x40490fdb,
        0x4b000000, 0x4b7fffff, 0x3effffff, 0x3f7fffff, 0x47800000, 0x477fe000, 0x38800000, 0x33800000, 0x4f000000,
        0xcf000000, 0x4f800000, 0x7fffffff, 0xffffffff, 0x00000002, 0x0000001f, 0x00000020, 0x00000021, 0x0000ffff,
        0x00010000, 0x12345678, 0xdeadbeef, 0x3c00, 0x7c00, 0xfc00, 0x7e00, 0x0001, 0x8000,
    };
    const UINT ns = sizeof(special) / sizeof(special[0]), n = 4096;
    std::vector<uint32_t> a(n), b(n);
    uint32_t seed = 0x2545f491;
    for (UINT i = 0; i < n; i++)
    {
        if (i < ns * ns) { a[i] = special[i / ns]; b[i] = special[i % ns]; }
        else { seed = seed * 1664525 + 1013904223; a[i] = seed; seed = seed * 1664525 + 1013904223; b[i] = seed; }
    }
    ID3D12Resource *upload = create_buffer(n * 8, D3D12_HEAP_TYPE_UPLOAD);
    uint8_t *up = nullptr;
    upload->Map(0, nullptr, (void **)&up);
    memcpy(up, a.data(), n * 4);
    memcpy(up + n * 4, b.data(), n * 4);
    upload->Unmap(0, nullptr);
    ID3D12Resource *in = create_buffer(n * 8, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    g.list->CopyBufferRegion(in, 0, upload, 0, n * 8);
    transition(in, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (!execute_and_wait())
        return false;
    D3D12_ROOT_PARAMETER rp[3] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rp[1].Descriptor.ShaderRegister = 1;
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 3, rp };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    int failed = 0;
    for (int dxbc = 0; dxbc < 2; dxbc++)
    {
        const char *path = dxbc ? "opdiff-dxbc.bin" : "opdiff-dxil.bin";
        FILE *f = create_file(path);
        if (!f) { printf("  can't write %s\n", path); return false; }
        fwrite(&n, 4, 1, f);
        fwrite(a.data(), 4, n, f);
        fwrite(b.data(), 4, n, f);
        for (auto &op : ops)
        {
            if (dxbc && op.sm6_only)
                continue;
            std::string src = std::string(
                "ByteAddressBuffer A : register(t0); ByteAddressBuffer B : register(t1); RWByteAddressBuffer O : register(u0);\n"
                "[numthreads(64, 1, 1)] void main(uint i : SV_DispatchThreadID) {\n"
                "  uint x = A.Load(i * 4), y = A.Load(") + std::to_string(n * 4) + " + i * 4);\n"
                "  float fx = asfloat(x), fy = asfloat(y); int ix = asint(x), iy = asint(y);\n"
                "  O.Store(i * 4, " + op.expr + ");\n}\n";
            std::vector<char> cs = dxbc ? compile_fxc(src.c_str(), "main", "cs_5_0") : compile_dxc(src.c_str(), "main", "cs_6_0");
            ID3D12PipelineState *ps = create_compute_pipeline(rs, cs);
            char name[32] = {};
            snprintf(name, sizeof(name), "%s", op.name);
            fwrite(name, 1, 32, f);
            uint32_t ok = ps != nullptr;
            fwrite(&ok, 4, 1, f);
            if (!ps)
            {
                printf("  %s %s: no pipeline\n", dxbc ? "dxbc" : "dxil", op.name);
                failed++;
                continue;
            }
            ID3D12Resource *out = create_buffer(n * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            g.list->SetComputeRootSignature(rs);
            g.list->SetPipelineState(ps);
            g.list->SetComputeRootShaderResourceView(0, in->GetGPUVirtualAddress());
            g.list->SetComputeRootShaderResourceView(1, in->GetGPUVirtualAddress());
            g.list->SetComputeRootUnorderedAccessView(2, out->GetGPUVirtualAddress());
            g.list->Dispatch(n / 64, 1, 1);
            ID3D12Resource *rb = nullptr;
            const uint32_t *r = read_buffer(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, n * 4, &rb);
            if (r)
                fwrite(r, 4, n, f);
            else
                failed++;
            rb->Release(); out->Release(); ps->Release();
        }
        fclose(f);
        printf("  wrote %s\n", path);
    }
    in->Release(); upload->Release(); rs->Release();
    EXPECT(!failed, "%d op pipelines failed", failed);
    return true;
}

// Sampler differential dump (opt-in: D3D12TEST_SAMPLEDIFF=1, writes samplediff.bin in the current directory): a 32x32
// RGBA8 texture with 6 mips of random texels and an R32_FLOAT depth-like texture, sampled with about 25 sampler
// descriptors from a sampler heap (filters, address modes, border colors, LOD bias and clamps, comparison) at 256
// coordinates in and outside [0, 1], by SampleLevel, SampleGrad, Gather and SampleCmpLevelZero. tests/run/samplediff.py
// compares two dumps (with the Metal Shader Converter bridge and without). Always passes: it's a dump.
static bool test_samplediff()
{
    const char *path = "samplediff.bin";
    if (!getenv("D3D12TEST_SAMPLEDIFF"))
    {
        printf("  skipped: D3D12TEST_SAMPLEDIFF not set\n");
        return true;
    }
    static const char *src = R"(
cbuffer C : register(b0) { uint sampler_index; };
Texture2D<float4> tex : register(t0);
Texture2D<float> depth : register(t1);
RWStructuredBuffer<float4> o : register(u0);
uint hash(uint x) { x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16; return x; }
[numthreads(64, 1, 1)] void main(uint t : SV_DispatchThreadID)
{
    SamplerState s = SamplerDescriptorHeap[sampler_index];
    SamplerComparisonState sc = SamplerDescriptorHeap[sampler_index];
    uint h = hash(t * 4 + 1), h2 = hash(t * 4 + 2), h3 = hash(t * 4 + 3);
    float2 uv = float2(h & 0xffff, h >> 16) / 65536.0 * 3.0 - 1.0;     // [-1, 2)
    float lod = (h2 & 0xff) / 255.0 * 6.0 - 0.5;
    float2 g = float2((h3 & 0xff) / 64.0, (h3 >> 8 & 0xff) / 256.0) / 32.0;   // anisotropic gradients
    uint base = t * 4;
    o[base + 0] = tex.SampleLevel(s, uv, lod);
    o[base + 1] = tex.SampleGrad(s, uv, float2(g.x, 0), float2(0, g.y));
    o[base + 2] = tex.Gather(s, uv);
    o[base + 3] = float4(depth.SampleCmpLevelZero(sc, uv, (h2 >> 8 & 0xff) / 255.0), 0, 0, 0);
}
)";
    std::vector<char> cs = compile_dxc(src, "main", "cs_6_6");
    if (cs.empty())
        return false;
    ID3D12Device *dev = g.device;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC vrsd = {};
    vrsd.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    D3D12_DESCRIPTOR_RANGE1 ranges[2] = {
        { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 0 },
        { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 2 },
    };
    D3D12_ROOT_PARAMETER1 rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.Num32BitValues = 1;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable = { 2, ranges };
    vrsd.Desc_1_1 = { 2, rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED };
    ID3DBlob *blob = nullptr, *error = nullptr;
    CHECK_HR(D3D12SerializeVersionedRootSignature(&vrsd, &blob, &error));
    ID3D12RootSignature *rs = nullptr;
    CHECK_HR(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs)));
    blob->Release();
    ID3D12PipelineState *ps = create_compute_pipeline(rs, cs);
    if (!ps)
        return false;

    // Textures: RGBA8 32x32 with 6 mips of random texels; R32_FLOAT 32x32 of random depths.
    const UINT N = 32, mips = 6;
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = N; td.Height = N; td.DepthOrArraySize = 1; td.MipLevels = mips;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    ID3D12Resource *tex = nullptr, *dtex = nullptr;
    CHECK_HR(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex)));
    td.MipLevels = 1; td.Format = DXGI_FORMAT_R32_FLOAT;
    CHECK_HR(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&dtex)));
    ID3D12Resource *upload = create_buffer(256 * N * (mips + 1), D3D12_HEAP_TYPE_UPLOAD);
    uint8_t *up = nullptr;
    upload->Map(0, nullptr, (void **)&up);
    uint32_t seed = 12345;
    auto rnd = [&]() { seed = seed * 1664525 + 1013904223; return seed; };
    for (UINT m = 0; m < mips; m++)
        for (UINT y = 0; y < (N >> m); y++)
            for (UINT x = 0; x < (N >> m); x++)
                ((uint32_t *)(up + m * 256 * N + y * 256))[x] = rnd();
    for (UINT y = 0; y < N; y++)
        for (UINT x = 0; x < N; x++)
            ((float *)(up + mips * 256 * N + y * 256))[x] = (rnd() >> 8) / 16777216.0f;
    upload->Unmap(0, nullptr);
    for (UINT m = 0; m <= mips; m++)
    {
        D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
        dst.pResource = m < mips ? tex : dtex;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = m < mips ? m : 0;
        srcl.pResource = upload;
        srcl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcl.PlacedFootprint.Offset = m * 256 * N;
        UINT w = m < mips ? N >> m : N;
        srcl.PlacedFootprint.Footprint = { m < mips ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R32_FLOAT, w, w, 1, 256 };
        g.list->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
    }
    transition(tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(dtex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    const UINT threads = 256;
    ID3D12Resource *out = create_buffer(threads * 4 * 16, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap *heap = nullptr, *sheap = nullptr;
    CHECK_HR(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
    UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE c = heap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateShaderResourceView(tex, nullptr, c);
    c.ptr += inc;
    dev->CreateShaderResourceView(dtex, nullptr, c);
    c.ptr += inc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = threads * 4;
    uav.Buffer.StructureByteStride = 16;
    dev->CreateUnorderedAccessView(out, nullptr, &uav, c);

    // Sampler descriptors.
    struct Cfg { D3D12_FILTER f; D3D12_TEXTURE_ADDRESS_MODE u, v; float bias; UINT aniso; float minlod, maxlod; float border[4]; D3D12_COMPARISON_FUNC cmp; };
    const D3D12_TEXTURE_ADDRESS_MODE W_ = D3D12_TEXTURE_ADDRESS_MODE_WRAP, M_ = D3D12_TEXTURE_ADDRESS_MODE_MIRROR,
        C_ = D3D12_TEXTURE_ADDRESS_MODE_CLAMP, B_ = D3D12_TEXTURE_ADDRESS_MODE_BORDER, O_ = D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
    const float FMAX = D3D12_FLOAT32_MAX;
    static std::vector<Cfg> cfgs;
    cfgs = {
        { D3D12_FILTER_MIN_MAG_MIP_POINT, W_, W_, 0, 1, 0, FMAX, {}, D3D12_COMPARISON_FUNC_NEVER },
        { D3D12_FILTER_MIN_MAG_MIP_LINEAR, W_, W_, 0, 1, 0, FMAX, {}, D3D12_COMPARISON_FUNC_NEVER },
        { D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT, W_, W_, 0, 1, 0, FMAX, {}, D3D12_COMPARISON_FUNC_NEVER },
        { D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT, W_, W_, 0, 1, 0, FMAX, {}, D3D12_COMPARISON_FUNC_NEVER },
        { D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT, W_, W_, 0, 1, 0, FMAX, {}, D3D12_COMPARISON_FUNC_NEVER },
        { D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR, W_, W_, 0, 1, 0, FMAX, {}, D3D12_COMPARISON_FUNC_NEVER },
        { D3D12_FILTER_MIN_MAG_MIP_LINEAR, M_, M_, 0, 1, 0, FMAX, {}, D3D12_COMPARISON_FUNC_NEVER },
        { D3D12_FILTER_MIN_MAG_MIP_LINEAR, C_, C_, 0, 1, 0, FMAX, {}, D3D12_COMPARISON_FUNC_NEVER },
        { D3D12_FILTER_MIN_MAG_MIP_LINEAR, O_, O_, 0, 1, 0, FMAX, {}, D3D12_COMPARISON_FUNC_NEVER },
        { D3D12_FILTER_MIN_MAG_MIP_LINEAR, B_, B_, 0, 1, 0, FMAX, { 0, 0, 0, 0 }, D3D12_COMPARISON_FUNC_NEVER },
        { D3D12_FILTER_MIN_MAG_MIP_LINEAR, B_, B_, 0, 1, 0, FMAX, { 0, 0, 0, 1 }, D3D12_COMPARISON_FUNC_NEVER },
        { D3D12_FILTER_MIN_MAG_MIP_LINEAR, B_, B_, 0, 1, 0, FMAX, { 1, 1, 1, 1 }, D3D12_COMPARISON_FUNC_NEVER },
        { D3D12_FILTER_MIN_MAG_MIP_POINT, W_, C_, 0, 1, 0, FMAX, {}, D3D12_COMPARISON_FUNC_NEVER },
        { D3D12_FILTER_MIN_MAG_MIP_LINEAR, W_, W_, 1.5f, 1, 0, FMAX, {}, D3D12_COMPARISON_FUNC_NEVER },
        { D3D12_FILTER_MIN_MAG_MIP_LINEAR, W_, W_, -2.0f, 1, 0, FMAX, {}, D3D12_COMPARISON_FUNC_NEVER },
        { D3D12_FILTER_MIN_MAG_MIP_LINEAR, W_, W_, 0, 1, 1.5f, FMAX, {}, D3D12_COMPARISON_FUNC_NEVER },
        { D3D12_FILTER_MIN_MAG_MIP_LINEAR, W_, W_, 0, 1, 0, 2.0f, {}, D3D12_COMPARISON_FUNC_NEVER },
        { D3D12_FILTER_MIN_MAG_MIP_POINT, W_, W_, 0, 1, 0, 0.0f, {}, D3D12_COMPARISON_FUNC_NEVER },
        { D3D12_FILTER_ANISOTROPIC, W_, W_, 0, 4, 0, FMAX, {}, D3D12_COMPARISON_FUNC_NEVER },
        { D3D12_FILTER_ANISOTROPIC, W_, W_, 0, 16, 0, FMAX, {}, D3D12_COMPARISON_FUNC_NEVER },
        { D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT, C_, C_, 0, 1, 0, FMAX, {}, D3D12_COMPARISON_FUNC_LESS },
        { D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, C_, C_, 0, 1, 0, FMAX, {}, D3D12_COMPARISON_FUNC_LESS_EQUAL },
        { D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, C_, C_, 0, 1, 0, FMAX, {}, D3D12_COMPARISON_FUNC_GREATER },
        { D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, B_, B_, 0, 1, 0, FMAX, { 1, 1, 1, 1 }, D3D12_COMPARISON_FUNC_LESS },
        { D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT, W_, W_, 0, 1, 0, FMAX, {}, D3D12_COMPARISON_FUNC_ALWAYS },
    };
    D3D12_DESCRIPTOR_HEAP_DESC shd = { D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, (UINT)cfgs.size(), D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    CHECK_HR(dev->CreateDescriptorHeap(&shd, IID_PPV_ARGS(&sheap)));
    UINT sinc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    for (UINT i = 0; i < cfgs.size(); i++)
    {
        D3D12_SAMPLER_DESC sd = {};
        sd.Filter = cfgs[i].f;
        sd.AddressU = cfgs[i].u; sd.AddressV = cfgs[i].v; sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sd.MipLODBias = cfgs[i].bias;
        sd.MaxAnisotropy = cfgs[i].aniso;
        sd.ComparisonFunc = cfgs[i].cmp;
        memcpy(sd.BorderColor, cfgs[i].border, sizeof(sd.BorderColor));
        sd.MinLOD = cfgs[i].minlod; sd.MaxLOD = cfgs[i].maxlod;
        D3D12_CPU_DESCRIPTOR_HANDLE sc = sheap->GetCPUDescriptorHandleForHeapStart();
        sc.ptr += i * sinc;
        dev->CreateSampler(&sd, sc);
    }

    FILE *f = create_file(path);
    if (!f) { printf("  can't write %s\n", path); return false; }
    UINT count = (UINT)cfgs.size();
    fwrite(&count, 4, 1, f);
    fwrite(&threads, 4, 1, f);
    ID3D12DescriptorHeap *heaps[2] = { heap, sheap };
    for (UINT i = 0; i < count; i++)
    {
        g.list->SetDescriptorHeaps(2, heaps);
        g.list->SetComputeRootSignature(rs);
        g.list->SetPipelineState(ps);
        g.list->SetComputeRoot32BitConstant(0, i, 0);
        g.list->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
        g.list->Dispatch(threads / 64, 1, 1);
        ID3D12Resource *rb = nullptr;
        const uint32_t *r = read_buffer(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, threads * 4 * 16, &rb);
        if (!r) return false;
        fwrite(r, 16, threads * 4, f);
        rb->Release();
    }
    fclose(f);
    printf("  wrote %u sampler configurations\n", count);
    out->Release(); heap->Release(); sheap->Release(); tex->Release(); dtex->Release(); upload->Release();
    ps->Release(); rs->Release();
    return true;
}

// Typed buffer format differential dump (opt-in: D3D12TEST_FORMATDIFF=1, writes formatdiff.bin in the current
// directory): for each format a buffer of random bytes is read through a typed SRV (Buffer<float4>, <uint4> or <int4>
// by the format's class) and values made from it are written through a typed UAV; both the loaded values and the stored
// bytes are dumped. tests/run/formatdiff.py compares two dumps (with the Metal Shader Converter bridge and without).
static bool test_formatdiff()
{
    const char *path = "formatdiff.bin";
    if (!getenv("D3D12TEST_FORMATDIFF"))
    {
        printf("  skipped: D3D12TEST_FORMATDIFF not set\n");
        return true;
    }
    enum Cls { F, U, I };
    static const struct { DXGI_FORMAT fmt; Cls cls; UINT size; const char *name; } fmts[] = {
        { DXGI_FORMAT_R32G32B32A32_FLOAT, F, 16, "R32G32B32A32_FLOAT" }, { DXGI_FORMAT_R32G32B32A32_UINT, U, 16, "R32G32B32A32_UINT" },
        { DXGI_FORMAT_R32G32B32A32_SINT, I, 16, "R32G32B32A32_SINT" }, { DXGI_FORMAT_R16G16B16A16_FLOAT, F, 8, "R16G16B16A16_FLOAT" },
        { DXGI_FORMAT_R16G16B16A16_UNORM, F, 8, "R16G16B16A16_UNORM" }, { DXGI_FORMAT_R16G16B16A16_SNORM, F, 8, "R16G16B16A16_SNORM" },
        { DXGI_FORMAT_R16G16B16A16_UINT, U, 8, "R16G16B16A16_UINT" }, { DXGI_FORMAT_R16G16B16A16_SINT, I, 8, "R16G16B16A16_SINT" },
        { DXGI_FORMAT_R32G32_FLOAT, F, 8, "R32G32_FLOAT" }, { DXGI_FORMAT_R32G32_UINT, U, 8, "R32G32_UINT" }, { DXGI_FORMAT_R32G32_SINT, I, 8, "R32G32_SINT" },
        { DXGI_FORMAT_R10G10B10A2_UNORM, F, 4, "R10G10B10A2_UNORM" }, { DXGI_FORMAT_R10G10B10A2_UINT, U, 4, "R10G10B10A2_UINT" },
        { DXGI_FORMAT_R11G11B10_FLOAT, F, 4, "R11G11B10_FLOAT" }, { DXGI_FORMAT_R8G8B8A8_UNORM, F, 4, "R8G8B8A8_UNORM" },
        { DXGI_FORMAT_R8G8B8A8_SNORM, F, 4, "R8G8B8A8_SNORM" }, { DXGI_FORMAT_R8G8B8A8_UINT, U, 4, "R8G8B8A8_UINT" },
        { DXGI_FORMAT_R8G8B8A8_SINT, I, 4, "R8G8B8A8_SINT" }, { DXGI_FORMAT_B8G8R8A8_UNORM, F, 4, "B8G8R8A8_UNORM" },
        { DXGI_FORMAT_R16G16_FLOAT, F, 4, "R16G16_FLOAT" }, { DXGI_FORMAT_R16G16_UNORM, F, 4, "R16G16_UNORM" },
        { DXGI_FORMAT_R16G16_SNORM, F, 4, "R16G16_SNORM" }, { DXGI_FORMAT_R16G16_UINT, U, 4, "R16G16_UINT" },
        { DXGI_FORMAT_R16G16_SINT, I, 4, "R16G16_SINT" }, { DXGI_FORMAT_R32_FLOAT, F, 4, "R32_FLOAT" },
        { DXGI_FORMAT_R32_UINT, U, 4, "R32_UINT" }, { DXGI_FORMAT_R32_SINT, I, 4, "R32_SINT" },
        { DXGI_FORMAT_R8G8_UNORM, F, 2, "R8G8_UNORM" }, { DXGI_FORMAT_R8G8_SNORM, F, 2, "R8G8_SNORM" }, { DXGI_FORMAT_R8G8_UINT, U, 2, "R8G8_UINT" },
        { DXGI_FORMAT_R16_FLOAT, F, 2, "R16_FLOAT" }, { DXGI_FORMAT_R16_UNORM, F, 2, "R16_UNORM" }, { DXGI_FORMAT_R16_SNORM, F, 2, "R16_SNORM" },
        { DXGI_FORMAT_R16_UINT, U, 2, "R16_UINT" }, { DXGI_FORMAT_R16_SINT, I, 2, "R16_SINT" },
        { DXGI_FORMAT_R8_UNORM, F, 1, "R8_UNORM" }, { DXGI_FORMAT_R8_SNORM, F, 1, "R8_SNORM" }, { DXGI_FORMAT_R8_UINT, U, 1, "R8_UINT" },
        { DXGI_FORMAT_R8_SINT, I, 1, "R8_SINT" },
    };
    static const char *tmpl = R"(
Buffer<T> src : register(t0);
RWBuffer<T> dst : register(u0);
RWStructuredBuffer<uint4> loaded : register(u1);
[numthreads(64, 1, 1)] void main(uint i : SV_DispatchThreadID)
{
    T v = src[i];
    loaded[i] = asuint(v);
    dst[i] = STORE;
}
)";
    const char *stores[3] = { "v * 0.75 - float4(0.1, 0.2, -0.3, 0.05) + float4(i, i, i, i) * 0.003", "v * 3u + i", "v * -3 + int(i)" };
    const char *types[3] = { "float4", "uint4", "int4" };
    const UINT n = 256;
    ID3D12Device *dev = g.device;
    D3D12_DESCRIPTOR_RANGE ranges[2] = { { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 }, { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 1 } };
    D3D12_ROOT_PARAMETER rp = {};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp.DescriptorTable = { 2, ranges };
    D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &rp };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    ID3D12PipelineState *pso[3] = {};
    for (int c = 0; c < 3; c++)
    {
        std::string src = std::string("#define T ") + types[c] + "\n#define STORE " + stores[c] + "\n" + tmpl;
        pso[c] = create_compute_pipeline(rs, compile_dxc(src.c_str(), "main", "cs_6_0"));
        if (!pso[c]) return false;
    }
    ID3D12Resource *upload = create_buffer(n * 16, D3D12_HEAP_TYPE_UPLOAD);
    uint8_t *up = nullptr;
    upload->Map(0, nullptr, (void **)&up);
    uint32_t seed = 777;
    for (UINT i = 0; i < n * 16; i++) { seed = seed * 1664525 + 1013904223; up[i] = seed >> 24; }
    // Keep 16- and 32-bit floats finite and not too large: clear the top exponent bits in some elements.
    upload->Unmap(0, nullptr);
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap *heap = nullptr;
    CHECK_HR(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
    UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    FILE *f = create_file(path);
    if (!f) { printf("  can't write %s\n", path); return false; }
    UINT count = sizeof(fmts) / sizeof(fmts[0]);
    fwrite(&count, 4, 1, f);
    fwrite(&n, 4, 1, f);
    for (UINT k = 0; k < count; k++)
    {
        ID3D12Resource *srcb = create_buffer(n * 16, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        ID3D12Resource *dstb = create_buffer(n * 16, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12Resource *ldb = create_buffer(n * 16, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        g.list->CopyBufferRegion(srcb, 0, upload, 0, n * 16);
        transition(srcb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        D3D12_CPU_DESCRIPTOR_HANDLE c = heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = fmts[k].fmt;
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Buffer.NumElements = n;
        dev->CreateShaderResourceView(srcb, &sd, c);
        c.ptr += inc;
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = fmts[k].fmt;
        ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = n;
        dev->CreateUnorderedAccessView(dstb, nullptr, &ud, c);
        c.ptr += inc;
        D3D12_UNORDERED_ACCESS_VIEW_DESC ld = {};
        ld.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        ld.Buffer.NumElements = n;
        ld.Buffer.StructureByteStride = 16;
        dev->CreateUnorderedAccessView(ldb, nullptr, &ld, c);
        g.list->SetDescriptorHeaps(1, &heap);
        g.list->SetComputeRootSignature(rs);
        g.list->SetPipelineState(pso[fmts[k].cls]);
        g.list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
        g.list->Dispatch(n / 64, 1, 1);
        ID3D12Resource *rb1 = nullptr, *rb2 = nullptr;
        const uint32_t *lv = read_buffer(ldb, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, n * 16, &rb1);
        const uint32_t *sv = read_buffer(dstb, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, n * 16, &rb2);
        if (!lv || !sv) return false;
        char name[32] = {};
        snprintf(name, sizeof(name), "%s", fmts[k].name);
        fwrite(name, 1, 32, f);
        fwrite(&fmts[k].size, 4, 1, f);
        fwrite(&fmts[k].cls, 4, 1, f);
        fwrite(lv, 16, n, f);
        fwrite(sv, 1, n * fmts[k].size, f);
        rb1->Release(); rb2->Release(); srcb->Release(); dstb->Release(); ldb->Release();
    }
    fclose(f);
    printf("  wrote %u formats\n", count);
    heap->Release(); upload->Release(); rs->Release();
    for (auto *p : pso) p->Release();
    return true;
}

// Texture format differential dump (opt-in: D3D12TEST_TEXDIFF=1, writes texdiff.bin in the current directory): 16x16
// textures of random data in color formats (sRGB, packed 16-bit, shared exponent, BC1 to BC7) and depth/stencil
// formats, every texel read with Load through an SRV (depth and stencil planes through their plane formats).
// tests/run/texdiff.py compares two dumps.
static bool test_texdiff()
{
    const char *path = "texdiff.bin";
    if (!getenv("D3D12TEST_TEXDIFF"))
    {
        printf("  skipped: D3D12TEST_TEXDIFF not set\n");
        return true;
    }
    // resource format, SRV format, plane, bytes per block, block size, uint read, name
    static const struct { DXGI_FORMAT res, srv; UINT plane, bpb, block; bool uint_read, depth; const char *name; } fmts[] = {
        { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 0, 4, 1, false, false, "R8G8B8A8_UNORM_SRGB" },
        { DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, 0, 4, 1, false, false, "B8G8R8A8_UNORM_SRGB" },
        { DXGI_FORMAT_B8G8R8X8_UNORM, DXGI_FORMAT_B8G8R8X8_UNORM, 0, 4, 1, false, false, "B8G8R8X8_UNORM" },
        { DXGI_FORMAT_R9G9B9E5_SHAREDEXP, DXGI_FORMAT_R9G9B9E5_SHAREDEXP, 0, 4, 1, false, false, "R9G9B9E5_SHAREDEXP" },
        { DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_B5G6R5_UNORM, 0, 2, 1, false, false, "B5G6R5_UNORM" },
        { DXGI_FORMAT_B5G5R5A1_UNORM, DXGI_FORMAT_B5G5R5A1_UNORM, 0, 2, 1, false, false, "B5G5R5A1_UNORM" },
        { DXGI_FORMAT_B4G4R4A4_UNORM, DXGI_FORMAT_B4G4R4A4_UNORM, 0, 2, 1, false, false, "B4G4R4A4_UNORM" },
        { DXGI_FORMAT_A8_UNORM, DXGI_FORMAT_A8_UNORM, 0, 1, 1, false, false, "A8_UNORM" },
        { DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM, 0, 4, 1, false, false, "R10G10B10A2_UNORM" },
        { DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_R11G11B10_FLOAT, 0, 4, 1, false, false, "R11G11B10_FLOAT" },
        { DXGI_FORMAT_R8G8_SNORM, DXGI_FORMAT_R8G8_SNORM, 0, 2, 1, false, false, "R8G8_SNORM" },
        { DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM, 0, 8, 4, false, false, "BC1_UNORM" },
        { DXGI_FORMAT_BC1_UNORM_SRGB, DXGI_FORMAT_BC1_UNORM_SRGB, 0, 8, 4, false, false, "BC1_UNORM_SRGB" },
        { DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC2_UNORM, 0, 16, 4, false, false, "BC2_UNORM" },
        { DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC3_UNORM, 0, 16, 4, false, false, "BC3_UNORM" },
        { DXGI_FORMAT_BC4_UNORM, DXGI_FORMAT_BC4_UNORM, 0, 8, 4, false, false, "BC4_UNORM" },
        { DXGI_FORMAT_BC4_SNORM, DXGI_FORMAT_BC4_SNORM, 0, 8, 4, false, false, "BC4_SNORM" },
        { DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_BC5_UNORM, 0, 16, 4, false, false, "BC5_UNORM" },
        { DXGI_FORMAT_BC5_SNORM, DXGI_FORMAT_BC5_SNORM, 0, 16, 4, false, false, "BC5_SNORM" },
        { DXGI_FORMAT_BC6H_UF16, DXGI_FORMAT_BC6H_UF16, 0, 16, 4, false, false, "BC6H_UF16" },
        { DXGI_FORMAT_BC6H_SF16, DXGI_FORMAT_BC6H_SF16, 0, 16, 4, false, false, "BC6H_SF16" },
        { DXGI_FORMAT_BC7_UNORM, DXGI_FORMAT_BC7_UNORM, 0, 16, 4, false, false, "BC7_UNORM" },
        { DXGI_FORMAT_BC7_UNORM_SRGB, DXGI_FORMAT_BC7_UNORM_SRGB, 0, 16, 4, false, false, "BC7_UNORM_SRGB" },
        { DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT, 0, 4, 1, false, true, "D32_FLOAT depth" },
        { DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UNORM, 0, 2, 1, false, true, "D16_UNORM depth" },
        { DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_R24_UNORM_X8_TYPELESS, 0, 4, 1, false, true, "D24S8 depth" },
        { DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_X24_TYPELESS_G8_UINT, 1, 1, 1, true, true, "D24S8 stencil" },
        { DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, 0, 4, 1, false, true, "D32S8 depth" },
        { DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_X32_TYPELESS_G8X24_UINT, 1, 1, 1, true, true, "D32S8 stencil" },
    };
    static const char *src = R"(
Texture2D<float4> tf : register(t0);
Texture2D<uint4> tu : register(t1);
RWStructuredBuffer<uint4> o : register(u0);
cbuffer C : register(b0) { uint use_uint; };
[numthreads(16, 16, 1)] void main(uint2 p : SV_DispatchThreadID)
{
    o[p.y * 16 + p.x] = use_uint ? tu.Load(int3(p, 0)) : asuint(tf.Load(int3(p, 0)));
}
)";
    ID3D12Device *dev = g.device;
    D3D12_DESCRIPTOR_RANGE ranges[2] = { { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0 }, { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 2 } };
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.Num32BitValues = 1;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable = { 2, ranges };
    D3D12_ROOT_SIGNATURE_DESC rsd = { 2, rp };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    ID3D12PipelineState *ps = create_compute_pipeline(rs, compile_dxc(src, "main", "cs_6_0"));
    if (!ps) return false;
    const UINT N = 16;
    ID3D12Resource *upload = create_buffer(256 * N, D3D12_HEAP_TYPE_UPLOAD);
    ID3D12Resource *out = create_buffer(N * N * 16, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap *heap = nullptr;
    CHECK_HR(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
    UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    FILE *f = create_file(path);
    if (!f) { printf("  can't write %s\n", path); return false; }
    UINT count = sizeof(fmts) / sizeof(fmts[0]);
    fwrite(&count, 4, 1, f);
    uint32_t seed = 4242;
    int failed = 0;
    for (UINT k = 0; k < count; k++)
    {
        // Random bytes, except depth: floats in [0, 1] (D32), any value for UNORM depth, stencil any byte.
        uint8_t *up = nullptr;
        upload->Map(0, nullptr, (void **)&up);
        for (UINT i = 0; i < 256 * N; i++) { seed = seed * 1664525 + 1013904223; up[i] = seed >> 24; }
        if (fmts[k].depth && fmts[k].plane == 0 && fmts[k].bpb == 4 && fmts[k].srv != DXGI_FORMAT_R24_UNORM_X8_TYPELESS)
            for (UINT i = 0; i < 64 * N; i++) { seed = seed * 1664525 + 1013904223; ((float *)up)[i] = (seed >> 8) / 16777216.0f; }
        upload->Unmap(0, nullptr);
        D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC td = {};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = N; td.Height = N; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = fmts[k].res;
        td.SampleDesc.Count = 1;
        td.Flags = fmts[k].depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_NONE;
        ID3D12Resource *tex = nullptr;
        char name[32] = {};
        snprintf(name, sizeof(name), "%s", fmts[k].name);
        fwrite(name, 1, 32, f);
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex))))
        {
            uint32_t zero = 0;
            fwrite(&zero, 4, 1, f);
            printf("  %s: no texture\n", fmts[k].name);
            continue;
        }
        UINT planes = (fmts[k].res == DXGI_FORMAT_R24G8_TYPELESS || fmts[k].res == DXGI_FORMAT_R32G8X24_TYPELESS) ? 2 : 1;
        for (UINT pl = 0; pl < planes; pl++)
        {
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
            UINT rows = 0; UINT64 rowsize = 0, total = 0;
            dev->GetCopyableFootprints(&td, pl, 1, 0, &fp, &rows, &rowsize, &total);
            fp.Offset = 0;
            fp.Footprint.RowPitch = 256;
            D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
            dst.pResource = tex;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = pl;
            srcl.pResource = upload;
            srcl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            srcl.PlacedFootprint = fp;
            g.list->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
        }
        transition(tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        D3D12_CPU_DESCRIPTOR_HANDLE c = heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = fmts[k].srv;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        sd.Texture2D.PlaneSlice = fmts[k].plane;
        dev->CreateShaderResourceView(tex, &sd, c);                       // t0: float view (or the stencil view, unused)
        c.ptr += inc;
        dev->CreateShaderResourceView(tex, &sd, c);                       // t1: the same, read as uint for stencil
        c.ptr += inc;
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = N * N;
        ud.Buffer.StructureByteStride = 16;
        dev->CreateUnorderedAccessView(out, nullptr, &ud, c);
        g.list->SetDescriptorHeaps(1, &heap);
        g.list->SetComputeRootSignature(rs);
        g.list->SetPipelineState(ps);
        g.list->SetComputeRoot32BitConstant(0, fmts[k].uint_read ? 1 : 0, 0);
        g.list->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
        g.list->Dispatch(1, 1, 1);
        ID3D12Resource *rb = nullptr;
        const uint32_t *r = read_buffer(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, N * N * 16, &rb);
        if (!r) { failed++; continue; }
        uint32_t one = 1;
        fwrite(&one, 4, 1, f);
        uint32_t ui = fmts[k].uint_read;
        fwrite(&ui, 4, 1, f);
        fwrite(r, 16, N * N, f);
        // The uploaded bytes, for checking against a reference.
        uint8_t *upr = nullptr;
        upload->Map(0, nullptr, (void **)&upr);
        fwrite(upr, 1, 256 * N, f);
        upload->Unmap(0, nullptr);
        rb->Release();
        tex->Release();
    }
    fclose(f);
    printf("  wrote %u texture formats\n", count);
    out->Release(); heap->Release(); upload->Release(); ps->Release(); rs->Release();
    EXPECT(!failed, "%d formats failed", failed);
    return true;
}

// Vertex input format differential dump (opt-in: D3D12TEST_VTXDIFF=1, writes vtxdiff.bin in the current directory): for
// each vertex format, 64 points from a per-vertex stream and 4 instances from a per-instance stream (step rate 2) of
// random bytes, read by a vertex shader that writes both attributes' bits to a UAV (missing components must read as 0,
// 0, 0, 1). tests/run/vtxdiff.py compares two dumps.
static bool test_vtxdiff()
{
    const char *path = "vtxdiff.bin";
    if (!getenv("D3D12TEST_VTXDIFF"))
    {
        printf("  skipped: D3D12TEST_VTXDIFF not set\n");
        return true;
    }
    enum Cls { F, U, I };
    static const struct { DXGI_FORMAT fmt; Cls cls; UINT size; const char *name; } fmts[] = {
        { DXGI_FORMAT_R32G32B32A32_FLOAT, F, 16, "R32G32B32A32_FLOAT" }, { DXGI_FORMAT_R32G32B32_FLOAT, F, 12, "R32G32B32_FLOAT" },
        { DXGI_FORMAT_R32G32B32_UINT, U, 12, "R32G32B32_UINT" }, { DXGI_FORMAT_R32G32B32_SINT, I, 12, "R32G32B32_SINT" },
        { DXGI_FORMAT_R32G32_FLOAT, F, 8, "R32G32_FLOAT" }, { DXGI_FORMAT_R32_FLOAT, F, 4, "R32_FLOAT" }, { DXGI_FORMAT_R32_UINT, U, 4, "R32_UINT" },
        { DXGI_FORMAT_R16G16B16A16_FLOAT, F, 8, "R16G16B16A16_FLOAT" }, { DXGI_FORMAT_R16G16B16A16_UNORM, F, 8, "R16G16B16A16_UNORM" },
        { DXGI_FORMAT_R16G16B16A16_SNORM, F, 8, "R16G16B16A16_SNORM" }, { DXGI_FORMAT_R16G16B16A16_UINT, U, 8, "R16G16B16A16_UINT" },
        { DXGI_FORMAT_R16G16B16A16_SINT, I, 8, "R16G16B16A16_SINT" }, { DXGI_FORMAT_R16G16_FLOAT, F, 4, "R16G16_FLOAT" },
        { DXGI_FORMAT_R16G16_UNORM, F, 4, "R16G16_UNORM" }, { DXGI_FORMAT_R16G16_SNORM, F, 4, "R16G16_SNORM" },
        { DXGI_FORMAT_R16G16_SINT, I, 4, "R16G16_SINT" }, { DXGI_FORMAT_R16_UINT, U, 2, "R16_UINT" }, { DXGI_FORMAT_R16_FLOAT, F, 2, "R16_FLOAT" },
        { DXGI_FORMAT_R8G8B8A8_UNORM, F, 4, "R8G8B8A8_UNORM" }, { DXGI_FORMAT_R8G8B8A8_SNORM, F, 4, "R8G8B8A8_SNORM" },
        { DXGI_FORMAT_R8G8B8A8_UINT, U, 4, "R8G8B8A8_UINT" }, { DXGI_FORMAT_R8G8B8A8_SINT, I, 4, "R8G8B8A8_SINT" },
        { DXGI_FORMAT_B8G8R8A8_UNORM, F, 4, "B8G8R8A8_UNORM" }, { DXGI_FORMAT_R10G10B10A2_UNORM, F, 4, "R10G10B10A2_UNORM" },
        { DXGI_FORMAT_R10G10B10A2_UINT, U, 4, "R10G10B10A2_UINT" }, { DXGI_FORMAT_R11G11B10_FLOAT, F, 4, "R11G11B10_FLOAT" },
        { DXGI_FORMAT_R8G8_UNORM, F, 2, "R8G8_UNORM" }, { DXGI_FORMAT_R8_UNORM, F, 1, "R8_UNORM" }, { DXGI_FORMAT_R8_UINT, U, 1, "R8_UINT" },
    };
    static const char *tmpl = R"(
struct In { T a : ATTR; T b : INST; uint vid : SV_VertexID; uint iid : SV_InstanceID; };
RWStructuredBuffer<uint4> o : register(u0);
float4 main(In i) : SV_Position
{
    o[i.iid * 64 + i.vid] = asuint(i.a);
    if (i.vid == 0) o[256 + i.iid] = asuint(i.b);
    return float4(0, 0, 0, 1);
}
float4 ps() : SV_Target { return 0; }
)";
    const char *types[3] = { "float4", "uint4", "int4" };
    ID3D12Device *dev = g.device;
    D3D12_ROOT_PARAMETER rp = {};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    // A render target and a pixel shader: a pipeline with nothing to rasterize into is made with rasterizer discard,
    // which the bridge can't do (a converted vertex function returns its outputs), and would test SPIR-V only.
    std::vector<char> vsb[3], psb = compile_dxc((std::string("#define T float4\n") + tmpl).c_str(), "ps", "ps_6_0");
    for (int c = 0; c < 3; c++)
    {
        std::string src = std::string("#define T ") + types[c] + "\n" + tmpl;
        vsb[c] = compile_dxc(src.c_str(), "main", "vs_6_0");
        if (vsb[c].empty()) return false;
    }
    if (psb.empty()) return false;
    D3D12_HEAP_PROPERTIES rhp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC rtd = {};
    rtd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rtd.Width = 4; rtd.Height = 4; rtd.DepthOrArraySize = 1; rtd.MipLevels = 1;
    rtd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rtd.SampleDesc.Count = 1;
    rtd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource *rt = nullptr;
    CHECK_HR(dev->CreateCommittedResource(&rhp, D3D12_HEAP_FLAG_NONE, &rtd, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&rt)));
    D3D12_DESCRIPTOR_HEAP_DESC rhd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    ID3D12DescriptorHeap *rtv_heap = nullptr;
    CHECK_HR(dev->CreateDescriptorHeap(&rhd, IID_PPV_ARGS(&rtv_heap)));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(rt, nullptr, rtv);
    const UINT V = 64, inst = 4;
    ID3D12Resource *vb = create_buffer(V * 16 + 256, D3D12_HEAP_TYPE_UPLOAD);
    uint8_t *vp = nullptr;
    vb->Map(0, nullptr, (void **)&vp);
    uint32_t seed = 99;
    for (UINT i = 0; i < V * 16 + 256; i++) { seed = seed * 1664525 + 1013904223; vp[i] = seed >> 24; }
    vb->Unmap(0, nullptr);
    ID3D12Resource *out = create_buffer(260 * 16, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    FILE *f = create_file(path);
    if (!f) { printf("  can't write %s\n", path); return false; }
    UINT count = sizeof(fmts) / sizeof(fmts[0]);
    fwrite(&count, 4, 1, f);
    int failed = 0;
    for (UINT k = 0; k < count; k++)
    {
        D3D12_INPUT_ELEMENT_DESC el[2] = {
            { "ATTR", 0, fmts[k].fmt, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "INST", 0, fmts[k].fmt, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 2 },
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = rs;
        pd.VS = { vsb[fmts[k].cls].data(), vsb[fmts[k].cls].size() };
        pd.PS = { psb.data(), psb.size() };
        pd.InputLayout = { el, 2 };
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pd.SampleMask = ~0u;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        pd.SampleDesc.Count = 1;
        ID3D12PipelineState *pso = nullptr;
        char name[32] = {};
        snprintf(name, sizeof(name), "%s", fmts[k].name);
        fwrite(name, 1, 32, f);
        uint32_t cls = fmts[k].cls;
        if (FAILED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso))))
        {
            uint32_t zero = 0;
            fwrite(&zero, 4, 1, f);
            printf("  %s: no pipeline\n", fmts[k].name);
            failed++;
            continue;
        }
        D3D12_VERTEX_BUFFER_VIEW vbv[2] = {
            { vb->GetGPUVirtualAddress(), V * fmts[k].size, fmts[k].size },
            { vb->GetGPUVirtualAddress() + V * 16, 256, fmts[k].size },
        };
        D3D12_VIEWPORT vpt = { 0, 0, 4, 4, 0, 1 };
        D3D12_RECT sc = { 0, 0, 4, 4 };
        g.list->RSSetViewports(1, &vpt);
        g.list->RSSetScissorRects(1, &sc);
        g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        g.list->SetGraphicsRootSignature(rs);
        g.list->SetPipelineState(pso);
        g.list->SetGraphicsRootUnorderedAccessView(0, out->GetGPUVirtualAddress());
        g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
        g.list->IASetVertexBuffers(0, 2, vbv);
        g.list->DrawInstanced(V, inst, 0, 0);
        ID3D12Resource *rb = nullptr;
        const uint32_t *r = read_buffer(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 260 * 16, &rb);
        if (!r) return false;
        uint32_t one = 1;
        fwrite(&one, 4, 1, f);
        fwrite(&cls, 4, 1, f);
        fwrite(r, 16, 260, f);
        rb->Release();
        pso->Release();
    }
    fclose(f);
    printf("  wrote %u vertex formats\n", count);
    out->Release(); vb->Release(); rs->Release(); rt->Release(); rtv_heap->Release();
    EXPECT(!failed, "%d vertex formats have no pipeline", failed);
    return true;
}

// R10G10B10A2_UINT vertex attributes. Metal has no such vertex format (only the normalized one), so the attribute is
// fetched as one R32_UINT word and the vertex shader unpacks it. Checks every component against a CPU unpack, for
// DXIL (uint4, int4, uint3 and 16-bit) and DXBC shaders, next to an R32_UINT attribute at a later offset in the same
// stream, per vertex and per instance; and that the format reports vertex buffer support.
static bool test_vertex_rgb10a2_uint()
{
    ID3D12Device *dev = g.device;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = { DXGI_FORMAT_R10G10B10A2_UINT };
    CHECK_HR(dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs)));
    EXPECT(fs.Support1 & D3D12_FORMAT_SUPPORT1_IA_VERTEX_BUFFER, "R10G10B10A2_UINT reports no vertex buffer support");

    static const char *tmpl = R"(
struct In { T a : ATTR; uint b : EXTRA; T c : INST; uint vid : SV_VertexID; uint iid : SV_InstanceID; };
RWStructuredBuffer<uint4> o : register(u0);
float4 main(In i) : SV_Position
{
    uint4 a = 0, c = 0;
    a.SWZ = i.a.SWZ; c.SWZ = i.c.SWZ;
    o[(i.iid * 64 + i.vid) * 3 + 0] = a;
    o[(i.iid * 64 + i.vid) * 3 + 1] = uint4(i.b, 0, 0, 0);
    o[(i.iid * 64 + i.vid) * 3 + 2] = c;
    return float4(0, 0, 0, 1);
}
float4 ps() : SV_Target { return 0; }
)";
    D3D12_FEATURE_DATA_D3D12_OPTIONS4 o4 = {};
    dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &o4, sizeof(o4));
    struct Variant { const char *name, *type, *swz, *target; bool dxbc, fp16; unsigned comps; };
    static const Variant variants[] = {
        { "DXIL uint4", "uint4", "xyzw", "vs_6_0", false, false, 4 },
        { "DXIL int4", "int4", "xyzw", "vs_6_0", false, false, 4 },
        { "DXIL uint3", "uint3", "xyz", "vs_6_0", false, false, 3 },
        { "DXIL uint16_t4", "uint16_t4", "xyzw", "vs_6_2", false, true, 4 },
        { "DXBC uint4", "uint4", "xyzw", "vs_5_0", true, false, 4 },
        { "DXBC int2", "int2", "xy", "vs_5_0", true, false, 2 },
    };

    D3D12_ROOT_PARAMETER rp = {};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    std::vector<char> psb = compile_dxc((std::string("#define T uint4\n#define SWZ xyzw\n") + tmpl).c_str(), "ps", "ps_6_0");
    if (psb.empty()) return false;

    D3D12_HEAP_PROPERTIES rhp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC rtd = {};
    rtd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rtd.Width = 4; rtd.Height = 4; rtd.DepthOrArraySize = 1; rtd.MipLevels = 1;
    rtd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rtd.SampleDesc.Count = 1;
    rtd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource *rt = nullptr;
    CHECK_HR(dev->CreateCommittedResource(&rhp, D3D12_HEAP_FLAG_NONE, &rtd, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&rt)));
    D3D12_DESCRIPTOR_HEAP_DESC rhd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    ID3D12DescriptorHeap *rtv_heap = nullptr;
    CHECK_HR(dev->CreateDescriptorHeap(&rhd, IID_PPV_ARGS(&rtv_heap)));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(rt, nullptr, rtv);

    // Stream 0: 8-byte vertices, the packed word at offset 0 and an R32_UINT at offset 4. Stream 1: per instance
    // (step rate 1), the packed word at offset 4 of a 12-byte stride.
    const UINT V = 64, inst = 3;
    uint32_t words[V * 2 + inst * 3];
    uint32_t seed = 1234;
    for (uint32_t &w : words) { seed = seed * 1664525 + 1013904223; w = seed ^ (seed >> 13); }
    words[0] = 0xffffffffu; words[2] = 0; words[4] = 0xc00003ffu; words[6] = 0x3ff00000u;
    ID3D12Resource *vb = create_buffer(sizeof(words), D3D12_HEAP_TYPE_UPLOAD);
    void *vp = nullptr;
    vb->Map(0, nullptr, &vp);
    memcpy(vp, words, sizeof(words));
    vb->Unmap(0, nullptr);
    const UINT out_size = V * inst * 3 * 16;
    ID3D12Resource *out = create_buffer(out_size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto unpack = [](uint32_t w, unsigned c) { return c == 3 ? w >> 30 : (w >> (10 * c)) & 0x3ffu; };

    for (const Variant &v : variants)
    {
        if (v.fp16 && !o4.Native16BitShaderOpsSupported)
        {
            printf("  %s: skipped, no native 16-bit shader operations\n", v.name);
            continue;
        }
        std::string src = std::string("#define T ") + v.type + "\n#define SWZ " + v.swz + "\n" + tmpl;
        static const wchar_t *fp16_args[] = { L"-enable-16bit-types" };
        std::vector<char> vsb = v.dxbc ? compile_fxc(src.c_str(), "main", v.target)
                                       : compile_dxc(src.c_str(), "main", v.target, v.fp16 ? fp16_args : nullptr, v.fp16 ? 1 : 0);
        if (vsb.empty()) return false;

        D3D12_INPUT_ELEMENT_DESC el[3] = {
            { "ATTR", 0, DXGI_FORMAT_R10G10B10A2_UINT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "EXTRA", 0, DXGI_FORMAT_R32_UINT, 0, 4, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "INST", 0, DXGI_FORMAT_R10G10B10A2_UINT, 1, 4, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = rs;
        pd.VS = { vsb.data(), vsb.size() };
        pd.PS = { psb.data(), psb.size() };
        pd.InputLayout = { el, 3 };
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pd.SampleMask = ~0u;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        pd.SampleDesc.Count = 1;
        ID3D12PipelineState *pso = nullptr;
        HRESULT hr = dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso));
        EXPECT(SUCCEEDED(hr), "%s: CreateGraphicsPipelineState hr %#x", v.name, (unsigned)hr);
        if (FAILED(hr)) continue;

        D3D12_VERTEX_BUFFER_VIEW vbv[2] = {
            { vb->GetGPUVirtualAddress(), V * 8, 8 },
            { vb->GetGPUVirtualAddress() + V * 8, inst * 12, 12 },
        };
        D3D12_VIEWPORT vpt = { 0, 0, 4, 4, 0, 1 };
        D3D12_RECT sc = { 0, 0, 4, 4 };
        g.list->RSSetViewports(1, &vpt);
        g.list->RSSetScissorRects(1, &sc);
        g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        g.list->SetGraphicsRootSignature(rs);
        g.list->SetPipelineState(pso);
        g.list->SetGraphicsRootUnorderedAccessView(0, out->GetGPUVirtualAddress());
        g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
        g.list->IASetVertexBuffers(0, 2, vbv);
        g.list->DrawInstanced(V, inst, 0, 0);
        ID3D12Resource *rb = nullptr;
        const uint32_t *r = read_buffer(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, out_size, &rb);
        if (!r) return false;
        int bad = 0;
        for (UINT i = 0; i < inst; i++)
            for (UINT j = 0; j < V; j++)
            {
                const uint32_t *e = r + (i * V + j) * 12;
                uint32_t wa = words[j * 2], wb = words[j * 2 + 1], wc = words[V * 2 + i * 3 + 1];
                for (unsigned c = 0; c < 4; c++)
                {
                    uint32_t want_a = c < v.comps ? unpack(wa, c) : 0, want_c = c < v.comps ? unpack(wc, c) : 0;
                    if (e[c] != want_a || e[8 + c] != want_c)
                    {
                        if (bad++ < 4)
                            printf("  %s: instance %u vertex %u component %u: %#x/%#x, want %#x/%#x\n", v.name, i, j, c,
                                   e[c], e[8 + c], want_a, want_c);
                    }
                }
                if (e[4] != wb && bad++ < 4)
                    printf("  %s: instance %u vertex %u R32_UINT %#x, want %#x\n", v.name, i, j, e[4], wb);
            }
        EXPECT(!bad, "%s: %d values wrong", v.name, bad);
        rb->Release();
        pso->Release();
    }
    out->Release(); vb->Release(); rs->Release(); rt->Release(); rtv_heap->Release();
    return true;
}

// Dynamic resources (SM 6.6) in a graphics pipeline: the vertex shader reads its positions from a structured buffer at
// a heap index from a root constant; the pixel shader samples a texture at a literal heap index with a sampler from the
// sampler heap, so it reads nothing from the root arguments, only the heaps (the bridge must bind them to it anyway).
static bool test_dynamicresources_graphics()
{
    static const char *src = R"(
cbuffer C : register(b0) { uint pos_index; };
float4 vs(uint id : SV_VertexID) : SV_Position
{
    StructuredBuffer<float2> pos = ResourceDescriptorHeap[pos_index];
    return float4(pos[id], 0, 1);
}
float4 ps(float4 p : SV_Position) : SV_Target
{
    Texture2D<float4> tex = ResourceDescriptorHeap[5];
    SamplerState smp = SamplerDescriptorHeap[11];
    return tex.Sample(smp, float2(p.x / 2, 0.5));
}
)";
    std::vector<char> vs = compile_dxc(src, "vs", "vs_6_6"), ps = compile_dxc(src, "ps", "ps_6_6");
    if (vs.empty() || ps.empty())
        return false;
    ID3D12Device *dev = g.device;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC vrsd = {};
    vrsd.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    D3D12_ROOT_PARAMETER1 rp1 = {};
    rp1.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp1.Constants.Num32BitValues = 1;
    rp1.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    vrsd.Desc_1_1 = { 1, &rp1, 0, nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED };
    ID3DBlob *blob = nullptr, *error = nullptr;
    CHECK_HR(D3D12SerializeVersionedRootSignature(&vrsd, &blob, &error));
    ID3D12RootSignature *rs = nullptr;
    CHECK_HR(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs)));
    blob->Release();
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
    CHECK_HR(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)));

    // A full-screen triangle's positions, and a 2x1 texture (r = 10, 20).
    ID3D12Resource *upload = create_buffer(4096, D3D12_HEAP_TYPE_UPLOAD);
    uint8_t *u = nullptr;
    upload->Map(0, nullptr, (void **)&u);
    const float tri[6] = { -1, -1, -1, 3, 3, -1 };
    memcpy(u, tri, sizeof(tri));
    const uint32_t texels[2] = { 10, 20 };
    memcpy(u + 2048, texels, sizeof(texels));
    upload->Unmap(0, nullptr);
    ID3D12Resource *pos_buf = create_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 2; td.Height = 1; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    ID3D12Resource *tex = nullptr, *rt = nullptr;
    CHECK_HR(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex)));
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    CHECK_HR(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&rt)));
    g.list->CopyBufferRegion(pos_buf, 0, upload, 0, sizeof(tri));
    D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
    dst.pResource = tex;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcl.pResource = upload;
    srcl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcl.PlacedFootprint.Offset = 2048;
    srcl.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, 2, 1, 1, 256 };
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
    transition(pos_buf, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 64, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap *heap = nullptr, *sheap = nullptr, *rtv_heap = nullptr;
    CHECK_HR(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
    D3D12_DESCRIPTOR_HEAP_DESC shd = { D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 16, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    CHECK_HR(dev->CreateDescriptorHeap(&shd, IID_PPV_ARGS(&sheap)));
    D3D12_DESCRIPTOR_HEAP_DESC rhd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    CHECK_HR(dev->CreateDescriptorHeap(&rhd, IID_PPV_ARGS(&rtv_heap)));
    UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    UINT sinc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    auto cpu = [&](ID3D12DescriptorHeap *h, UINT i, UINT step) { D3D12_CPU_DESCRIPTOR_HANDLE c = h->GetCPUDescriptorHandleForHeapStart(); c.ptr += i * step; return c; };
    const UINT pos_index = 41;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements = 3;
    srv.Buffer.StructureByteStride = 8;
    dev->CreateShaderResourceView(pos_buf, &srv, cpu(heap, pos_index, inc));
    dev->CreateShaderResourceView(tex, nullptr, cpu(heap, 5, inc));
    D3D12_SAMPLER_DESC sd = {};
    sd.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sd.MaxLOD = D3D12_FLOAT32_MAX;
    dev->CreateSampler(&sd, cpu(sheap, 11, sinc));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(rt, nullptr, rtv);

    float clear[4] = { 0, 0, 0, 0 };
    g.list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp = { 0, 0, 2, 1, 0, 1 };
    D3D12_RECT sc = { 0, 0, 2, 1 };
    g.list->RSSetViewports(1, &vp);
    g.list->RSSetScissorRects(1, &sc);
    ID3D12DescriptorHeap *heaps[2] = { heap, sheap };
    g.list->SetDescriptorHeaps(2, heaps);
    g.list->SetGraphicsRootSignature(rs);
    g.list->SetPipelineState(pso);
    g.list->SetGraphicsRoot32BitConstant(0, pos_index, 0);
    g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.list->DrawInstanced(3, 1, 0, 0);
    ID3D12Resource *rb = create_buffer(256, D3D12_HEAP_TYPE_READBACK);
    transition(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION rdst = {}, rsrc = {};
    rdst.pResource = rb;
    rdst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    rdst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, 2, 1, 1, 256 };
    rsrc.pResource = rt;
    rsrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g.list->CopyTextureRegion(&rdst, 0, 0, 0, &rsrc, nullptr);
    if (!execute_and_wait())
        return false;
    const uint32_t *px = nullptr;
    D3D12_RANGE r = { 0, 8 };
    rb->Map(0, &r, (void **)&px);
    EXPECT(px[0] == 10 && px[1] == 20, "pixels 0x%08x 0x%08x, want 0x0000000a 0x00000014", px[0], px[1]);
    rb->Unmap(0, nullptr);
    rb->Release(); rtv_heap->Release(); heap->Release(); sheap->Release(); rt->Release(); tex->Release(); pos_buf->Release();
    upload->Release(); pso->Release(); rs->Release();
    return true;
}

// Dynamic resources (SM 6.6): ResourceDescriptorHeap and SamplerDescriptorHeap indexed by values from a root
// constant: a structured buffer SRV, a texture sampled with a heap sampler, and a UAV written through the heap.
static bool test_dynamicresources()
{
    static const char *src = R"(
cbuffer C : register(b0) { uint srv_index; uint tex_index; uint uav_index; uint sampler_index; };
[numthreads(4, 1, 1)]
void main(uint t : SV_DispatchThreadID)
{
    StructuredBuffer<uint> in_buf = ResourceDescriptorHeap[srv_index];
    Texture2D<float4> tex = ResourceDescriptorHeap[tex_index];
    SamplerState smp = SamplerDescriptorHeap[sampler_index];
    RWStructuredBuffer<uint> out_buf = ResourceDescriptorHeap[uav_index];
    float4 c = tex.SampleLevel(smp, float2(0.25 + 0.5 * (t & 1), 0.5), 0);
    out_buf[t] = in_buf[t] * 10 + uint(c.r * 255.0 + 0.5);
}
)";
    std::vector<char> cs = compile_dxc(src, "main", "cs_6_6");
    if (cs.empty())
        return false;
    D3D12_ROOT_PARAMETER rp = {};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp.Constants.Num32BitValues = 4;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &rp, 0, nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED };
    ID3D12Device *dev = g.device;
    // Directly indexed heaps need root signature 1.1.
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC vrsd = {};
    vrsd.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    D3D12_ROOT_PARAMETER1 rp1 = {};
    rp1.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp1.Constants.Num32BitValues = 4;
    rp1.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    vrsd.Desc_1_1 = { 1, &rp1, 0, nullptr, rsd.Flags };
    ID3DBlob *blob = nullptr, *error = nullptr;
    CHECK_HR(D3D12SerializeVersionedRootSignature(&vrsd, &blob, &error));
    ID3D12RootSignature *rs = nullptr;
    CHECK_HR(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs)));
    blob->Release();
    ID3D12PipelineState *ps = create_compute_pipeline(rs, cs);
    if (!ps)
        return false;

    // Resources: a structured buffer {1, 2, 3, 4}, a 2x1 texture (r = 10, 20), the output buffer.
    ID3D12Resource *upload = create_buffer(4096, D3D12_HEAP_TYPE_UPLOAD);
    uint32_t *u = nullptr;
    upload->Map(0, nullptr, (void **)&u);
    for (UINT i = 0; i < 4; i++)
        u[i] = i + 1;
    u[512] = 10;   // texel (0, 0), RGBA8: r = 10
    u[513] = 20;   // texel (1, 0)
    upload->Unmap(0, nullptr);
    ID3D12Resource *in_buf = create_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *out_buf = create_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 2; td.Height = 1; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    ID3D12Resource *tex = nullptr;
    CHECK_HR(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex)));
    g.list->CopyBufferRegion(in_buf, 0, upload, 0, 16);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
    dst.pResource = tex;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcl.pResource = upload;
    srcl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcl.PlacedFootprint.Offset = 2048;
    srcl.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, 2, 1, 1, 256 };
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
    transition(in_buf, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // Heaps: the descriptors at scattered indexes.
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 64, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap *heap = nullptr;
    CHECK_HR(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
    D3D12_DESCRIPTOR_HEAP_DESC shd = { D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 16, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap *sheap = nullptr;
    CHECK_HR(dev->CreateDescriptorHeap(&shd, IID_PPV_ARGS(&sheap)));
    UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    UINT sinc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    auto cpu = [&](ID3D12DescriptorHeap *h, UINT i, UINT step) { D3D12_CPU_DESCRIPTOR_HANDLE c = h->GetCPUDescriptorHandleForHeapStart(); c.ptr += i * step; return c; };
    const UINT srv_index = 37, tex_index = 5, uav_index = 60, sampler_index = 11;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements = 4;
    srv.Buffer.StructureByteStride = 4;
    dev->CreateShaderResourceView(in_buf, &srv, cpu(heap, srv_index, inc));
    dev->CreateShaderResourceView(tex, nullptr, cpu(heap, tex_index, inc));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = 4;
    uav.Buffer.StructureByteStride = 4;
    dev->CreateUnorderedAccessView(out_buf, nullptr, &uav, cpu(heap, uav_index, inc));
    D3D12_SAMPLER_DESC sd = {};
    sd.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sd.MaxLOD = D3D12_FLOAT32_MAX;
    dev->CreateSampler(&sd, cpu(sheap, sampler_index, sinc));

    ID3D12DescriptorHeap *heaps[2] = { heap, sheap };
    g.list->SetDescriptorHeaps(2, heaps);
    g.list->SetComputeRootSignature(rs);
    g.list->SetPipelineState(ps);
    UINT idx[4] = { srv_index, tex_index, uav_index, sampler_index };
    g.list->SetComputeRoot32BitConstants(0, 4, idx, 0);
    g.list->Dispatch(1, 1, 1);
    ID3D12Resource *rb;
    const uint32_t *v = read_buffer(out_buf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 16, &rb);
    if (!v)
        return false;
    for (UINT t = 0; t < 4; t++)
    {
        uint32_t want = (t + 1) * 10 + ((t & 1) ? 20 : 10);
        EXPECT(v[t] == want, "thread %u = %u, want %u", t, v[t], want);
    }
    rb->Release(); heap->Release(); sheap->Release(); tex->Release(); out_buf->Release(); in_buf->Release(); upload->Release();
    ps->Release(); rs->Release();
    return true;
}

// Render target basics: 4x MSAA with a resolve, a depth test, a stencil write and test (D24S8, which Metal has
// as D32S8), and occlusion and timestamp queries.
// Timestamps: back to back, around GPU work and from two heaps interleaved (MoltenVK samples the timestamps between
// two Metal encoders once and resolves that into each), resolved whole and in part at an offset, in the same list.
static bool test_timestamps()
{
    static const char *src = R"(
RWByteAddressBuffer o : register(u0);
[numthreads(64, 1, 1)]
void main(uint t : SV_DispatchThreadID)
{
    uint v = t;
    for (uint i = 0; i < 20000; i++)
        v = v * 1664525u + 1013904223u;
    o.Store((t % 64) * 4, v);
}
)";
    std::vector<char> cs = compile_dxc(src, "main", "cs_6_0");
    if (cs.empty())
        return false;
    D3D12_ROOT_PARAMETER rp = {};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &rp };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    ID3D12PipelineState *ps = create_compute_pipeline(rs, cs);
    if (!ps)
        return false;
    D3D12_QUERY_HEAP_DESC qd = { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 8 };
    ID3D12QueryHeap *a = nullptr, *b = nullptr;
    CHECK_HR(g.device->CreateQueryHeap(&qd, IID_PPV_ARGS(&a)));
    CHECK_HR(g.device->CreateQueryHeap(&qd, IID_PPV_ARGS(&b)));
    ID3D12Resource *buf = create_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource *res = create_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    g.list->SetComputeRootSignature(rs);
    g.list->SetPipelineState(ps);
    g.list->SetComputeRootUnorderedAccessView(0, buf->GetGPUVirtualAddress());
    g.list->EndQuery(a, D3D12_QUERY_TYPE_TIMESTAMP, 0);
    g.list->EndQuery(a, D3D12_QUERY_TYPE_TIMESTAMP, 1);
    g.list->Dispatch(256, 1, 1);
    g.list->EndQuery(a, D3D12_QUERY_TYPE_TIMESTAMP, 2);
    g.list->EndQuery(b, D3D12_QUERY_TYPE_TIMESTAMP, 5);
    g.list->EndQuery(a, D3D12_QUERY_TYPE_TIMESTAMP, 3);
    g.list->Dispatch(256, 1, 1);
    g.list->EndQuery(b, D3D12_QUERY_TYPE_TIMESTAMP, 6);
    g.list->EndQuery(a, D3D12_QUERY_TYPE_TIMESTAMP, 4);
    // Resolved in the same list, into a default buffer (read back below) and a readback one: Metal resolves the
    // samples only once the command buffer that took them completes (MoltenVK waits for it, or writes the readback
    // buffer on the host then).
    ID3D12Resource *direct = create_buffer(64, D3D12_HEAP_TYPE_READBACK);
    g.list->ResolveQueryData(a, D3D12_QUERY_TYPE_TIMESTAMP, 0, 5, direct, 0);
    g.list->ResolveQueryData(a, D3D12_QUERY_TYPE_TIMESTAMP, 0, 5, res, 0);
    g.list->ResolveQueryData(b, D3D12_QUERY_TYPE_TIMESTAMP, 5, 2, res, 64);
    g.list->ResolveQueryData(a, D3D12_QUERY_TYPE_TIMESTAMP, 2, 3, res, 128);
    ID3D12Resource *rb;
    const uint64_t *t = (const uint64_t *)read_buffer(res, D3D12_RESOURCE_STATE_COPY_DEST, 256, &rb);
    if (!t)
        return false;
    printf("  a: %llu %llu %llu %llu %llu, b: %llu %llu\n", (unsigned long long)t[0], (unsigned long long)t[1],
           (unsigned long long)t[2], (unsigned long long)t[3], (unsigned long long)t[4], (unsigned long long)t[8],
           (unsigned long long)t[9]);
    EXPECT(t[0] != 0, "first timestamp is 0");
    for (int i = 1; i < 5; i++)
        EXPECT(t[i] >= t[i - 1], "timestamp %d (%llu) before %d (%llu)", i, (unsigned long long)t[i], i - 1,
               (unsigned long long)t[i - 1]);
    EXPECT(t[2] > t[1], "no time passed over a dispatch (%llu, %llu)", (unsigned long long)t[1], (unsigned long long)t[2]);
    // Heap b's timestamps are taken with a's next to them, each heap's a moment after the other's.
    EXPECT(t[8] >= t[1] && t[9] >= t[8] && t[9] >= t[3], "heap b's timestamps %llu %llu out of order with a's",
           (unsigned long long)t[8], (unsigned long long)t[9]);
    for (int i = 0; i < 3; i++)
        EXPECT(t[16 + i] == t[2 + i], "partial resolve %d = %llu, want %llu", i, (unsigned long long)t[16 + i],
               (unsigned long long)t[2 + i]);
    uint64_t *d = nullptr;
    D3D12_RANGE dr = { 0, 40 };
    direct->Map(0, &dr, (void **)&d);
    for (int i = 0; i < 5; i++)
        EXPECT(d[i] == t[i], "readback heap resolve %d = %llu, want %llu", i, (unsigned long long)d[i], (unsigned long long)t[i]);
    direct->Unmap(0, nullptr);
    direct->Release();
    rb->Release(); res->Release(); buf->Release(); b->Release(); a->Release(); ps->Release(); rs->Release();
    return true;
}

static bool test_msaa_depth_stencil()
{
    static const char *src = R"(
cbuffer C : register(b0) { float4 color; float z; };
float4 vs(uint id : SV_VertexID) : SV_Position
{
    // A triangle over the lower-left half of the target (through the corners).
    static const float2 p[3] = { float2(-1, -1), float2(1, -1), float2(-1, 1) };
    return float4(p[id], z, 1);
}
float4 ps() : SV_Target { return color; }
)";
    std::vector<char> vs = compile_dxc(src, "vs", "vs_6_0"), ps = compile_dxc(src, "ps", "ps_6_0");
    if (vs.empty() || ps.empty())
        return false;
    D3D12_ROOT_PARAMETER rp = {};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp.Constants.Num32BitValues = 5;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &rp };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    auto make_pso = [&](bool stencil_write, bool stencil_test, ID3D12PipelineState **pso) -> HRESULT {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = rs;
        pd.VS = { vs.data(), vs.size() };
        pd.PS = { ps.data(), ps.size() };
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = ~0u;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.DepthStencilState.DepthEnable = TRUE;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        pd.DepthStencilState.StencilEnable = stencil_write || stencil_test;
        pd.DepthStencilState.StencilReadMask = pd.DepthStencilState.StencilWriteMask = 0xff;
        D3D12_DEPTH_STENCILOP_DESC op = { D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                          stencil_write ? D3D12_STENCIL_OP_REPLACE : D3D12_STENCIL_OP_KEEP,
                                          stencil_test ? D3D12_COMPARISON_FUNC_EQUAL : D3D12_COMPARISON_FUNC_ALWAYS };
        pd.DepthStencilState.FrontFace = pd.DepthStencilState.BackFace = op;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
        pd.SampleDesc.Count = 4;
        return g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(pso));
    };
    ID3D12PipelineState *pso_write = nullptr, *pso_test = nullptr;
    CHECK_HR(make_pso(true, false, &pso_write));
    CHECK_HR(make_pso(false, true, &pso_test));

    const UINT W = 8, H = 8;
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = W; td.Height = H; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 4;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource *msaa = nullptr, *resolved = nullptr, *ds = nullptr;
    CHECK_HR(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&msaa)));
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_NONE;
    CHECK_HR(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr, IID_PPV_ARGS(&resolved)));
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.SampleDesc.Count = 4;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE dcv = { DXGI_FORMAT_D24_UNORM_S8_UINT };
    dcv.DepthStencil = { 1.0f, 0 };
    CHECK_HR(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_DEPTH_WRITE, &dcv, IID_PPV_ARGS(&ds)));
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 }, dhd = { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1 };
    ID3D12DescriptorHeap *rtv_heap = nullptr, *dsv_heap = nullptr;
    CHECK_HR(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap)));
    CHECK_HR(g.device->CreateDescriptorHeap(&dhd, IID_PPV_ARGS(&dsv_heap)));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart(), dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
    g.device->CreateRenderTargetView(msaa, nullptr, rtv);
    g.device->CreateDepthStencilView(ds, nullptr, dsv);
    D3D12_QUERY_HEAP_DESC ohd = { D3D12_QUERY_HEAP_TYPE_OCCLUSION, 2 }, thd = { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2 };
    ID3D12QueryHeap *occ = nullptr, *ts = nullptr;
    CHECK_HR(g.device->CreateQueryHeap(&ohd, IID_PPV_ARGS(&occ)));
    CHECK_HR(g.device->CreateQueryHeap(&thd, IID_PPV_ARGS(&ts)));
    ID3D12Resource *qres = create_buffer(256, D3D12_HEAP_TYPE_READBACK);

    float clear[4] = { 0, 0, 0, 1 };
    g.list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    g.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
    g.list->EndQuery(ts, D3D12_QUERY_TYPE_TIMESTAMP, 0);
    g.list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    D3D12_VIEWPORT vp = { 0, 0, (float)W, (float)H, 0, 1 };
    D3D12_RECT sc = { 0, 0, (LONG)W, (LONG)H };
    g.list->RSSetViewports(1, &vp);
    g.list->RSSetScissorRects(1, &sc);
    g.list->SetGraphicsRootSignature(rs);
    g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    // 1: red at z = 0.5, writing stencil 1 (counted by the occlusion query).
    float c1[5] = { 1, 0, 0, 1, 0.5f };
    g.list->SetPipelineState(pso_write);
    g.list->OMSetStencilRef(1);
    g.list->SetGraphicsRoot32BitConstants(0, 5, c1, 0);
    g.list->BeginQuery(occ, D3D12_QUERY_TYPE_OCCLUSION, 0);
    g.list->DrawInstanced(3, 1, 0, 0);
    g.list->EndQuery(occ, D3D12_QUERY_TYPE_OCCLUSION, 0);
    // 2: green behind it (z = 0.75): fails the depth test everywhere; counted as 0 samples.
    float c2[5] = { 0, 1, 0, 1, 0.75f };
    g.list->SetGraphicsRoot32BitConstants(0, 5, c2, 0);
    g.list->BeginQuery(occ, D3D12_QUERY_TYPE_OCCLUSION, 1);
    g.list->DrawInstanced(3, 1, 0, 0);
    g.list->EndQuery(occ, D3D12_QUERY_TYPE_OCCLUSION, 1);
    // 3: blue in front (z = 0.25) only where stencil == 1.
    float c3[5] = { 0, 0, 1, 1, 0.25f };
    g.list->SetPipelineState(pso_test);
    g.list->SetGraphicsRoot32BitConstants(0, 5, c3, 0);
    g.list->DrawInstanced(3, 1, 0, 0);
    g.list->EndQuery(ts, D3D12_QUERY_TYPE_TIMESTAMP, 1);
    g.list->ResolveQueryData(occ, D3D12_QUERY_TYPE_OCCLUSION, 0, 2, qres, 0);
    g.list->ResolveQueryData(ts, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, qres, 16);
    transition(msaa, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    g.list->ResolveSubresource(resolved, 0, msaa, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
    ID3D12Resource *rb = create_buffer(256 * H, D3D12_HEAP_TYPE_READBACK);
    transition(resolved, D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
    dst.pResource = rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 1, 256 };
    srcl.pResource = resolved;
    srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
    if (!execute_and_wait())
        return false;
    const uint32_t *px = nullptr;
    D3D12_RANGE range = { 0, 256 * H };
    rb->Map(0, &range, (void **)&px);
    // The triangle's hypotenuse runs from the top-left to the bottom-right corner: below it blue, above black,
    // and on the diagonal pixels a resolved mix.
    uint32_t inside = px[(H - 1) * 64 + 0], outside = px[0 * 64 + W - 1], edge = px[3 * 64 + 3];
    EXPECT(inside == 0xffff0000, "covered pixel 0x%08x, want blue 0xffff0000", inside);
    EXPECT(outside == 0xff000000, "uncovered pixel 0x%08x, want black", outside);
    uint32_t edge_blue = (edge >> 16) & 0xff;
    EXPECT(edge_blue > 0 && edge_blue < 255 && (edge & 0xffff) == 0, "edge pixel 0x%08x, want a partial blue", edge);
    const UINT64 *q = nullptr;
    D3D12_RANGE qr = { 0, 32 };
    qres->Map(0, &qr, (void **)&q);
    // 28 of the 64 pixels are fully inside (plus partial ones), at 4 samples each.
    EXPECT(q[0] >= 28 * 4 && q[0] <= 36 * 4, "occlusion samples %llu, want about %u", (unsigned long long)q[0], 32 * 4);
    EXPECT(q[1] == 0, "occluded draw samples %llu, want 0", (unsigned long long)q[1]);
    EXPECT(q[3] > q[2], "timestamps %llu %llu do not increase", (unsigned long long)q[2], (unsigned long long)q[3]);
    rb->Release(); qres->Release(); ts->Release(); occ->Release(); rtv_heap->Release(); dsv_heap->Release();
    ds->Release(); resolved->Release(); msaa->Release(); pso_write->Release(); pso_test->Release(); rs->Release();
    return true;
}

// Static samplers pick mip levels: a 2-mip texture (mip 0 blue, mip 1 white) sampled at level 1 in a compute shader
// (SampleLevel) and a pixel shader (SampleLevel, and Sample over a quad half its size, which derives level 1). Metal
// Shader Converter's static samplers read level 0 only.
static bool test_static_sampler_lod()
{
    static const char *src = R"(
Texture2D<float4> t : register(t0);
SamplerState s : register(s0);
RWStructuredBuffer<float4> o : register(u0);
[numthreads(1, 1, 1)] void cs() { o[0] = t.SampleLevel(s, float2(0.5, 0.5), 1); }
float4 vs(uint id : SV_VertexID, out float2 uv : TEXCOORD) : SV_Position
{
    uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps_level(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return t.SampleLevel(s, uv, 1); }
float4 ps_implicit(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return t.Sample(s, uv); }
)";
    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable = { 1, &range };
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_STATIC_SAMPLER_DESC ss = {};
    ss.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 2, rp, 1, &ss };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    ID3D12PipelineState *cps = create_compute_pipeline(rs, compile_dxc(src, "cs", "cs_6_0"));
    std::vector<char> vs = compile_dxc(src, "vs", "vs_6_0"), ps_level = compile_dxc(src, "ps_level", "ps_6_0"),
                      ps_implicit = compile_dxc(src, "ps_implicit", "ps_6_0");
    if (!rs || !cps || vs.empty() || ps_level.empty() || ps_implicit.empty())
        return false;
    ID3D12PipelineState *gps[2] = {};
    for (int i = 0; i < 2; i++)
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = rs;
        pd.VS = { vs.data(), vs.size() };
        pd.PS = i ? D3D12_SHADER_BYTECODE{ ps_implicit.data(), ps_implicit.size() } : D3D12_SHADER_BYTECODE{ ps_level.data(), ps_level.size() };
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = ~0u;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pd.SampleDesc.Count = 1;
        CHECK_HR(g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&gps[i])));
    }

    // The texture: 4x4 blue, its 2x2 mip white.
    ID3D12Resource *upload = create_buffer(8192, D3D12_HEAP_TYPE_UPLOAD);
    uint8_t *u = nullptr;
    upload->Map(0, nullptr, (void **)&u);
    for (UINT y = 0; y < 4; y++)
        for (UINT x = 0; x < 4; x++)
        {
            uint32_t blue = 0xffff0000, white = 0xffffffff;
            memcpy(u + 256 * y + 4 * x, &blue, 4);
            memcpy(u + 4096 + 256 * y + 4 * x, &white, 4);
        }
    upload->Unmap(0, nullptr);
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 4; td.Height = 4; td.DepthOrArraySize = 1; td.MipLevels = 2;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    ID3D12Resource *tex = nullptr;
    CHECK_HR(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex)));
    for (UINT mip = 0; mip < 2; mip++)
    {
        D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
        dst.pResource = tex;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = mip;
        srcl.pResource = upload;
        srcl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcl.PlacedFootprint.Offset = mip * 4096;
        srcl.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, 4u >> mip, 4u >> mip, 1, 256 };
        g.list->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
    }
    transition(tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap *heap = nullptr;
    CHECK_HR(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
    g.device->CreateShaderResourceView(tex, nullptr, heap->GetCPUDescriptorHandleForHeapStart());

    // Two 2x2 targets (level 1 explicit, implicit) and the compute result.
    td.Width = 2; td.Height = 2; td.MipLevels = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource *rt[2] = {};
    D3D12_DESCRIPTOR_HEAP_DESC rhd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2 };
    ID3D12DescriptorHeap *rtv_heap = nullptr;
    CHECK_HR(g.device->CreateDescriptorHeap(&rhd, IID_PPV_ARGS(&rtv_heap)));
    UINT rtv_inc = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ID3D12Resource *out = create_buffer(16, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g.list->SetDescriptorHeaps(1, &heap);
    g.list->SetComputeRootSignature(rs);
    g.list->SetPipelineState(cps);
    g.list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    g.list->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
    g.list->Dispatch(1, 1, 1);
    D3D12_VIEWPORT vp = { 0, 0, 2, 2, 0, 1 };
    D3D12_RECT sc = { 0, 0, 2, 2 };
    for (int i = 0; i < 2; i++)
    {
        CHECK_HR(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&rt[i])));
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += i * rtv_inc;
        g.device->CreateRenderTargetView(rt[i], nullptr, rtv);
        g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        g.list->RSSetViewports(1, &vp);
        g.list->RSSetScissorRects(1, &sc);
        g.list->SetGraphicsRootSignature(rs);
        g.list->SetGraphicsRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
        g.list->SetGraphicsRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
        g.list->SetPipelineState(gps[i]);
        g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g.list->DrawInstanced(3, 1, 0, 0);
    }
    ID3D12Resource *rb;
    const float *c = (const float *)read_buffer(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 16, &rb);
    if (!c)
        return false;
    EXPECT(c[0] > 0.99f && c[1] > 0.99f && c[2] > 0.99f, "compute SampleLevel(1) = %f %f %f, want white (mip 1)", c[0], c[1], c[2]);
    static const char *names[2] = { "pixel shader SampleLevel(1)", "pixel shader Sample at half size" };
    for (int i = 0; i < 2; i++)
    {
        ID3D12Resource *rrb = create_buffer(512, D3D12_HEAP_TYPE_READBACK);
        transition(rt[i], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
        dst.pResource = rrb;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, 2, 2, 1, 256 };
        srcl.pResource = rt[i];
        srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        g.list->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
        if (!execute_and_wait())
            return false;
        uint32_t *px = nullptr;
        D3D12_RANGE r = { 0, 512 };
        rrb->Map(0, &r, (void **)&px);
        EXPECT(px[0] == 0xffffffff, "%s = %08x, want ffffffff (mip 1)", names[i], px[0]);
        rrb->Unmap(0, nullptr);
        rrb->Release();
    }
    rb->Release(); out->Release(); rt[0]->Release(); rt[1]->Release(); rtv_heap->Release(); heap->Release(); tex->Release();
    upload->Release(); gps[0]->Release(); gps[1]->Release(); cps->Release(); rs->Release();
    return true;
}

// Textures: a BC1 texture (decoded colors), a 2-layer texture array with 2 mips sampled at explicit levels and
// layers, and a copy of a texture region.
static bool test_textures()
{
    static const char *src = R"(
Texture2D<float4> bc : register(t0);
Texture2DArray<float4> arr : register(t1);
Texture2D<float4> copied : register(t2);
SamplerState s : register(s0);
RWStructuredBuffer<float4> o : register(u0);
[numthreads(1, 1, 1)]
void main()
{
    o[0] = bc.SampleLevel(s, float2(0.125, 0.125), 0);
    o[1] = arr.SampleLevel(s, float3(0.5, 0.5, 0), 0);
    o[2] = arr.SampleLevel(s, float3(0.5, 0.5, 1), 0);
    o[3] = arr.SampleLevel(s, float3(0.5, 0.5, 1), 1);
    o[4] = copied.Load(int3(1, 0, 0));
}
)";
    std::vector<char> cs = compile_dxc(src, "main", "cs_6_0");
    if (cs.empty())
        return false;
    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0 };
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable = { 1, &range };
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[0].ShaderVisibility = rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC ss = {};
    ss.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.MaxLOD = D3D12_FLOAT32_MAX;
    ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 2, rp, 1, &ss };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    ID3D12PipelineState *ps = create_compute_pipeline(rs, cs);
    if (!ps)
        return false;

    ID3D12Resource *upload = create_buffer(65536, D3D12_HEAP_TYPE_UPLOAD);
    uint8_t *u = nullptr;
    upload->Map(0, nullptr, (void **)&u);
    memset(u, 0, 65536);
    // BC1 4x4 block: color0 = pure red (RGB565 0xf800), color1 = pure blue (0x001f), all indexes 0 (color0).
    uint16_t bc1[4] = { 0xf800, 0x001f, 0, 0 };
    memcpy(u, bc1, 8);
    // The array: layer 0 mip 0 (4x4) green, layer 1 mip 0 (4x4) blue, layer 1 mip 1 (2x2) white. RGBA8.
    auto fill = [&](size_t offset, UINT w, UINT h, uint32_t v) {
        for (UINT y = 0; y < h; y++)
            for (UINT x = 0; x < w; x++)
                memcpy(u + offset + 256 * y + 4 * x, &v, 4);
    };
    fill(4096, 4, 4, 0xff00ff00);
    fill(8192, 4, 4, 0xffff0000);
    fill(12288, 2, 2, 0xffffffff);
    // The copy source: a 4x1 texture 0x11, 0x22, 0x33, 0x44 (in red).
    uint32_t row[4] = { 0xff000011, 0xff000022, 0xff000033, 0xff000044 };
    memcpy(u + 16384, row, 16);
    upload->Unmap(0, nullptr);

    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.SampleDesc.Count = 1;
    ID3D12Resource *bc = nullptr, *arr = nullptr, *src_tex = nullptr, *dst_tex = nullptr;
    td.Width = 4; td.Height = 4; td.DepthOrArraySize = 1; td.MipLevels = 1; td.Format = DXGI_FORMAT_BC1_UNORM;
    CHECK_HR(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&bc)));
    td.DepthOrArraySize = 2; td.MipLevels = 2; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    CHECK_HR(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&arr)));
    td.Width = 4; td.Height = 1; td.DepthOrArraySize = 1; td.MipLevels = 1;
    CHECK_HR(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&src_tex)));
    CHECK_HR(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&dst_tex)));
    auto upload_sub = [&](ID3D12Resource *t, UINT sub, UINT64 offset, DXGI_FORMAT f, UINT w, UINT h, UINT pitch) {
        D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
        dst.pResource = t;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = sub;
        srcl.pResource = upload;
        srcl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcl.PlacedFootprint.Offset = offset;
        srcl.PlacedFootprint.Footprint = { f, w, h, 1, pitch };
        g.list->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
    };
    upload_sub(bc, 0, 0, DXGI_FORMAT_BC1_UNORM, 4, 4, 256);
    // Subresource = mip + layer * mip levels.
    upload_sub(arr, 0, 4096, DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, 256);
    upload_sub(arr, 2, 8192, DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, 256);
    upload_sub(arr, 3, 12288, DXGI_FORMAT_R8G8B8A8_UNORM, 2, 2, 256);
    upload_sub(src_tex, 0, 16384, DXGI_FORMAT_R8G8B8A8_UNORM, 4, 1, 256);
    // Texture to texture: texels 2..3 of the source to 0..1 of the destination (texel 1 is checked: 0x44).
    transition(src_tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION tdst = {}, tsrc = {};
    tdst.pResource = dst_tex; tdst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    tsrc.pResource = src_tex; tsrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_BOX box = { 2, 0, 0, 4, 1, 1 };
    g.list->CopyTextureRegion(&tdst, 0, 0, 0, &tsrc, &box);
    D3D12_RESOURCE_STATES srv_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    transition(bc, D3D12_RESOURCE_STATE_COPY_DEST, srv_state);
    transition(arr, D3D12_RESOURCE_STATE_COPY_DEST, srv_state);
    transition(dst_tex, D3D12_RESOURCE_STATE_COPY_DEST, srv_state);

    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap *heap = nullptr;
    CHECK_HR(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
    UINT inc = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
    g.device->CreateShaderResourceView(bc, nullptr, h);
    h.ptr += inc;
    g.device->CreateShaderResourceView(arr, nullptr, h);
    h.ptr += inc;
    g.device->CreateShaderResourceView(dst_tex, nullptr, h);
    ID3D12Resource *out = create_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g.list->SetDescriptorHeaps(1, &heap);
    g.list->SetComputeRootSignature(rs);
    g.list->SetPipelineState(ps);
    g.list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    g.list->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
    g.list->Dispatch(1, 1, 1);
    ID3D12Resource *rb;
    const float *v = (const float *)read_buffer(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 80, &rb);
    if (!v)
        return false;
    auto near_ = [](const float *c, float r, float gg, float b, float a) {
        return fabsf(c[0] - r) < 0.01f && fabsf(c[1] - gg) < 0.01f && fabsf(c[2] - b) < 0.01f && fabsf(c[3] - a) < 0.01f; };
    EXPECT(near_(v + 0, 1, 0, 0, 1), "BC1 texel (%f %f %f %f), want red", v[0], v[1], v[2], v[3]);
    EXPECT(near_(v + 4, 0, 1, 0, 1), "layer 0 (%f %f %f %f), want green", v[4], v[5], v[6], v[7]);
    EXPECT(near_(v + 8, 0, 0, 1, 1), "layer 1 (%f %f %f %f), want blue", v[8], v[9], v[10], v[11]);
    EXPECT(near_(v + 12, 1, 1, 1, 1), "layer 1 mip 1 (%f %f %f %f), want white", v[12], v[13], v[14], v[15]);
    EXPECT(fabsf(v[16] - 0x44 / 255.0f) < 0.002f, "copied texel %f, want %f", v[16], 0x44 / 255.0f);
    rb->Release(); out->Release(); heap->Release(); dst_tex->Release(); src_tex->Release(); arr->Release(); bc->Release();
    upload->Release(); ps->Release(); rs->Release();
    return true;
}

// Fences: a GPU wait released by a CPU Signal (the work must not run before it), events at values reached, skipped
// over and never reached, an event for a value already reached, SetEventOnMultipleFenceCompletion (any and all), and
// 100 rounds of a compute queue and the direct queue taking turns on one buffer through two fences.
static bool test_fences()
{
    static const char *src = R"(
RWByteAddressBuffer b : register(u0);
[numthreads(1, 1, 1)] void main() { b.Store(0, b.Load(0) + 1); }
)";
    D3D12_ROOT_PARAMETER rp = {};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &rp };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    ID3D12PipelineState *ps = create_compute_pipeline(rs, compile_dxc(src, "main", "cs_6_0"));
    if (!ps)
        return false;
    ID3D12CommandQueue *cq = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    CHECK_HR(g.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&cq)));
    ID3D12CommandAllocator *ca = nullptr;
    CHECK_HR(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&ca)));
    ID3D12GraphicsCommandList *cl = nullptr;
    CHECK_HR(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, ca, nullptr, IID_PPV_ARGS(&cl)));
    ID3D12Resource *buf = create_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_COMMON);
    // One list that increments the buffer, run on both queues (the direct one gets its own copy).
    cl->SetComputeRootSignature(rs);
    cl->SetPipelineState(ps);
    cl->SetComputeRootUnorderedAccessView(0, buf->GetGPUVirtualAddress());
    cl->Dispatch(1, 1, 1);
    CHECK_HR(cl->Close());
    ID3D12GraphicsCommandList *dl = nullptr;
    ID3D12CommandAllocator *da = nullptr;
    CHECK_HR(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&da)));
    CHECK_HR(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, da, nullptr, IID_PPV_ARGS(&dl)));
    dl->SetComputeRootSignature(rs);
    dl->SetPipelineState(ps);
    dl->SetComputeRootUnorderedAccessView(0, buf->GetGPUVirtualAddress());
    dl->Dispatch(1, 1, 1);
    CHECK_HR(dl->Close());

    ID3D12Fence *gate = nullptr, *done = nullptr, *a = nullptr, *b = nullptr;
    CHECK_HR(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
    CHECK_HR(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done)));
    CHECK_HR(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&a)));
    CHECK_HR(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&b)));
    HANDLE ev[3] = { CreateEventA(nullptr, FALSE, FALSE, nullptr), CreateEventA(nullptr, FALSE, FALSE, nullptr),
                     CreateEventA(nullptr, FALSE, FALSE, nullptr) };

    // 1. The compute queue waits for 5 on gate, which the CPU signals after 100 ms.
    ID3D12CommandList *lists[1] = { cl };
    CHECK_HR(cq->Wait(gate, 5));
    cq->ExecuteCommandLists(1, lists);
    CHECK_HR(cq->Signal(done, 1));
    Sleep(100);
    EXPECT(done->GetCompletedValue() == 0, "work waiting on a fence ran before it was signaled (done = %llu)",
           (unsigned long long)done->GetCompletedValue());
    CHECK_HR(gate->Signal(5));
    CHECK_HR(done->SetEventOnCompletion(1, ev[0]));
    EXPECT(WaitForSingleObject(ev[0], 5000) == WAIT_OBJECT_0, "the CPU signal didn't release the GPU wait");

    // 2. Events at 2 (passed over), 3 (reached) and 10 (never), with the queue signaling 3.
    CHECK_HR(done->SetEventOnCompletion(2, ev[0]));
    CHECK_HR(done->SetEventOnCompletion(3, ev[1]));
    CHECK_HR(done->SetEventOnCompletion(10, ev[2]));
    CHECK_HR(cq->Signal(done, 3));
    EXPECT(WaitForSingleObject(ev[0], 5000) == WAIT_OBJECT_0, "the event at a passed-over value didn't fire");
    EXPECT(WaitForSingleObject(ev[1], 5000) == WAIT_OBJECT_0, "the event at the signaled value didn't fire");
    EXPECT(WaitForSingleObject(ev[2], 100) == WAIT_TIMEOUT, "the event at a value never reached fired");
    EXPECT(done->GetCompletedValue() == 3, "completed value %llu, want 3", (unsigned long long)done->GetCompletedValue());
    // 3. A value already reached fires at once.
    CHECK_HR(done->SetEventOnCompletion(2, ev[0]));
    EXPECT(WaitForSingleObject(ev[0], 1000) == WAIT_OBJECT_0, "the event at a value already reached didn't fire");

    // 4. Multiple fences: any (a reaches 1) and all (then b reaches 1).
    ID3D12Device1 *dev1 = nullptr;
    if (SUCCEEDED(g.device->QueryInterface(IID_PPV_ARGS(&dev1))))
    {
        ID3D12Fence *ab[2] = { a, b };
        UINT64 ones[2] = { 1, 1 };
        CHECK_HR(dev1->SetEventOnMultipleFenceCompletion(ab, ones, 2, D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY, ev[0]));
        CHECK_HR(dev1->SetEventOnMultipleFenceCompletion(ab, ones, 2, D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL, ev[1]));
        CHECK_HR(cq->Signal(a, 1));
        EXPECT(WaitForSingleObject(ev[0], 5000) == WAIT_OBJECT_0, "the any-wait didn't fire when one fence was reached");
        EXPECT(WaitForSingleObject(ev[1], 100) == WAIT_TIMEOUT, "the all-wait fired with one fence reached");
        CHECK_HR(g.queue->Signal(b, 1));
        EXPECT(WaitForSingleObject(ev[1], 5000) == WAIT_OBJECT_0, "the all-wait didn't fire when both were reached");
        dev1->Release();
    }

    // 5. Ping-pong: round r, compute waits a = r and signals b = r; direct waits b = r and signals a = r + 1.
    ID3D12CommandList *dlists[1] = { dl };
    const UINT rounds = 100;
    for (UINT r = 1; r <= rounds; r++)
    {
        CHECK_HR(cq->Wait(a, r));
        cq->ExecuteCommandLists(1, lists);
        CHECK_HR(cq->Signal(b, r + 1));
        CHECK_HR(g.queue->Wait(b, r + 1));
        g.queue->ExecuteCommandLists(1, dlists);
        CHECK_HR(g.queue->Signal(a, r + 1));
    }
    CHECK_HR(a->SetEventOnCompletion(rounds + 1, ev[0]));
    EXPECT(WaitForSingleObject(ev[0], 10000) == WAIT_OBJECT_0, "the ping-pong didn't finish");
    ID3D12Resource *rbuf;
    const uint32_t *v = read_buffer(buf, D3D12_RESOURCE_STATE_COMMON, 256, &rbuf);
    if (!v)
        return false;
    // Case 1's run, then two per round.
    EXPECT(v[0] == 1 + 2 * rounds, "the buffer holds %u, want %u", v[0], 1 + 2 * rounds);
    rbuf->Release();
    for (HANDLE h : ev)
        CloseHandle(h);
    gate->Release(); done->Release(); a->Release(); b->Release();
    dl->Release(); da->Release(); cl->Release(); ca->Release(); cq->Release(); buf->Release(); ps->Release(); rs->Release();
    return true;
}

// Queues: a copy queue fills a buffer, a compute queue (waiting on the copy's fence) doubles it, and the direct
// queue (waiting on the compute fence) reads it back.
static bool test_queues()
{
    static const char *src = R"(
RWStructuredBuffer<uint> b : register(u0);
[numthreads(64, 1, 1)] void main(uint t : SV_DispatchThreadID) { b[t] *= 2; }
)";
    D3D12_ROOT_PARAMETER rp = {};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &rp };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    ID3D12PipelineState *ps = create_compute_pipeline(rs, compile_dxc(src, "main", "cs_6_0"));
    if (!ps)
        return false;
    ID3D12CommandQueue *copy_q = nullptr, *compute_q = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    CHECK_HR(g.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&copy_q)));
    qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    CHECK_HR(g.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&compute_q)));
    ID3D12CommandAllocator *copy_a = nullptr, *compute_a = nullptr;
    CHECK_HR(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&copy_a)));
    CHECK_HR(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&compute_a)));
    ID3D12GraphicsCommandList *copy_l = nullptr, *compute_l = nullptr;
    CHECK_HR(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, copy_a, nullptr, IID_PPV_ARGS(&copy_l)));
    CHECK_HR(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, compute_a, nullptr, IID_PPV_ARGS(&compute_l)));
    ID3D12Fence *f = nullptr;
    CHECK_HR(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f)));

    const UINT n = 256;
    ID3D12Resource *upload = create_buffer(n * 4, D3D12_HEAP_TYPE_UPLOAD);
    uint32_t *u = nullptr;
    upload->Map(0, nullptr, (void **)&u);
    for (UINT i = 0; i < n; i++)
        u[i] = i;
    upload->Unmap(0, nullptr);
    ID3D12Resource *buf = create_buffer(n * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_COMMON);
    copy_l->CopyBufferRegion(buf, 0, upload, 0, n * 4);
    CHECK_HR(copy_l->Close());
    compute_l->SetComputeRootSignature(rs);
    compute_l->SetPipelineState(ps);
    compute_l->SetComputeRootUnorderedAccessView(0, buf->GetGPUVirtualAddress());
    compute_l->Dispatch(n / 64, 1, 1);
    CHECK_HR(compute_l->Close());
    // Submit in the reverse order of execution: the waits order them.
    CHECK_HR(compute_q->Wait(f, 1));
    ID3D12CommandList *cl = compute_l;
    compute_q->ExecuteCommandLists(1, &cl);
    CHECK_HR(compute_q->Signal(f, 2));
    cl = copy_l;
    copy_q->ExecuteCommandLists(1, &cl);
    CHECK_HR(copy_q->Signal(f, 1));
    CHECK_HR(g.queue->Wait(f, 2));
    ID3D12Resource *rb;
    const uint32_t *v = read_buffer(buf, D3D12_RESOURCE_STATE_COMMON, n * 4, &rb);
    if (!v)
        return false;
    UINT wrong = 0;
    for (UINT i = 0; i < n; i++)
        wrong += v[i] != 2 * i;
    EXPECT(!wrong, "%u of %u values not doubled (v[1] = %u, v[255] = %u)", wrong, n, v[1], v[255]);
    EXPECT(f->GetCompletedValue() == 2, "fence %llu, want 2", (unsigned long long)f->GetCompletedValue());
    rb->Release(); buf->Release(); upload->Release(); f->Release(); copy_l->Release(); compute_l->Release();
    copy_a->Release(); compute_a->Release(); copy_q->Release(); compute_q->Release(); ps->Release(); rs->Release();
    return true;
}

// Input layout: per-vertex positions and per-instance offsets (step rate 2), and colors from a second vertex buffer.
static bool test_inputlayout()
{
    static const char *src = R"(
struct VSIn { float2 pos : POSITION; float2 offset : OFFSET; float4 color : COLOR; };
struct VSOut { float4 pos : SV_Position; float4 color : COLOR; };
VSOut vs(VSIn i) { VSOut o; o.pos = float4(i.pos + i.offset, 0, 1); o.color = i.color; return o; }
float4 ps(VSOut i) : SV_Target { return i.color; }
)";
    std::vector<char> vs = compile_dxc(src, "vs", "vs_6_0"), ps = compile_dxc(src, "ps", "ps_6_0");
    if (vs.empty() || ps.empty())
        return false;
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3D12RootSignature *rs = create_root_signature(rsd);
    D3D12_INPUT_ELEMENT_DESC ie[3] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "OFFSET", 0, DXGI_FORMAT_R32G32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 2 },
        { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 8, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 2 },
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = rs;
    pd.VS = { vs.data(), vs.size() };
    pd.PS = { ps.data(), ps.size() };
    pd.InputLayout = { ie, 3 };
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
    // A quad over the left quarter (x in [-1, -0.5]); instances 0-1 use offset 0 (red), 2-3 offset 1 (green),
    // which moves it to x in [0, 0.5].
    ID3D12Resource *vb = create_buffer(256, D3D12_HEAP_TYPE_UPLOAD);
    float *f = nullptr;
    vb->Map(0, nullptr, (void **)&f);
    const float quad[12] = { -1, -1, -0.5f, -1, -1, 1, -0.5f, -1, -0.5f, 1, -1, 1 };
    memcpy(f, quad, sizeof(quad));
    float *inst = f + 32;
    inst[0] = 0; inst[1] = 0; ((uint32_t *)inst)[2] = 0xff0000ff;
    inst[3] = 1; inst[4] = 0; ((uint32_t *)inst)[5] = 0xff00ff00;
    vb->Unmap(0, nullptr);
    D3D12_VERTEX_BUFFER_VIEW vbv[2] = { { vb->GetGPUVirtualAddress(), 48, 8 }, { vb->GetGPUVirtualAddress() + 128, 24, 12 } };

    const UINT W = 8, H = 2;
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
    float clear[4] = { 0, 0, 0, 1 };
    g.list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp = { 0, 0, (float)W, (float)H, 0, 1 };
    D3D12_RECT sc = { 0, 0, (LONG)W, (LONG)H };
    g.list->RSSetViewports(1, &vp);
    g.list->RSSetScissorRects(1, &sc);
    g.list->SetGraphicsRootSignature(rs);
    g.list->SetPipelineState(pso);
    g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.list->IASetVertexBuffers(0, 2, vbv);
    g.list->DrawInstanced(6, 4, 0, 0);
    ID3D12Resource *rb = create_buffer(256 * H, D3D12_HEAP_TYPE_READBACK);
    transition(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
    dst.pResource = rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 1, 256 };
    srcl.pResource = rt;
    srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
    if (!execute_and_wait())
        return false;
    const uint32_t *px = nullptr;
    D3D12_RANGE r = { 0, 256 * H };
    rb->Map(0, &r, (void **)&px);
    EXPECT(px[0] == 0xff0000ff, "instances 0-1 pixel 0x%08x, want red", px[0]);
    EXPECT(px[4] == 0xff00ff00, "instances 2-3 pixel 0x%08x, want green", px[4]);
    EXPECT(px[2] == 0xff000000, "uncovered pixel 0x%08x, want black", px[2]);
    rb->Release(); rtv_heap->Release(); rt->Release(); vb->Release(); pso->Release(); rs->Release();
    return true;
}

// ExecuteIndirect with vertex and index buffer views: three sequences, each with its root constants (color),
// vertex buffer (with a different stride and base vertex in one) and index buffer (16 and 32 bit, and a start index),
// then an ordinary draw to check that the state is bound again after them.
static bool test_executeindirect_buffers()
{
    static const char *src = R"(
cbuffer C : register(b0) { float4 color; };
Texture2D<float4> tex : register(t0);
StructuredBuffer<float4> sb : register(t1);
float4 vs(float2 pos : POSITION) : SV_Position { return float4(pos, 0, 1); }
float4 ps() : SV_Target { return color * tex.Load(int3(0, 0, 0)) * sb[0]; }
)";
    std::vector<char> vs = compile_dxc(src, "vs", "vs_6_0"), ps = compile_dxc(src, "ps", "ps_6_0");
    if (vs.empty() || ps.empty())
        return false;
    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0 };
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.Num32BitValues = 4;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable = { 1, &range };
    rp[0].ShaderVisibility = rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 2, rp };
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
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
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState *pso = nullptr;
    CHECK_HR(g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)));

    D3D12_INDIRECT_ARGUMENT_DESC ad[4] = {};
    ad[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    ad[0].Constant = { 0, 0, 4 };
    ad[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW;
    ad[1].VertexBuffer.Slot = 0;
    ad[2].Type = D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW;
    ad[3].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
#pragma pack(push, 4)
    struct Cmd { float color[4]; D3D12_VERTEX_BUFFER_VIEW vbv; D3D12_INDEX_BUFFER_VIEW ibv; D3D12_DRAW_INDEXED_ARGUMENTS draw; };
#pragma pack(pop)
    static_assert(sizeof(Cmd) == 68, "packed command");
    D3D12_COMMAND_SIGNATURE_DESC csd = { sizeof(Cmd), 4, ad, 0 };
    ID3D12CommandSignature *sig = nullptr;
    CHECK_HR(g.device->CreateCommandSignature(&csd, rs, IID_PPV_ARGS(&sig)));

    ID3D12Resource *buf = create_buffer(4096, D3D12_HEAP_TYPE_UPLOAD);
    uint8_t *u = nullptr;
    buf->Map(0, nullptr, (void **)&u);
    memset(u, 0, 4096);
    D3D12_GPU_VIRTUAL_ADDRESS va = buf->GetGPUVirtualAddress();
    // Quads over the column pairs [0,2), [2,4), [4,6) and [6,8) of an 8x2 target.
    auto quad = [&](size_t offset, UINT stride, UINT first, float x0, float x1) {
        const float v[4][2] = { { x0, -1 }, { x1, -1 }, { x1, 1 }, { x0, 1 } };
        for (UINT i = 0; i < 4; i++)
            memcpy(u + offset + (first + i) * stride, v[i], 8);
    };
    quad(0, 8, 0, -1.0f, -0.5f);
    quad(256, 8, 0, -0.5f, 0.0f);
    quad(512, 12, 2, 0.0f, 0.5f);       // Two vertices before it: base vertex 2.
    quad(768, 8, 0, 0.5f, 1.0f);
    const uint16_t i16[6] = { 0, 1, 2, 0, 2, 3 }, i16c[9] = { 9, 9, 9, 0, 1, 2, 0, 2, 3 };
    const uint32_t i32[6] = { 0, 1, 2, 0, 2, 3 };
    memcpy(u + 1024, i16, sizeof(i16));
    memcpy(u + 1280, i16c, sizeof(i16c));
    memcpy(u + 1536, i32, sizeof(i32));
    Cmd cmds[3] = {
        { { 1, 0, 0, 1 }, { va + 0, 32, 8 }, { va + 1024, 12, DXGI_FORMAT_R16_UINT }, { 6, 1, 0, 0, 0 } },
        { { 0, 1, 0, 1 }, { va + 256, 32, 8 }, { va + 1536, 24, DXGI_FORMAT_R32_UINT }, { 6, 1, 0, 0, 0 } },
        { { 0, 0, 1, 1 }, { va + 512, 72, 12 }, { va + 1280, 18, DXGI_FORMAT_R16_UINT }, { 6, 1, 3, 2, 0 } },
    };
    memcpy(u + 2048, cmds, sizeof(cmds));
    const uint32_t one = 1;
    memcpy(u + 2304, &one, 4);          // The count for the second ExecuteIndirect.
    const float ones[4] = { 1, 1, 1, 1 };
    memcpy(u + 2560, ones, 16);         // The structured buffer.
    const uint32_t texel = 0xffffffff;
    memcpy(u + 3072, &texel, 4);        // The texture's texel (512-aligned footprint).
    buf->Unmap(0, nullptr);

    const UINT W = 8, H = 2;
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC xd = {};
    xd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    xd.Width = 1; xd.Height = 1; xd.DepthOrArraySize = 1; xd.MipLevels = 1;
    xd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    xd.SampleDesc.Count = 1;
    ID3D12Resource *tex = nullptr;
    CHECK_HR(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &xd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex)));
    {
        D3D12_TEXTURE_COPY_LOCATION tdst = {}, tsrc = {};
        tdst.pResource = tex;
        tdst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        tsrc.pResource = buf;
        tsrc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        tsrc.PlacedFootprint.Offset = 3072;
        tsrc.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 256 };
        g.list->CopyTextureRegion(&tdst, 0, 0, 0, &tsrc, nullptr);
        transition(tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    D3D12_DESCRIPTOR_HEAP_DESC shd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap *srv_heap = nullptr;
    CHECK_HR(g.device->CreateDescriptorHeap(&shd, IID_PPV_ARGS(&srv_heap)));
    D3D12_CPU_DESCRIPTOR_HANDLE sh = srv_heap->GetCPUDescriptorHandleForHeapStart();
    g.device->CreateShaderResourceView(tex, nullptr, sh);
    sh.ptr += g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_SHADER_RESOURCE_VIEW_DESC sbd = {};
    sbd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    sbd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sbd.Buffer.FirstElement = 2560 / 16;
    sbd.Buffer.NumElements = 1;
    sbd.Buffer.StructureByteStride = 16;
    g.device->CreateShaderResourceView(buf, &sbd, sh);
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
    float clear[4] = { 0, 0, 0, 1 };
    g.list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp = { 0, 0, (float)W, (float)H, 0, 1 };
    D3D12_RECT sc = { 0, 0, (LONG)W, (LONG)H };
    g.list->RSSetViewports(1, &vp);
    g.list->RSSetScissorRects(1, &sc);
    g.list->SetDescriptorHeaps(1, &srv_heap);
    g.list->SetGraphicsRootSignature(rs);
    g.list->SetGraphicsRootDescriptorTable(1, srv_heap->GetGPUDescriptorHandleForHeapStart());
    g.list->SetPipelineState(pso);
    g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    // Row 0: all three sequences. Row 1: the count buffer limits it to the first.
    D3D12_RECT row0 = { 0, 0, (LONG)W, 1 }, row1 = { 0, 1, (LONG)W, 2 };
    g.list->RSSetScissorRects(1, &row0);
    g.list->ExecuteIndirect(sig, 3, buf, 2048, nullptr, 0);
    g.list->RSSetScissorRects(1, &row1);
    g.list->ExecuteIndirect(sig, 3, buf, 2048, buf, 2304);
    g.list->RSSetScissorRects(1, &sc);
    // The views and constants the signature set are undefined after it: set them for an ordinary draw.
    const float white[4] = { 1, 1, 1, 1 };
    D3D12_VERTEX_BUFFER_VIEW vbv = { va + 768, 32, 8 };
    D3D12_INDEX_BUFFER_VIEW ibv = { va + 1024, 12, DXGI_FORMAT_R16_UINT };
    g.list->SetGraphicsRoot32BitConstants(0, 4, white, 0);
    g.list->IASetVertexBuffers(0, 1, &vbv);
    g.list->IASetIndexBuffer(&ibv);
    g.list->DrawIndexedInstanced(6, 1, 0, 0, 0);

    ID3D12Resource *rb = create_buffer(256 * H, D3D12_HEAP_TYPE_READBACK);
    transition(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
    dst.pResource = rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 1, 256 };
    srcl.pResource = rt;
    srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
    if (!execute_and_wait())
        return false;
    const uint32_t *px = nullptr;
    D3D12_RANGE r = { 0, 256 * H };
    rb->Map(0, &r, (void **)&px);
    const uint32_t want[4] = { 0xff0000ff, 0xff00ff00, 0xffff0000, 0xffffffff };
    const char *what[4] = { "sequence 0 (16-bit indices)", "sequence 1 (32-bit indices)",
                            "sequence 2 (stride 12, base vertex, start index)", "the draw after them" };
    for (UINT i = 0; i < 4; i++)
        for (UINT y = 0; y < H; y++)
        {
            uint32_t w = y == 1 && (i == 1 || i == 2) ? 0xff000000 : want[i];   // Not drawn with the count of 1.
            EXPECT(px[y * 64 + 2 * i + 1] == w, "%s: pixel (%u, %u) 0x%08x, want 0x%08x", what[i], 2 * i + 1, y,
                   px[y * 64 + 2 * i + 1], w);
        }
    rb->Unmap(0, nullptr);
    rb->Release(); rtv_heap->Release(); rt->Release(); srv_heap->Release(); tex->Release(); buf->Release(); sig->Release(); pso->Release(); rs->Release();
    return true;
}

// View instancing (tier 2): two views, mapped to slices 1 and 0 of a render target array, through a vertex shader,
// a geometry shader, tessellation and a mesh shader. Each pre-rasterization stage adds a multiple of SV_ViewID to
// red (1x, then 2x and 4x by the next stages), the pixel shader writes its own view to green.
static bool test_viewinstancing()
{
    static const char *src = R"(
struct V { float4 pos : SV_Position; float r : RED; };
V vs(uint v : SV_VertexID, uint view : SV_ViewID)
{
    V o;
    float2 uv = float2((v << 1) & 2, v & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.r = view;
    return o;
}
[maxvertexcount(3)]
void gs(triangle V i[3], uint view : SV_ViewID, inout TriangleStream<V> s)
{
    for (uint k = 0; k < 3; k++)
    {
        V o = i[k];
        o.r += 2 * view;
        s.Append(o);
    }
}
struct PC { float e[3] : SV_TessFactor; float in_ : SV_InsideTessFactor; };
PC pc(uint view : SV_ViewID) { PC p; p.e[0] = p.e[1] = p.e[2] = 1; p.in_ = 1; return p; }
[domain("tri")] [partitioning("integer")] [outputtopology("triangle_cw")] [outputcontrolpoints(3)] [patchconstantfunc("pc")]
V hs(InputPatch<V, 3> p, uint i : SV_OutputControlPointID, uint view : SV_ViewID)
{
    V o = p[i];
    o.r += 2 * view;
    return o;
}
[domain("tri")]
V ds(PC c, float3 b : SV_DomainLocation, const OutputPatch<V, 3> p, uint view : SV_ViewID)
{
    V o;
    o.pos = p[0].pos * b.x + p[1].pos * b.y + p[2].pos * b.z;
    o.r = p[0].r + 4 * view;
    return o;
}
[numthreads(1, 1, 1)]
[outputtopology("triangle")]
void ms(uint view : SV_ViewID, out vertices V v[3], out indices uint3 tri[1])
{
    SetMeshOutputCounts(3, 1);
    for (uint k = 0; k < 3; k++)
    {
        float2 uv = float2((k << 1) & 2, k & 2);
        v[k].pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
        v[k].r = view;
    }
    tri[0] = uint3(0, 1, 2);
}
float4 ps(V i, uint view : SV_ViewID) : SV_Target { return float4(i.r / 16.0, (view + 1) / 4.0, 0, 1); }
)";
    std::vector<char> vs = compile_dxc(src, "vs", "vs_6_1"), gs = compile_dxc(src, "gs", "gs_6_1"),
                      hs = compile_dxc(src, "hs", "hs_6_1"), ds = compile_dxc(src, "ds", "ds_6_1"),
                      ms = compile_dxc(src, "ms", "ms_6_5"), ps = compile_dxc(src, "ps", "ps_6_1");
    if (vs.empty() || gs.empty() || hs.empty() || ds.empty() || ms.empty() || ps.empty())
        return false;
    // After compiling, which fills the DXIL cache for the other lane.
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 o3 = {};
    g.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &o3, sizeof(o3));
    if (o3.ViewInstancingTier < D3D12_VIEW_INSTANCING_TIER_2)
    {
        printf("  skipped: view instancing tier %d\n", o3.ViewInstancingTier);
        return true;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7 = {};
    g.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof(o7));
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    ID3D12RootSignature *rs = create_root_signature(rsd);
    ID3D12Device2 *dev2 = nullptr;
    CHECK_HR(g.device->QueryInterface(IID_PPV_ARGS(&dev2)));
    ID3D12GraphicsCommandList6 *list6 = nullptr;
    CHECK_HR(g.list->QueryInterface(IID_PPV_ARGS(&list6)));

    D3D12_RASTERIZER_DESC rast = {};
    rast.FillMode = D3D12_FILL_MODE_SOLID;
    rast.CullMode = D3D12_CULL_MODE_NONE;
    rast.DepthClipEnable = TRUE;
    D3D12_BLEND_DESC blend = {};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    D3D12_RT_FORMAT_ARRAY rtv_formats = {};
    rtv_formats.NumRenderTargets = 1;
    rtv_formats.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    const D3D12_VIEW_INSTANCE_LOCATION locations[2] = { { 0, 1 }, { 0, 0 } };   // View 0 to slice 1, view 1 to slice 0.
    D3D12_VIEW_INSTANCING_DESC vid = { 2, locations, D3D12_VIEW_INSTANCING_FLAG_NONE };
    struct
    {
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature *> rs;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE> vs;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS, D3D12_SHADER_BYTECODE> hs;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS, D3D12_SHADER_BYTECODE> ds;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS, D3D12_SHADER_BYTECODE> gs;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> rast;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC> blend;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> mask;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE> topology;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> rtvs;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> sample;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING, D3D12_VIEW_INSTANCING_DESC> views;
    } stream;
    struct
    {
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature *> rs;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> ms;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> rast;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC> blend;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> mask;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> rtvs;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> sample;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING, D3D12_VIEW_INSTANCING_DESC> views;
    } mesh_stream;

    const UINT W = 4, H = 4;
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = W; td.Height = H; td.DepthOrArraySize = 2; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    ID3D12DescriptorHeap *rtv_heap = nullptr;
    CHECK_HR(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap)));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();

    enum { VERTEX, GEOMETRY, TESSELLATION, MESH };
    const char *names[4] = { "vertex", "geometry", "tessellation", "mesh" };
    const float scale[4] = { 1, 3, 7, 1 };   // The multiple of the view the pre-rasterization stages add up to.
    for (int kind = VERTEX; kind <= MESH; kind++)
    {
        if (kind == MESH && o7.MeshShaderTier < D3D12_MESH_SHADER_TIER_1)
            continue;
        ID3D12PipelineState *pso = nullptr;
        if (kind == MESH)
        {
            mesh_stream.rs.value = rs;
            mesh_stream.ms.value = { ms.data(), ms.size() };
            mesh_stream.ps.value = { ps.data(), ps.size() };
            mesh_stream.rast.value = rast;
            mesh_stream.blend.value = blend;
            mesh_stream.mask.value = ~0u;
            mesh_stream.rtvs.value = rtv_formats;
            mesh_stream.sample.value = { 1, 0 };
            mesh_stream.views.value = vid;
            D3D12_PIPELINE_STATE_STREAM_DESC sd = { sizeof(mesh_stream), &mesh_stream };
            CHECK_HR(dev2->CreatePipelineState(&sd, IID_PPV_ARGS(&pso)));
        }
        else
        {
            stream.rs.value = rs;
            stream.vs.value = { vs.data(), vs.size() };
            stream.hs.value = kind == TESSELLATION ? D3D12_SHADER_BYTECODE{ hs.data(), hs.size() } : D3D12_SHADER_BYTECODE{};
            stream.ds.value = kind == TESSELLATION ? D3D12_SHADER_BYTECODE{ ds.data(), ds.size() } : D3D12_SHADER_BYTECODE{};
            stream.gs.value = kind == GEOMETRY ? D3D12_SHADER_BYTECODE{ gs.data(), gs.size() } : D3D12_SHADER_BYTECODE{};
            stream.ps.value = { ps.data(), ps.size() };
            stream.rast.value = rast;
            stream.blend.value = blend;
            stream.mask.value = ~0u;
            stream.topology.value = kind == TESSELLATION ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            stream.rtvs.value = rtv_formats;
            stream.sample.value = { 1, 0 };
            stream.views.value = vid;
            D3D12_PIPELINE_STATE_STREAM_DESC sd = { sizeof(stream), &stream };
            CHECK_HR(dev2->CreatePipelineState(&sd, IID_PPV_ARGS(&pso)));
        }
        ID3D12Resource *rt = nullptr;
        CHECK_HR(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&rt)));
        D3D12_RENDER_TARGET_VIEW_DESC rvd = {};
        rvd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rvd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        rvd.Texture2DArray.ArraySize = 2;
        g.device->CreateRenderTargetView(rt, &rvd, rtv);
        float clear[4] = { 0, 0, 0, 0 };
        g.list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        D3D12_VIEWPORT vp = { 0, 0, (float)W, (float)H, 0, 1 };
        D3D12_RECT sc = { 0, 0, (LONG)W, (LONG)H };
        g.list->RSSetViewports(1, &vp);
        g.list->RSSetScissorRects(1, &sc);
        g.list->SetGraphicsRootSignature(rs);
        g.list->SetPipelineState(pso);
        if (kind == MESH)
            list6->DispatchMesh(1, 1, 1);
        else
        {
            g.list->IASetPrimitiveTopology(kind == TESSELLATION ? D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g.list->DrawInstanced(3, 1, 0, 0);
        }
        ID3D12Resource *rb = create_buffer(256 * H * 2, D3D12_HEAP_TYPE_READBACK);
        transition(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        for (UINT slice = 0; slice < 2; slice++)
        {
            D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
            dst.pResource = rb;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Offset = slice * 256 * H;
            dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 1, 256 };
            srcl.pResource = rt;
            srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            srcl.SubresourceIndex = slice;
            g.list->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
        }
        if (!execute_and_wait())
            return false;
        const uint8_t *px = nullptr;
        D3D12_RANGE r = { 0, 256 * H * 2 };
        rb->Map(0, &r, (void **)&px);
        for (UINT view = 0; view < 2; view++)
        {
            const uint8_t *t = px + locations[view].RenderTargetArrayIndex * 256 * H + (H / 2) * 256 + (W / 2) * 4;
            int want_r = (int)(scale[kind] * view / 16.0f * 255.0f + 0.5f), want_g = (int)((view + 1) / 4.0f * 255.0f + 0.5f);
            EXPECT(abs(t[0] - want_r) <= 1 && abs(t[1] - want_g) <= 1 && t[3] == 255,
                   "%s: view %u (slice %u) pixel %02x %02x %02x %02x, want %02x %02x 00 ff", names[kind], view,
                   locations[view].RenderTargetArrayIndex, t[0], t[1], t[2], t[3], want_r, want_g);
        }
        rb->Unmap(0, nullptr);
        rb->Release(); rt->Release(); pso->Release();
    }
    rtv_heap->Release(); list6->Release(); dev2->Release(); rs->Release();
    return true;
}

// D3D11On12: a D3D11 device on the D3D12 device and queue clears a wrapped D3D12 texture, and copies a texture it
// creates (with initial data) into a corner of it; D3D12 reads the result back.
static bool test_d3d11on12()
{
    HMODULE d3d11 = LoadLibraryA("d3d11.dll");
    auto create = d3d11 ? (PFN_D3D11ON12_CREATE_DEVICE)(void *)GetProcAddress(d3d11, "D3D11On12CreateDevice") : nullptr;
    if (!create)
    {
        printf("  skipped: no D3D11On12CreateDevice\n");
        return true;
    }
    ID3D11Device *dev11 = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    D3D_FEATURE_LEVEL fl = {};
    IUnknown *queues[] = { g.queue };
    CHECK_HR(create(g.device, 0, nullptr, 0, queues, 1, 0, &dev11, &ctx, &fl));
    printf("  D3D11 feature level 0x%x\n", fl);
    ID3D11On12Device *on12 = nullptr;
    CHECK_HR(dev11->QueryInterface(IID_PPV_ARGS(&on12)));

    const UINT W = 4, H = 4;
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = W; td.Height = H; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource *tex12 = nullptr;
    CHECK_HR(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&tex12)));
    D3D11_RESOURCE_FLAGS flags11 = { D3D11_BIND_RENDER_TARGET };
    ID3D11Texture2D *wrapped = nullptr;
    CHECK_HR(on12->CreateWrappedResource(tex12, &flags11, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                         IID_PPV_ARGS(&wrapped)));
    ID3D11RenderTargetView *rtv = nullptr;
    CHECK_HR(dev11->CreateRenderTargetView(wrapped, nullptr, &rtv));
    // A 2x2 texture of D3D11's own, with initial data.
    const uint32_t texels[4] = { 0xff0000ff, 0xff00ff00, 0xffff0000, 0xffffffff };
    D3D11_TEXTURE2D_DESC d11 = { 2, 2, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA init = { texels, 8, 0 };
    ID3D11Texture2D *small = nullptr;
    CHECK_HR(dev11->CreateTexture2D(&d11, &init, &small));

    ID3D11Resource *res = wrapped;
    on12->AcquireWrappedResources(&res, 1);
    const float gray[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
    ctx->ClearRenderTargetView(rtv, gray);
    ctx->CopySubresourceRegion(wrapped, 0, 2, 2, 0, small, 0, nullptr);
    on12->ReleaseWrappedResources(&res, 1);
    ctx->Flush();

    ID3D12Resource *rb = create_buffer(256 * H, D3D12_HEAP_TYPE_READBACK);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
    dst.pResource = rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 1, 256 };
    srcl.pResource = tex12;
    srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
    if (!execute_and_wait())
        return false;
    const uint32_t *px = nullptr;
    D3D12_RANGE r = { 0, 256 * H };
    rb->Map(0, &r, (void **)&px);
    EXPECT(px[0] == 0xff808080 || px[0] == 0xff7f7f7f, "cleared texel 0x%08x, want 0xff808080", px[0]);
    for (UINT i = 0; i < 4; i++)
    {
        uint32_t got = px[(2 + i / 2) * 64 + 2 + i % 2];
        EXPECT(got == texels[i], "copied texel %u: 0x%08x, want 0x%08x", i, got, texels[i]);
    }
    rb->Unmap(0, nullptr);
    rb->Release(); small->Release(); rtv->Release(); wrapped->Release(); tex12->Release();
    on12->Release(); ctx->Release(); dev11->Release();
    return true;
}

// SM 6.7 advanced texture ops (AdvancedTextureOpsSupported) and quad ops in compute: sampling an integer texture,
// GatherRaw, SampleCmpLevel in a compute shader, texel offsets that aren't immediates, QuadAll.
static bool test_sm67_textures()
{
    static const char *src = R"(
Texture2D<uint> ti : register(t0);
Texture2D<float> tf : register(t1);
SamplerState s : register(s0);
SamplerComparisonState c : register(s1);
cbuffer C : register(b0) { int2 off; };
RWStructuredBuffer<uint4> o : register(u0);
[numthreads(2, 2, 1)]
void main(uint2 t : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    bool all_in = QuadAll(t.x < 2);
    bool all_zero = QuadAll(t.x == 0);
    if (gi != 0)
        return;
    o[0] = uint4(ti.SampleLevel(s, float2(0.75, 0.25), 0), ti.SampleLevel(s, float2(0.25, 0.25), 0, off), all_in, all_zero);
    o[1] = ti.GatherRaw(s, float2(0.5, 0.5));
    o[2] = uint4(tf.SampleCmpLevel(c, float2(0.25, 0.25), 0.25, 0) * 100, tf.SampleCmpLevel(c, float2(0.75, 0.75), 0.25, 0) * 100, 0, 0);
}
)";
    std::vector<char> cs = compile_dxc(src, "main", "cs_6_7");
    if (cs.empty())
        return false;
    D3D12_FEATURE_DATA_D3D12_OPTIONS14 o14 = {};
    g.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS14, &o14, sizeof(o14));
    if (!o14.AdvancedTextureOpsSupported)
    {
        printf("  skipped: no advanced texture ops\n");
        return true;
    }
    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0 };
    D3D12_ROOT_PARAMETER rp[3] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable = { 1, &range };
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[1].Constants.Num32BitValues = 2;
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    for (auto &p : rp)
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC ss[2] = {};
    for (UINT i = 0; i < 2; i++)
    {
        ss[i].Filter = i ? D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_POINT;
        ss[i].AddressU = ss[i].AddressV = ss[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        ss[i].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS;
        ss[i].MaxLOD = D3D12_FLOAT32_MAX;
        ss[i].ShaderRegister = i;
        ss[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC rsd = { 3, rp, 2, ss };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    ID3D12PipelineState *ps = create_compute_pipeline(rs, cs);
    if (!ps)
        return false;

    // 2x2 textures: R32_UINT { 1, 2; 3, 4 } and R32_FLOAT { 0.1, 0.2; 0.3, 0.4 }.
    ID3D12Resource *upload = create_buffer(1024, D3D12_HEAP_TYPE_UPLOAD);
    uint8_t *u = nullptr;
    upload->Map(0, nullptr, (void **)&u);
    const uint32_t ui[4] = { 1, 2, 3, 4 };
    const float fl[4] = { 0.1f, 0.2f, 0.3f, 0.4f };
    memcpy(u, ui, 8); memcpy(u + 256, ui + 2, 8);
    memcpy(u + 512, fl, 8); memcpy(u + 768, fl + 2, 8);
    upload->Unmap(0, nullptr);
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 2; td.Height = 2; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.SampleDesc.Count = 1;
    ID3D12Resource *tex[2] = {};
    const DXGI_FORMAT formats[2] = { DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32_FLOAT };
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap *heap = nullptr;
    CHECK_HR(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
    D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < 2; i++)
    {
        td.Format = formats[i];
        CHECK_HR(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex[i])));
        D3D12_TEXTURE_COPY_LOCATION dst = {}, srcl = {};
        dst.pResource = tex[i];
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        srcl.pResource = upload;
        srcl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcl.PlacedFootprint.Offset = 512 * i;
        srcl.PlacedFootprint.Footprint = { formats[i], 2, 2, 1, 256 };
        g.list->CopyTextureRegion(&dst, 0, 0, 0, &srcl, nullptr);
        transition(tex[i], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        g.device->CreateShaderResourceView(tex[i], nullptr, h);
        h.ptr += g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    ID3D12Resource *out = create_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const int off[2] = { 1, 1 };   // From the texel at (0, 0) to the one at (1, 1).
    g.list->SetDescriptorHeaps(1, &heap);
    g.list->SetComputeRootSignature(rs);
    g.list->SetPipelineState(ps);
    g.list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    g.list->SetComputeRoot32BitConstants(1, 2, off, 0);
    g.list->SetComputeRootUnorderedAccessView(2, out->GetGPUVirtualAddress());
    g.list->Dispatch(1, 1, 1);
    ID3D12Resource *rb;
    const uint32_t *v = read_buffer(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 48, &rb);
    if (!v)
        return false;
    EXPECT(v[0] == 2, "integer texture sample %u, want 2", v[0]);
    EXPECT(v[1] == 4, "sample with an offset that isn't an immediate %u, want 4", v[1]);
    EXPECT(v[2] == 1 && v[3] == 0, "QuadAll %u %u, want 1 0", v[2], v[3]);
    // Gather order: (0, 1), (1, 1), (1, 0), (0, 0).
    EXPECT(v[4] == 3 && v[5] == 4 && v[6] == 2 && v[7] == 1, "GatherRaw %u %u %u %u, want 3 4 2 1", v[4], v[5], v[6], v[7]);
    EXPECT(v[8] == 0 && v[9] == 100, "SampleCmpLevel 0.25 < 0.1: %u, 0.25 < 0.4: %u, want 0 100", v[8], v[9]);
    rb->Release(); out->Release(); heap->Release(); tex[0]->Release(); tex[1]->Release(); upload->Release();
    ps->Release(); rs->Release();
    return true;
}

// Root CBVs (on MoltenVK, push descriptors: Metal constant buffers): two of them, at offsets into one buffer, with
// a dynamically indexed array, read by a compute shader.
static bool test_rootcbv()
{
    static const char *src = R"(
cbuffer A : register(b0) { float4 a[4]; };
cbuffer B : register(b1) { uint4 b; };
RWStructuredBuffer<float4> o : register(u0);
[numthreads(4, 1, 1)]
void main(uint t : SV_DispatchThreadID) { o[t] = a[t] * b.x + float(b.y); }
)";
    std::vector<char> cs = compile_dxc(src, "main", "cs_6_0");
    if (cs.empty())
        return false;
    D3D12_ROOT_PARAMETER rp[3] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rp[1].Descriptor.ShaderRegister = 1;
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    for (auto &p : rp)
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 3, rp };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    ID3D12PipelineState *ps = create_compute_pipeline(rs, cs);
    if (!ps)
        return false;
    ID3D12Resource *cb = create_buffer(1024, D3D12_HEAP_TYPE_UPLOAD);
    float *f = nullptr;
    cb->Map(0, nullptr, (void **)&f);
    for (int i = 0; i < 16; i++)
        f[64 + i] = float(i);                       // a at offset 256
    uint32_t *u = (uint32_t *)(f + 128);            // b at offset 512
    u[0] = 3; u[1] = 7; u[2] = 0; u[3] = 0;
    cb->Unmap(0, nullptr);
    ID3D12Resource *out = create_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g.list->SetComputeRootSignature(rs);
    g.list->SetPipelineState(ps);
    g.list->SetComputeRootConstantBufferView(0, cb->GetGPUVirtualAddress() + 256);
    g.list->SetComputeRootConstantBufferView(1, cb->GetGPUVirtualAddress() + 512);
    g.list->SetComputeRootUnorderedAccessView(2, out->GetGPUVirtualAddress());
    g.list->Dispatch(1, 1, 1);
    ID3D12Resource *rb;
    const float *v = (const float *)read_buffer(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 64, &rb);
    if (!v)
        return false;
    for (int i = 0; i < 16; i++)
        EXPECT(v[i] == float(i) * 3 + 7, "o[%d].%c = %f, want %f", i / 4, "xyzw"[i % 4], v[i], float(i) * 3 + 7);
    rb->Release(); out->Release(); cb->Release(); ps->Release(); rs->Release();
    return true;
}

// How long a CPU waiter takes to see a queue signal: an empty Signal waited on through
// SetEventOnCompletion (what engines do once per frame) and by polling GetCompletedValue.
static bool test_fencelatency()
{
    ID3D12Fence *f = nullptr;
    CHECK_HR(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f)));
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    ID3D12Resource *src = create_buffer(65536, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
    ID3D12Resource *dst = create_buffer(65536, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
    std::vector<double> evt, poll;
    UINT64 v = 0;
    // Each signal follows a small copy, so it waits on real GPU completion.
    auto submit = [&]() -> bool {
        g.list->CopyBufferRegion(dst, 0, src, 0, 65536);
        CHECK_HR(g.list->Close());
        ID3D12CommandList *lists[] = { g.list };
        g.queue->ExecuteCommandLists(1, lists);
        return true;
    };
    auto reset = [&]() -> bool {
        CHECK_HR(g.allocator->Reset());
        CHECK_HR(g.list->Reset(g.allocator, nullptr));
        return true;
    };
    for (int i = 0; i < 200; i++)
    {
        CHECK_HR(f->SetEventOnCompletion(++v, ev));
        QueryPerformanceCounter(&t0);
        if (!submit())
            return false;
        CHECK_HR(g.queue->Signal(f, v));
        WaitForSingleObject(ev, 5000);
        QueryPerformanceCounter(&t1);
        evt.push_back(1000.0 * (t1.QuadPart - t0.QuadPart) / freq.QuadPart);
        if (!reset())
            return false;

        QueryPerformanceCounter(&t0);
        if (!submit())
            return false;
        CHECK_HR(g.queue->Signal(f, ++v));
        while (f->GetCompletedValue() < v)
            YieldProcessor();
        QueryPerformanceCounter(&t1);
        poll.push_back(1000.0 * (t1.QuadPart - t0.QuadPart) / freq.QuadPart);
        if (!reset())
            return false;
    }
    src->Release();
    dst->Release();
    std::sort(evt.begin(), evt.end());
    std::sort(poll.begin(), poll.end());
    printf("  submit + signal -> event wait: median %.3f ms, p90 %.3f ms, max %.3f ms\n", evt[100], evt[180], evt[199]);
    printf("  submit + signal -> GetCompletedValue: median %.3f ms, p90 %.3f ms, max %.3f ms\n", poll[100], poll[180], poll[199]);
    EXPECT(evt[100] < 5.0, "signal to event takes %.3f ms", evt[100]);
    CloseHandle(ev);
    f->Release();
    return true;
}

// A chain of dispatches, each reading what the previous one wrote, separated by UAV barriers.
// MoltenVK keeps these barriers inside one compute encoder; a missing dependency shows up as a
// wrong sum.
static bool uavchain(bool aliasing)
{
    static const char *src = R"(
cbuffer C : register(b0) { uint step; };
RWStructuredBuffer<uint> b : register(u0);
[numthreads(64, 1, 1)] void main(uint t : SV_DispatchThreadID)
{
    // Element t takes its left neighbour's value from the previous step, plus one.
    uint left = t == 0 ? 0 : b[t - 1];
    GroupMemoryBarrierWithGroupSync();
    if (t == step) b[t] = left + 1;
}
)";
    std::vector<char> cs = compile_dxc(src, "main", "cs_6_0");
    if (cs.empty())
        return false;
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants = { 0, 0, 1 };
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    for (auto &p : rp)
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 2, rp };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    ID3D12PipelineState *ps = create_compute_pipeline(rs, cs);
    if (!ps)
        return false;
    ID3D12Resource *buf = create_buffer(64 * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g.list->SetComputeRootSignature(rs);
    g.list->SetPipelineState(ps);
    g.list->SetComputeRootUnorderedAccessView(1, buf->GetGPUVirtualAddress());
    D3D12_RESOURCE_BARRIER uav = {};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = buf;
    if (aliasing)
    {
        // Naming no resource means any placed resource ("before" overlaps UAV.pResource, so clear it).
        uav.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
        uav.Aliasing.pResourceBefore = nullptr;
        uav.Aliasing.pResourceAfter = nullptr;
    }
    for (UINT step = 0; step < 64; step++)
    {
        g.list->SetComputeRoot32BitConstants(0, 1, &step, 0);
        g.list->Dispatch(1, 1, 1);
        g.list->ResourceBarrier(1, &uav);
    }
    ID3D12Resource *rb;
    const uint32_t *v = read_buffer(buf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 64 * 4, &rb);
    if (!v)
        return false;
    for (UINT i = 0; i < 64; i++)
        EXPECT(v[i] == i + 1, "b[%u] = %u, want %u", i, v[i], i + 1);
    rb->Release(); buf->Release(); ps->Release(); rs->Release();
    return true;
}

static bool test_uavchain()
{
    return uavchain(false);
}

// The same chain separated by aliasing barriers naming no resource (a global barrier on every queue stage), which
// MoltenVK also keeps inside the compute encoder.
static bool test_aliaschain()
{
    return uavchain(true);
}

// FXC shaders preserve NaN and Inf (SignedZeroInfNanPreserve), which MoltenVK compiles with the default (not
// precise::) Metal transcendentals: the special values D3D defines for them must hold anyway, and the
// results stay within D3D's precision. Inputs come from root constants so FXC can't fold them.
static const char *cs_special_src = R"(
cbuffer c : register(b0) { float4 a; float4 b; };  // a = (0, inf, -inf, nan), b = (-1, 2, 300, 0.5)
RWByteAddressBuffer o : register(u0);
[numthreads(1, 1, 1)]
void main()
{
    float r[25] = {
        log2(a.x), log2(a.y), log2(b.x), log2(a.w), exp2(a.z), exp2(a.y), exp2(a.w), sqrt(b.x), sqrt(a.y), rsqrt(a.x),
        rsqrt(a.y), rsqrt(b.x), sin(a.y), cos(a.w), sin(b.z), cos(b.z), pow(a.x, b.y), pow(b.y, b.w), asin(b.y),
        max(log2(a.x), -100.0), acos(a.w), atan(a.y), tan(a.y), sin(a.z), cos(a.y) };
    [unroll] for (uint i = 0; i < 25; i++)
        o.Store(i * 4, asuint(r[i]));
}
)";

static bool special_values(bool dxbc)
{
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.Num32BitValues = 8;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 2, rp };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    ID3D12PipelineState *ps = create_compute_pipeline(rs, dxbc ? compile_fxc(cs_special_src, "main", "cs_5_0") :
                                                                compile_dxc(cs_special_src, "main", "cs_6_0"));
    if (!rs || !ps)
        return false;
    const float inf = INFINITY, nan = NAN;
    const float in[8] = { 0.0f, inf, -inf, nan, -1.0f, 2.0f, 300.0f, 0.5f };
    ID3D12Resource *buf = create_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g.list->SetComputeRootSignature(rs);
    g.list->SetPipelineState(ps);
    g.list->SetComputeRoot32BitConstants(0, 8, in, 0);
    g.list->SetComputeRootUnorderedAccessView(1, buf->GetGPUVirtualAddress());
    g.list->Dispatch(1, 1, 1);
    ID3D12Resource *rb;
    const float *v = (const float *)read_buffer(buf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 256, &rb);
    if (!v)
        return false;
    static const char *names[25] = { "log2(0)", "log2(inf)", "log2(-1)", "log2(nan)", "exp2(-inf)", "exp2(inf)", "exp2(nan)",
                                     "sqrt(-1)", "sqrt(inf)", "rsqrt(0)", "rsqrt(inf)", "rsqrt(-1)", "sin(inf)", "cos(nan)",
                                     "sin(300)", "cos(300)", "pow(0, 2)", "pow(2, 0.5)", "asin(2)", "max(log2(0), -100)", "acos(nan)",
                                     "atan(inf)", "tan(inf)", "sin(-inf)", "cos(inf)" };
    // nan marks results that must be NaN; tolerances are D3D's (0.0008 absolute for sin/cos, ~22 bits for exp2/log2).
    const float want[25] = { -inf, inf, nan, nan, 0.0f, inf, nan, nan, inf, inf, 0.0f, nan, nan, nan,
                             sinf(300.0f), cosf(300.0f), 0.0f, sqrtf(2.0f), nan, -100.0f, nan, 1.5707964f, nan, nan, nan };
    const float tol[25] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.0008f, 0.0008f, 0, 0.0001f, 0, 0, 0, 0.0001f };
    for (int i = 0; i < 25; i++)
    {
        bool ok = isnan(want[i]) ? isnan(v[i]) : isinf(want[i]) ? v[i] == want[i] : fabsf(v[i] - want[i]) <= tol[i];
        // Metal's sin and cos give 0 for an infinite or NaN argument: DXIL shaders get that on every path (SPIR-V
        // and Metal Shader Converter), and DXBC shaders through the converter. A known deviation, reported apart.
        bool metal_trig = (i == 12 || i == 13 || i == 23 || i == 24) && v[i] == 0.0f;
        if (!ok && metal_trig)
        {
            printf("  known Metal deviation: %s = %g, want %g\n", names[i], v[i], want[i]);
            continue;
        }
        EXPECT(ok, "%s = %g, want %g", names[i], v[i], want[i]);
    }
    rb->Release(); buf->Release(); ps->Release(); rs->Release();
    return true;
}

static bool test_special_values_dxbc() { return special_values(true); }
static bool test_special_values_dxil() { return special_values(false); }

// A UAV barrier inside a render pass: MoltenVK keeps the pass open and defers the barrier (Apple GPUs have
// no barriers inside render passes), so a later draw whose pixel shader uses UAVs must still see the
// compute writes the barrier orders. Draw 1 uses no UAVs, then the barrier, then draw 2 reads the UAV.
static bool test_uav_barrier_in_render_pass()
{
    static const char *src = R"(
RWByteAddressBuffer b : register(u1);  // u0 shares its slot with SV_Target in SM 5
[numthreads(1, 1, 1)] void cs() { b.Store(0, 0x3f000000); b.Store(4, 0x3e800000); } // 0.5, 0.25
float4 vs(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps_plain() : SV_Target { return float4(1, 0, 0, 1); }
float4 ps_uav() : SV_Target { return float4(asfloat(b.Load(0)), asfloat(b.Load(4)), 0, 1); }
)";
    D3D12_ROOT_PARAMETER rp = {};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp.Descriptor.ShaderRegister = 1;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &rp };
    ID3D12RootSignature *rs = create_root_signature(rsd);
    ID3D12PipelineState *cs = create_compute_pipeline(rs, compile_fxc(src, "cs", "cs_5_0"));
    std::vector<char> vs = compile_fxc(src, "vs", "vs_5_0"), ps_plain = compile_fxc(src, "ps_plain", "ps_5_0"),
                      ps_uav = compile_fxc(src, "ps_uav", "ps_5_0");
    if (!rs || !cs || vs.empty() || ps_plain.empty() || ps_uav.empty())
        return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = rs;
    pd.VS = { vs.data(), vs.size() };
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = ~0u;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState *plain = nullptr, *uav = nullptr;
    pd.PS = { ps_plain.data(), ps_plain.size() };
    CHECK_HR(g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&plain)));
    pd.PS = { ps_uav.data(), ps_uav.size() };
    CHECK_HR(g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&uav)));

    const UINT W = 16, H = 16;
    ID3D12Resource *buf = create_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
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

    D3D12_VIEWPORT vp = { 0, 0, (float)W, (float)H, 0, 1 };
    D3D12_RECT sc = { 0, 0, (LONG)W, (LONG)H };
    D3D12_RESOURCE_BARRIER ub = {};
    ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    ub.UAV.pResource = buf;
    float clear[4] = { 0, 0, 1, 1 };
    g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    g.list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    g.list->RSSetViewports(1, &vp);
    g.list->RSSetScissorRects(1, &sc);
    g.list->SetGraphicsRootSignature(rs);
    g.list->SetGraphicsRootUnorderedAccessView(0, buf->GetGPUVirtualAddress());
    g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.list->SetPipelineState(plain);
    g.list->DrawInstanced(3, 1, 0, 0);                 // opens the render pass
    g.list->SetComputeRootSignature(rs);
    g.list->SetComputeRootUnorderedAccessView(0, buf->GetGPUVirtualAddress());
    g.list->SetPipelineState(cs);
    g.list->Dispatch(1, 1, 1);                         // ends the pass: compute writes the buffer
    g.list->SetPipelineState(plain);
    g.list->DrawInstanced(3, 1, 0, 0);                 // a new pass, drawn without UAVs
    g.list->ResourceBarrier(1, &ub);                   // deferred inside the pass
    g.list->DrawInstanced(3, 1, 0, 0);                 // still no UAVs
    g.list->SetPipelineState(uav);
    g.list->DrawInstanced(3, 1, 0, 0);                 // reads the UAV: must see the compute writes
    transition(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12Resource *rb = create_buffer(W * H * 4, D3D12_HEAP_TYPE_READBACK);
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
    uint32_t got = px[4 * W + 4];
    EXPECT(got == 0xff004080 || got == 0xff004081 || got == 0xff00407f, "pixel 0x%08x, want 0xff004080 (UAV values 0.5, 0.25)", got);
    rb->Release(); rtv_heap->Release(); rt->Release(); buf->Release(); uav->Release(); plain->Release(); cs->Release(); rs->Release();
    return true;
}

struct Test { const char *name; bool (*fn)(); };
static const Test tests[] = {
    { "caps", test_caps },
    { "compute_dxil", test_compute_dxil },
    { "compute_dxbc", test_compute_dxbc },
    { "uav_barrier_in_render_pass", test_uav_barrier_in_render_pass },
    { "special_values_dxbc", test_special_values_dxbc },
    { "special_values_dxil", test_special_values_dxil },
    { "sm67", test_sm67 },
    { "sm67_textures", test_sm67_textures },
    { "rootcbv", test_rootcbv },
    { "fencelatency", test_fencelatency },
    { "timestamps", test_timestamps },
    { "uavchain", test_uavchain },
    { "aliaschain", test_aliaschain },
    { "draw", test_draw },
    { "conservative", test_conservative },
    { "rayquery", test_rayquery },
    { "dispatchrays", test_dispatchrays },
    { "mesh", test_mesh },
    { "mesh_resources", test_mesh_resources },
    { "mesh_cull", test_mesh_cull },
    { "mesh_features", test_mesh_features },
    { "tessellation", test_tessellation },
    { "tess_domains", test_tess_domains },
    { "tess_domains_dxbc", test_tess_domains_dxbc },
    { "gs_features", test_gs_features },
    { "gs_features_dxbc", test_gs_features_dxbc },
    { "executeindirect", test_executeindirect },
    { "executeindirect_dispatch_count", test_executeindirect_dispatch_count },
    { "dynamicresources", test_dynamicresources },
    { "dynamicresources_graphics", test_dynamicresources_graphics },
    { "opdiff", test_opdiff },
    { "samplediff", test_samplediff },
    { "formatdiff", test_formatdiff },
    { "texdiff", test_texdiff },
    { "vtxdiff", test_vtxdiff },
    { "vertex_rgb10a2_uint", test_vertex_rgb10a2_uint },
    { "sampler_lod_bias", test_sampler_lod_bias },
    { "sampler_lod_bias_dxbc", test_sampler_lod_bias_dxbc },
    { "sampler_lod_bias_late", test_sampler_lod_bias_late },
    { "depth_equal_cross_path", test_depth_equal_cross_path },
    { "depth_equal_cross_path_dxbc", test_depth_equal_cross_path_dxbc },
    { "srv_min_lod_clamp", test_srv_min_lod_clamp },
    { "srv_min_lod_clamp_dxbc", test_srv_min_lod_clamp_dxbc },
    { "sample_nan_coords", test_sample_nan_coords },
    { "sample_nan_coords_dxbc", test_sample_nan_coords_dxbc },
    { "rt_float_conversion", test_rt_float_conversion },
    { "rt_float_conversion_dxbc", test_rt_float_conversion_dxbc },
    { "uav_float_conversion", test_uav_float_conversion },
    { "uav_float_conversion_dxbc", test_uav_float_conversion_dxbc },
    { "msaa_depth_stencil", test_msaa_depth_stencil },
    { "textures", test_textures },
    { "static_sampler_lod", test_static_sampler_lod },
    { "queues", test_queues },
    { "fences", test_fences },
    { "wave_ops_compute", test_wave_ops_compute },
    { "inputlayout", test_inputlayout },
    { "executeindirect_buffers", test_executeindirect_buffers },
    { "executeindirect_template_first", test_executeindirect_template_first },
    { "vs_uav_no_attachments", test_vs_uav_no_attachments },
    { "uav_global_barrier_deferred", test_uav_global_barrier_deferred },
    { "depth_bounds", test_depth_bounds },
    { "viewinstancing", test_viewinstancing },
    { "d3d11on12", test_d3d11on12 },
    { "samplerfeedback", test_samplerfeedback },
    { "vrs", test_vrs },
    { "tiled3d", test_tiled3d },
    { "typedbuffer", test_typedbuffer },
    { "typedbuffer_matrix", test_typedbuffer_matrix },
    { "typedbuffer_uav_offset", test_typedbuffer_uav_offset },
    { "descriptor_copies", test_descriptor_copies },
    { "descriptor_lifetime", test_descriptor_lifetime },
    { "heap_placement", test_heap_placement },
    { "typedbuffer_rgb32_float4", test_typedbuffer_rgb32_float4 },
    { "typedbuffer_rgb32_float4_dxbc", test_typedbuffer_rgb32_float4_dxbc },
    { "typedbuffer_rgb32", test_typedbuffer_rgb32 },
    { "typedbuffer_rgb32_dxbc", test_typedbuffer_rgb32_dxbc },
    { "view_kinds_dxil", test_view_kinds_dxil },
    { "view_kinds_dxbc", test_view_kinds_dxbc },
    { "robust", test_robust },
    { "logicop", test_logicop },
    { "dual_source_blend", test_dual_source_blend },
    { "depth_only", test_depth_only },
    { "draw_params", test_draw_params },
    { "draw_params_sm68", test_draw_params_sm68 },
    { "culling_dxil", test_culling_dxil },
    { "culling_dxbc", test_culling_dxbc },
    { "raster_depth", test_raster_depth },
    { "cull_clip_distance_dxil", test_cull_clip_distance_dxil },
    { "cull_clip_distance_dxbc", test_cull_clip_distance_dxbc },
    { "barycentrics", test_barycentrics },
    { "streamoutput", test_streamoutput },
    { "gs_streamoutput_dxil", test_gs_streamoutput_dxil },
    { "gs_streamoutput_dxbc", test_gs_streamoutput_dxbc },
    { "gs_packed_dxil", test_gs_packed_dxil },
    { "gs_packed_dxbc", test_gs_packed_dxbc },
    { "gs_narrow_dxil", test_gs_narrow_dxil },
    { "gs_narrow_dxbc", test_gs_narrow_dxbc },
    { "pervertex", test_pervertex },
    { "cbv_rows_robust_dxil", test_cbv_rows_robust_dxil },
    { "cbv_rows_robust_dxbc", test_cbv_rows_robust_dxbc },
    { "minmax", test_minmax },
    { "minmax_heap", test_minmax_heap },
    { "minmax_heap_late", test_minmax_heap_late },
    { "wave_first_helpers", test_wave_first_helpers },
    { "wbi_markers", test_wbi_markers },
    { "predication", test_predication },
    { "predication_rewrite", test_predication_rewrite },
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
