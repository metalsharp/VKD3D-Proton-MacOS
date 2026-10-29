/*
 * fsr4probe: runs AMD FSR Upscaling from the FSR SDK's signed DLLs (amd_fidelityfx_loader_dx12.dll and
 * amd_fidelityfx_upscaler_dx12.dll next to the exe, unmodified) on the D3D12 device, and checks its output.
 *
 *   fsr4probe.exe [--version <substring>] [--frames N] [--mode quality|balanced|performance|ultra]
 *                 [--size WxH] [--images <dir>]
 *
 * 1. Reports the adapter, its shader model and the features FSR Upscaling 4 depends on.
 * 2. Lists the upscaler versions the FFX API offers on this device (ffxQueryDescGetVersions).
 * 3. For each version (or those whose name contains --version): creates a context pinned to it
 *    (ffxOverrideVersion), confirms the provider (ffxQueryGetProviderVersion), and upscales N frames of
 *    a moving, jittered test scene with depth and motion vectors. The last output is compared with the
 *    scene rendered at display resolution (PSNR), alongside a bilinear upscale of the last input frame,
 *    and checked for NaN/Inf. GPU time per dispatch comes from timestamp queries (stale on MoltenVK) and from
 *    the wall time from submit to completion, with the inputs uploaded in a submit of their own.
 *
 * Build: scripts/build-fsr-probe.sh (llvm-mingw, x86_64), against the SDK's MIT-licensed API headers.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "ffx_api.h"
#define _WINDOWS   // ffx_api_loader.h defines ffxLoadFunctions for Windows builds only
#include "ffx_api_loader.h"
#include "dx12/ffx_api_dx12.h"
#include "ffx_upscale.h"

#define CHECK(expr) do { HRESULT hr_ = (expr); if (FAILED(hr_)) { printf("FAIL %s: hr %#lx (line %d)\n", #expr, (long)hr_, __LINE__); exit(2); } } while (0)

struct Options {
    std::string version;
    unsigned frames = 32, displayW = 1920, displayH = 1080, mode = FFX_UPSCALE_QUALITY_MODE_PERFORMANCE;
    std::string images;
};

static ffxFunctions g_ffx;
static ID3D12Device *g_dev;
static ID3D12CommandQueue *g_queue;
static ID3D12CommandAllocator *g_alloc;
static ID3D12GraphicsCommandList *g_list;
static ID3D12Fence *g_fence;
static UINT64 g_fenceValue;
static HANDLE g_event;

/* Each distinct message once: the runtime repeats its warnings every frame. */
static void onMessage(uint32_t type, const wchar_t *message) {
    static std::vector<std::wstring> seen;
    for (const auto &m : seen) if (m == message) return;
    seen.emplace_back(message);
    printf("  ffx %s: %ls\n", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", message);
}

static void submitAndWait() {
    CHECK(g_list->Close());
    ID3D12CommandList *lists[] = {g_list};
    g_queue->ExecuteCommandLists(1, lists);
    CHECK(g_queue->Signal(g_fence, ++g_fenceValue));
    if (g_fence->GetCompletedValue() < g_fenceValue) {
        g_fence->SetEventOnCompletion(g_fenceValue, g_event);
        if (WaitForSingleObject(g_event, 30000) != WAIT_OBJECT_0) { printf("FAIL GPU timeout (30 s)\n"); exit(3); }
    }
    CHECK(g_dev->GetDeviceRemovedReason());
    CHECK(g_alloc->Reset());
    CHECK(g_list->Reset(g_alloc, nullptr));
}

static ID3D12Resource *makeTexture(unsigned w, unsigned h, DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w; desc.Height = h; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.Format = fmt; desc.SampleDesc.Count = 1; desc.Flags = flags;
    ID3D12Resource *res;
    CHECK(g_dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&res)));
    return res;
}

static ID3D12Resource *makeBuffer(UINT64 size, D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES heap = {type};
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size; desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *res;
    CHECK(g_dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
          type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&res)));
    return res;
}

static void barrier(ID3D12Resource *res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (before == after) return;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before; b.Transition.StateAfter = after;
    g_list->ResourceBarrier(1, &b);
}

/* Uploads w*h texels of bpp bytes from data into tex (in COPY_DEST), through the upload buffer. */
static void upload(ID3D12Resource *tex, ID3D12Resource *staging, const void *data, unsigned w, unsigned h, unsigned bpp) {
    D3D12_RESOURCE_DESC desc = tex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; UINT rows; UINT64 rowSize, total;
    g_dev->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &rowSize, &total);
    unsigned char *map;
    D3D12_RANGE none = {0, 0};
    CHECK(staging->Map(0, &none, (void **)&map));
    for (unsigned y = 0; y < h; y++)
        memcpy(map + fp.Offset + (size_t)y * fp.Footprint.RowPitch, (const unsigned char *)data + (size_t)y * w * bpp, (size_t)w * bpp);
    staging->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION dst = {tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}; dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src = {staging, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}; src.PlacedFootprint = fp;
    g_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
}

/* IEEE half <-> float, enough for test data. */
static uint16_t toHalf(float f) {
    uint32_t x; memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000, mant = x & 0x7fffff; int exp = (int)((x >> 23) & 0xff) - 127 + 15;
    if (exp <= 0) return (uint16_t)sign;
    if (exp >= 31) return (uint16_t)(sign | 0x7c00);
    return (uint16_t)(sign | (exp << 10) | ((mant + 0x1000) >> 13));
}
static float fromHalf(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000) << 16, exp = (h >> 10) & 0x1f, mant = h & 0x3ff, x;
    if (exp == 0) x = sign;                                     // flush denormals: the test data has none
    else if (exp == 31) x = sign | 0x7f800000 | (mant << 13);
    else x = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    float f; memcpy(&f, &x, 4); return f;
}

/* The scene, in display pixels at time t: a zone plate band-limited below the display's Nyquist rate, sharp
 * discs and bars, and a smooth gradient, all scrolling at (vx, vy) display pixels per frame. */
static const float kVx = 1.3f, kVy = 0.7f;
static void scene(float x, float y, float t, float rgb[3]) {
    float sx = x - kVx * t, sy = y - kVy * t;
    float cx = sx - 960.0f, cy = sy - 540.0f, r2 = cx * cx + cy * cy;
    float zone = 0.5f + 0.5f * cosf(r2 * 0.00045f) * expf(-r2 * 0.0000012f);
    float bars = fmodf(fabsf(sx), 64.0f) < 6.0f ? 1.0f : 0.0f;
    float discX = fmodf(fabsf(sx) + 4000.0f, 240.0f) - 120.0f, discY = fmodf(fabsf(sy) + 4000.0f, 240.0f) - 120.0f;
    float disc = discX * discX + discY * discY < 40.0f * 40.0f ? 1.0f : 0.0f;
    float grad = (sy - 0.0f) / 1080.0f;
    rgb[0] = 0.15f + 0.6f * zone * (1.0f - disc) + 0.25f * bars;
    rgb[1] = 0.10f + 0.5f * zone + 0.4f * disc * (0.5f + 0.5f * grad);
    rgb[2] = 0.20f + 0.3f * grad + 0.3f * disc + 0.2f * bars;
    for (int i = 0; i < 3; i++) rgb[i] = rgb[i] < 0 ? 0 : rgb[i] > 1 ? 1 : rgb[i];
}

static double psnr(const std::vector<float> &a, const std::vector<float> &b) {
    double se = 0; size_t n = 0;
    for (size_t i = 0; i < a.size(); i++) { if ((i & 3) == 3) continue; double d = a[i] - b[i]; se += d * d; n++; }
    double mse = se / n;
    return mse <= 1e-12 ? 99.0 : 10.0 * log10(1.0 / mse);
}

static void writePPM(const std::string &path, const std::vector<float> &rgba, unsigned w, unsigned h) {
    char header[64];
    int len = snprintf(header, sizeof(header), "P6\n%u %u\n255\n", w, h);
    std::vector<unsigned char> data(header, header + len);
    data.reserve(len + (size_t)w * h * 3);
    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++)
            for (int c = 0; c < 3; c++) {
                float v = rgba[((size_t)y * w + x) * 4 + c];
                v = std::isfinite(v) ? (v < 0 ? 0 : v > 1 ? 1 : v) : 1;
                data.push_back((unsigned char)(powf(v, 1.0f / 2.2f) * 255.0f + 0.5f));
            }
    // CreateFile with the default security descriptor (the directory's inherited permissions)
    HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(file, data.data(), (DWORD)data.size(), &written, nullptr);
    CloseHandle(file);
}

static void printDevice() {
    IDXGIFactory1 *factory; IDXGIAdapter1 *adapter; DXGI_ADAPTER_DESC1 ad;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->EnumAdapters1(0, &adapter)) && SUCCEEDED(adapter->GetDesc1(&ad)))
        printf("adapter: %ls (vendor %04x, device %04x, %llu MB)\n", ad.Description, ad.VendorId, ad.DeviceId,
               (unsigned long long)(ad.DedicatedVideoMemory >> 20));
    D3D12_FEATURE_DATA_SHADER_MODEL sm = {(D3D_SHADER_MODEL)0x68};   // 6.8, newer than the mingw headers
    while (FAILED(g_dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) && sm.HighestShaderModel > D3D_SHADER_MODEL_5_1)
        sm.HighestShaderModel = (D3D_SHADER_MODEL)(sm.HighestShaderModel - 1);
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1 = {}; g_dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &o1, sizeof(o1));
    D3D12_FEATURE_DATA_D3D12_OPTIONS4 o4 = {}; g_dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &o4, sizeof(o4));
    D3D12_FEATURE_DATA_D3D12_OPTIONS9 o9 = {}; g_dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS9, &o9, sizeof(o9));
    printf("shader model %x.%x, wave ops %d (lanes %u-%u), native 16-bit ops %d, 64-bit atomics on typed resources %d, wave MMA tier %d\n",
           sm.HighestShaderModel >> 4, sm.HighestShaderModel & 0xf, o1.WaveOps, o1.WaveLaneCountMin, o1.WaveLaneCountMax,
           o4.Native16BitShaderOpsSupported, o9.AtomicInt64OnTypedResourceSupported, (int)o9.WaveMMATier);
}

/* Runs one version; returns 0 when its output is finite and closer to the scene than bilinear upscaling. */
static int runVersion(const Options &opt, uint64_t versionId, const char *versionName) {
    printf("\n== %s (id %#llx)\n", versionName, (unsigned long long)versionId);
    ffxContext ctx = nullptr;
    ffxCreateBackendDX12Desc backend = {}; backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12; backend.device = g_dev;
    ffxCreateContextDescUpscaleVersion apiVersion = {}; apiVersion.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    apiVersion.version = FFX_UPSCALER_VERSION;
    ffxOverrideVersion override_ = {}; override_.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION; override_.versionId = versionId;
    ffxCreateContextDescUpscale create = {}; create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    create.flags = FFX_UPSCALE_ENABLE_DEBUG_CHECKING | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
    create.maxUpscaleSize = {opt.displayW, opt.displayH};
    create.fpMessage = onMessage;

    uint32_t renderW = 0, renderH = 0;
    {
        ffxQueryDescUpscaleGetRenderResolutionFromQualityMode q = {};
        q.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE;
        q.header.pNext = &backend.header;
        q.displayWidth = opt.displayW; q.displayHeight = opt.displayH; q.qualityMode = opt.mode;
        q.pOutRenderWidth = &renderW; q.pOutRenderHeight = &renderH;
        if (g_ffx.Query(nullptr, &q.header) != FFX_API_RETURN_OK || !renderW) { renderW = opt.displayW / 2; renderH = opt.displayH / 2; }
    }
    create.maxRenderSize = {renderW, renderH};
    create.header.pNext = &backend.header;
    backend.header.pNext = &apiVersion.header;
    apiVersion.header.pNext = &override_.header;
    ffxReturnCode_t rc = g_ffx.CreateContext(&ctx, &create.header, nullptr);
    if (rc != FFX_API_RETURN_OK) { printf("FAIL ffxCreateContext: return code %u\n", rc); return 1; }

    ffxQueryGetProviderVersion pv = {}; pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    g_ffx.Query(&ctx, &pv.header);
    printf("provider: %s (id %#llx)\n", pv.versionName ? pv.versionName : "?", (unsigned long long)pv.versionId);
    FfxApiEffectMemoryUsage mem = {};
    ffxQueryDescUpscaleGetGPUMemoryUsage mq = {}; mq.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE; mq.gpuMemoryUsageUpscaler = &mem;
    if (g_ffx.Query(&ctx, &mq.header) == FFX_API_RETURN_OK)
        printf("GPU memory: %.1f MB (aliasable %.1f MB)\n", mem.totalUsageInBytes / 1048576.0, mem.aliasableUsageInBytes / 1048576.0);
    int32_t phases = 8;
    {
        ffxQueryDescUpscaleGetJitterPhaseCount q = {}; q.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
        q.renderWidth = renderW; q.displayWidth = opt.displayW; q.pOutPhaseCount = &phases;
        g_ffx.Query(&ctx, &q.header);
    }
    printf("render %ux%u -> display %ux%u, %d jitter phases, %u frames\n", renderW, renderH, opt.displayW, opt.displayH, phases, opt.frames);

    ID3D12Resource *color = makeTexture(renderW, renderH, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *depth = makeTexture(renderW, renderH, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *motion = makeTexture(renderW, renderH, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *output = makeTexture(opt.displayW, opt.displayH, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource *stagingColor = makeBuffer((UINT64)renderW * renderH * 8 + 65536 * 4, D3D12_HEAP_TYPE_UPLOAD);
    ID3D12Resource *stagingDepth = makeBuffer((UINT64)renderW * renderH * 4 + 65536 * 4, D3D12_HEAP_TYPE_UPLOAD);
    ID3D12Resource *stagingMotion = makeBuffer((UINT64)renderW * renderH * 4 + 65536 * 4, D3D12_HEAP_TYPE_UPLOAD);
    UINT64 outRowPitch = ((UINT64)opt.displayW * 8 + 255) & ~255ull;
    ID3D12Resource *readback = makeBuffer(outRowPitch * opt.displayH, D3D12_HEAP_TYPE_READBACK);
    ID3D12QueryHeap *timestamps; D3D12_QUERY_HEAP_DESC qh = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2};
    CHECK(g_dev->CreateQueryHeap(&qh, IID_PPV_ARGS(&timestamps)));
    ID3D12Resource *tsBuffer = makeBuffer(16, D3D12_HEAP_TYPE_READBACK);
    UINT64 tsFreq = 1; g_queue->GetTimestampFrequency(&tsFreq);

    const float scale = (float)opt.displayW / renderW;
    std::vector<uint16_t> colorData((size_t)renderW * renderH * 4), motionData((size_t)renderW * renderH * 2);
    std::vector<float> depthData((size_t)renderW * renderH, 0.5f);
    // Motion vectors point from a pixel to where it was in the previous frame, in render pixels.
    for (size_t i = 0; i < motionData.size(); i += 2) { motionData[i] = toHalf(-kVx / scale); motionData[i + 1] = toHalf(-kVy / scale); }
    double gpuMsTotal = 0, wallMsTotal = 0; unsigned gpuMsCount = 0, wallMsCount = 0;
    std::vector<float> lastInput;
    int failed = 0;

    for (unsigned frame = 0; frame < opt.frames && !failed; frame++) {
        float jx = 0, jy = 0;
        ffxQueryDescUpscaleGetJitterOffset jq = {}; jq.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET;
        jq.index = (int32_t)frame; jq.phaseCount = phases; jq.pOutX = &jx; jq.pOutY = &jy;
        g_ffx.Query(&ctx, &jq.header);
        // The jitter moves the projection by (jx, -jy) pixels, so render pixel (i, j) samples the scene at
        // ((i + 0.5 - jx) * scale, (j + 0.5 - jy) * scale) in display pixels (y down).
        for (unsigned j = 0; j < renderH; j++)
            for (unsigned i = 0; i < renderW; i++) {
                float rgb[3];
                scene((i + 0.5f - jx) * scale, (j + 0.5f - jy) * scale, (float)frame, rgb);
                uint16_t *p = &colorData[((size_t)j * renderW + i) * 4];
                p[0] = toHalf(rgb[0]); p[1] = toHalf(rgb[1]); p[2] = toHalf(rgb[2]); p[3] = toHalf(1.0f);
            }
        if (frame == opt.frames - 1) {
            lastInput.resize((size_t)renderW * renderH * 4);
            for (size_t k = 0; k < lastInput.size(); k++) lastInput[k] = fromHalf(colorData[k]);
        }
        upload(color, stagingColor, colorData.data(), renderW, renderH, 8);
        upload(depth, stagingDepth, depthData.data(), renderW, renderH, 4);
        upload(motion, stagingMotion, motionData.data(), renderW, renderH, 4);
        const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barrier(color, D3D12_RESOURCE_STATE_COPY_DEST, read);
        barrier(depth, D3D12_RESOURCE_STATE_COPY_DEST, read);
        barrier(motion, D3D12_RESOURCE_STATE_COPY_DEST, read);

        ffxDispatchDescUpscale d = {}; d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        d.commandList = g_list;
        d.color = ffxApiGetResourceDX12(color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.depth = ffxApiGetResourceDX12(depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.motionVectors = ffxApiGetResourceDX12(motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.output = ffxApiGetResourceDX12(output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        d.jitterOffset = {jx, jy};
        d.motionVectorScale = {1.0f, 1.0f};
        d.renderSize = {renderW, renderH};
        d.upscaleSize = {opt.displayW, opt.displayH};
        d.enableSharpening = false;
        d.frameTimeDelta = 16.6f;
        d.preExposure = 1.0f;
        d.reset = frame == 0;
        d.cameraNear = 0.1f; d.cameraFar = 1000.0f; d.cameraFovAngleVertical = 1.0f; d.viewSpaceToMetersFactor = 1.0f;
        // The inputs go in their own submission, so the dispatch's submission times the upscaler alone.
        submitAndWait();
        g_list->EndQuery(timestamps, D3D12_QUERY_TYPE_TIMESTAMP, 0);
        rc = g_ffx.Dispatch(&ctx, &d.header);
        g_list->EndQuery(timestamps, D3D12_QUERY_TYPE_TIMESTAMP, 1);
        g_list->ResolveQueryData(timestamps, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, tsBuffer, 0);
        if (rc != FFX_API_RETURN_OK) { printf("FAIL ffxDispatch (frame %u): return code %u\n", frame, rc); failed = 1; }
        barrier(color, read, D3D12_RESOURCE_STATE_COPY_DEST);
        barrier(depth, read, D3D12_RESOURCE_STATE_COPY_DEST);
        barrier(motion, read, D3D12_RESOURCE_STATE_COPY_DEST);
        if (frame == opt.frames - 1) {
            barrier(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION src = {output, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}; src.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION dst = {readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R16G16B16A16_FLOAT, opt.displayW, opt.displayH, 1, (UINT)outRowPitch};
            g_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            barrier(output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        LARGE_INTEGER t0, t1, f; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
        submitAndWait();
        QueryPerformanceCounter(&t1);
        double wallMs = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
        UINT64 *ts; D3D12_RANGE r = {0, 16};
        if (SUCCEEDED(tsBuffer->Map(0, &r, (void **)&ts))) {
            double ms = ts[1] > ts[0] ? (double)(ts[1] - ts[0]) * 1000.0 / (double)tsFreq : -1.0;
            if (frame >= 4) {   // the first frames include pipeline creation
                if (ms >= 0) { gpuMsTotal += ms; gpuMsCount++; }
                wallMsTotal += wallMs; wallMsCount++;
            }
            D3D12_RANGE none = {0, 0}; tsBuffer->Unmap(0, &none);
        }
    }

    if (!failed) {
        std::vector<float> out((size_t)opt.displayW * opt.displayH * 4), truth(out.size()), bilinear(out.size());
        unsigned char *map; D3D12_RANGE r = {0, (SIZE_T)(outRowPitch * opt.displayH)};
        CHECK(readback->Map(0, &r, (void **)&map));
        size_t bad = 0; double mean = 0;
        for (unsigned y = 0; y < opt.displayH; y++)
            for (unsigned x = 0; x < opt.displayW * 4; x++) {
                float v = fromHalf(((uint16_t *)(map + y * outRowPitch))[x]);
                out[(size_t)y * opt.displayW * 4 + x] = v;
                if (!std::isfinite(v)) bad++; else if ((x & 3) != 3) mean += v;
            }
        D3D12_RANGE none = {0, 0}; readback->Unmap(0, &none);
        float t = (float)(opt.frames - 1);
        for (unsigned y = 0; y < opt.displayH; y++)
            for (unsigned x = 0; x < opt.displayW; x++) {
                float rgb[3]; scene(x + 0.5f, y + 0.5f, t, rgb);
                float *p = &truth[((size_t)y * opt.displayW + x) * 4];
                p[0] = rgb[0]; p[1] = rgb[1]; p[2] = rgb[2]; p[3] = 1;
                // Bilinear upscale of the last input frame, undoing its jitter as a plain resample would not.
                float u = (x + 0.5f) / scale - 0.5f, v = (y + 0.5f) / scale - 0.5f;
                int x0 = (int)floorf(u), y0 = (int)floorf(v); float fx = u - x0, fy = v - y0;
                for (int c = 0; c < 4; c++) {
                    auto at = [&](int xi, int yi) {
                        xi = xi < 0 ? 0 : xi >= (int)renderW ? renderW - 1 : xi; yi = yi < 0 ? 0 : yi >= (int)renderH ? renderH - 1 : yi;
                        return lastInput[((size_t)yi * renderW + xi) * 4 + c];
                    };
                    bilinear[((size_t)y * opt.displayW + x) * 4 + c] =
                        (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) + (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
                }
            }
        mean /= (double)opt.displayW * opt.displayH * 3;
        double pOut = psnr(out, truth), pBil = psnr(bilinear, truth);
        printf("output: mean %.3f, %zu NaN/Inf values; PSNR vs display-resolution scene %.2f dB (bilinear upscale %.2f dB)\n",
               mean, bad, pOut, pBil);
        if (gpuMsCount) printf("GPU time per dispatch (timestamps): %.2f ms (mean of %u frames)\n", gpuMsTotal / gpuMsCount, gpuMsCount);
        else printf("GPU time per dispatch (timestamps): unavailable\n");
        if (wallMsCount) printf("submit-to-complete per dispatch: %.2f ms (mean of %u frames)\n", wallMsTotal / wallMsCount, wallMsCount);
        if (!opt.images.empty()) {
            std::string tag;
            for (const char *c = versionName; *c; c++) tag += isalnum((unsigned char)*c) ? *c : '_';
            CreateDirectoryA(opt.images.c_str(), nullptr);
            writePPM(opt.images + "\\" + tag + "-output.ppm", out, opt.displayW, opt.displayH);
            writePPM(opt.images + "\\truth.ppm", truth, opt.displayW, opt.displayH);
            writePPM(opt.images + "\\bilinear.ppm", bilinear, opt.displayW, opt.displayH);
        }
        if (bad || mean < 0.02) { printf("FAIL output is not a finite image\n"); failed = 1; }
        else if (pOut < pBil) { printf("FAIL output is further from the scene than a bilinear upscale\n"); failed = 1; }
        else printf("PASS\n");
    }

    g_ffx.DestroyContext(&ctx, nullptr);
    color->Release(); depth->Release(); motion->Release(); output->Release();
    stagingColor->Release(); stagingDepth->Release(); stagingMotion->Release(); readback->Release();
    timestamps->Release(); tsBuffer->Release();
    return failed;
}

int main(int argc, char **argv) {
    Options opt;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--version") && i + 1 < argc) opt.version = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) opt.frames = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--images") && i + 1 < argc) opt.images = argv[++i];
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%ux%u", &opt.displayW, &opt.displayH);
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
            const char *m = argv[++i];
            opt.mode = !strcmp(m, "quality") ? FFX_UPSCALE_QUALITY_MODE_QUALITY : !strcmp(m, "balanced") ? FFX_UPSCALE_QUALITY_MODE_BALANCED
                     : !strcmp(m, "ultra") ? FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE : FFX_UPSCALE_QUALITY_MODE_PERFORMANCE;
        } else { printf("usage: fsr4probe [--version <substring>] [--frames N] [--mode quality|balanced|performance|ultra] [--size WxH] [--images <dir>]\n"); return 1; }
    }
    if (opt.frames < 2) opt.frames = 2;

    CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g_dev)));
    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT};
    CHECK(g_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue)));
    CHECK(g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc)));
    CHECK(g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc, nullptr, IID_PPV_ARGS(&g_list)));
    CHECK(g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)));
    g_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    printDevice();
    // FSR_PROBE_LOAD_AMDXC=1: load amdxc64.dll from beside the exe as VKD3D-Proton would with FSR 4 on, for runs
    // without that VKD3D-Proton build.
    if (const char *e = getenv("FSR_PROBE_LOAD_AMDXC"); e && *e == '1')
        printf("amdxc64.dll %s\n", LoadLibraryW(L"amdxc64.dll") ? "loaded" : "not found");
    printf("amdxc64.dll in the process: %s\n", GetModuleHandleW(L"amdxc64.dll") ? "yes" : "no");

    HMODULE loader = LoadLibraryW(L"amd_fidelityfx_loader_dx12.dll");
    if (!loader) { printf("FAIL amd_fidelityfx_loader_dx12.dll not found next to the exe (error %lu)\n", GetLastError()); return 1; }
    ffxLoadFunctions(&g_ffx, loader);
    if (!g_ffx.Query || !g_ffx.CreateContext) { printf("FAIL loader exports missing\n"); return 1; }

    uint64_t count = 0;
    ffxQueryDescGetVersions vq = {}; vq.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    vq.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE; vq.device = g_dev; vq.outputCount = &count;
    ffxReturnCode_t rc = g_ffx.Query(nullptr, &vq.header);
    std::vector<uint64_t> ids(count); std::vector<const char *> names(count);
    vq.versionIds = ids.data(); vq.versionNames = names.data();
    if (rc == FFX_API_RETURN_OK && count) rc = g_ffx.Query(nullptr, &vq.header);
    printf("upscaler versions offered on this device (return code %u): %llu\n", rc, (unsigned long long)count);
    for (uint64_t i = 0; i < count; i++) printf("  %#018llx  %s\n", (unsigned long long)ids[i], names[i]);

    int failures = 0, runs = 0;
    for (uint64_t i = 0; i < count; i++) {
        if (!opt.version.empty() && !strstr(names[i], opt.version.c_str())) continue;
        runs++;
        failures += runVersion(opt, ids[i], names[i]);
    }
    if (!runs) { printf("FAIL no upscaler version matched \"%s\"\n", opt.version.c_str()); return 1; }
    printf("\n%d of %d versions failed\n", failures, runs);
    return failures ? 1 : 0;
}
