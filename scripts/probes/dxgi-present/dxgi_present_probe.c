/* DXGI-2 windowed presentation probe.
 * This intentionally stops at swapchain/present validation. Resize, fullscreen,
 * and long-run recovery belong to later DXGI stages.
 */
#include <windows.h>
#define COBJMACROS
#include <initguid.h>
#include <dxgi1_5.h>
#include <d3d12.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH 640
#define HEIGHT 480
#define BUFFER_COUNT 2
#define FRAME_COUNT 1000
#define ROW_PITCH (WIDTH * 4)
#define READBACK_SIZE ((UINT64)ROW_PITCH * HEIGHT)

typedef struct Runtime {
    IDXGIFactory1 *factory;
    IDXGIFactory2 *factory2;
    IDXGIAdapter1 *adapter;
    ID3D12Device *device;
    ID3D12CommandQueue *queue;
    ID3D12CommandAllocator *allocator;
    ID3D12GraphicsCommandList *list;
    ID3D12Fence *fence;
    HANDLE fence_event;
    UINT64 fence_value;
    ID3D12RootSignature *root_signature;
    ID3D12PipelineState *pso;
    unsigned char *vs;
    unsigned char *ps;
    unsigned long vs_size;
    unsigned long ps_size;
    HWND window;
    BOOL tearing_supported;
} Runtime;

typedef struct ModeResult {
    const char *name;
    int for_hwnd;
    DXGI_SWAP_EFFECT effect;
    int present1;
    HRESULT create_hr;
    HRESULT present_test_hr;
    HRESULT first_present_hr;
    HRESULT last_present_hr;
    HRESULT frame_stats_hr;
    HRESULT last_count_hr;
    UINT last_count;
    UINT64 frame_count;
    int pixel_ok;
    int supported;
} ModeResult;

static const char *hr_text(HRESULT hr)
{
    static char text[32];
    snprintf(text, sizeof(text), "0x%08lx", (unsigned long)hr);
    return text;
}

static int hr_ok_or_unsupported(HRESULT hr)
{
    /* GetFrameStatistics may also return DXGI_ERROR_FRAME_STATISTICS_DISJOINT (documented, windowed DXVK 2). */
    return SUCCEEDED(hr) || hr == DXGI_ERROR_UNSUPPORTED || hr == E_NOTIMPL || hr == DXGI_ERROR_FRAME_STATISTICS_DISJOINT;
}

static int load_file(const char *path, unsigned char **data, unsigned long *size)
{
    FILE *file = fopen(path, "rb");
    long length;
    if (!file) return 0;
    fseek(file, 0, SEEK_END);
    length = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (length <= 0) { fclose(file); return 0; }
    *data = malloc((size_t)length);
    if (!*data || fread(*data, 1, (size_t)length, file) != (size_t)length) {
        free(*data); *data = NULL; fclose(file); return 0;
    }
    fclose(file);
    *size = (unsigned long)length;
    return 1;
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_CLOSE) {
        DestroyWindow(window);
        return 0;
    }
    return DefWindowProcA(window, message, wparam, lparam);
}

static HWND create_window(void)
{
    WNDCLASSA klass;
    HWND window;
    memset(&klass, 0, sizeof(klass));
    klass.lpfnWndProc = window_proc;
    klass.hInstance = GetModuleHandleA(NULL);
    klass.lpszClassName = "VKD3DProtonDXGIPresentProbe";
    klass.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    RegisterClassA(&klass);
    window = CreateWindowExA(0, klass.lpszClassName, "VKD3D-Proton DXGI-2",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, WIDTH, HEIGHT,
            NULL, NULL, klass.hInstance, NULL);
    if (window) {
        ShowWindow(window, SW_SHOWNORMAL);
        UpdateWindow(window);
    }
    return window;
}

static void pump_messages(void)
{
    MSG message;
    while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
}

static int wait_fence(Runtime *runtime)
{
    if (runtime->fence->lpVtbl->GetCompletedValue(runtime->fence) < runtime->fence_value) {
        HRESULT hr = runtime->fence->lpVtbl->SetEventOnCompletion(runtime->fence,
                runtime->fence_value, runtime->fence_event);
        if (FAILED(hr)) return 0;
        if (WaitForSingleObject(runtime->fence_event, 30000) != WAIT_OBJECT_0)
            return 0;
    }
    return 1;
}

static int signal_and_wait(Runtime *runtime)
{
    runtime->fence_value++;
    if (FAILED(runtime->queue->lpVtbl->Signal(runtime->queue, runtime->fence,
            runtime->fence_value))) return 0;
    return wait_fence(runtime);
}

static void release_runtime(Runtime *runtime)
{
    if (runtime->queue && runtime->fence) {
        runtime->fence_value++;
        runtime->queue->lpVtbl->Signal(runtime->queue, runtime->fence, runtime->fence_value);
        wait_fence(runtime);
    }
    if (runtime->pso) ID3D12PipelineState_Release(runtime->pso);
    if (runtime->root_signature) ID3D12RootSignature_Release(runtime->root_signature);
    if (runtime->list) ID3D12GraphicsCommandList_Release(runtime->list);
    if (runtime->allocator) ID3D12CommandAllocator_Release(runtime->allocator);
    if (runtime->fence) ID3D12Fence_Release(runtime->fence);
    if (runtime->queue) ID3D12CommandQueue_Release(runtime->queue);
    if (runtime->device) ID3D12Device_Release(runtime->device);
    if (runtime->adapter) IDXGIAdapter1_Release(runtime->adapter);
    if (runtime->factory2) IDXGIFactory2_Release(runtime->factory2);
    if (runtime->factory) IDXGIFactory1_Release(runtime->factory);
    if (runtime->fence_event) CloseHandle(runtime->fence_event);
    if (runtime->window) DestroyWindow(runtime->window);
    free(runtime->vs);
    free(runtime->ps);
    memset(runtime, 0, sizeof(*runtime));
}

static int create_runtime(Runtime *runtime)
{
    D3D12_COMMAND_QUEUE_DESC queue_desc;
    D3D12_ROOT_SIGNATURE_DESC root_desc;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc;   /* The real one: mesh shaders come with pipeline state streams. */
    ID3DBlob *root_blob = NULL, *root_error = NULL;
    DXGI_ADAPTER_DESC1 adapter_desc;
    LUID device_luid;
    HRESULT hr;
    UINT i;

    memset(runtime, 0, sizeof(*runtime));
    runtime->window = create_window();
    if (!runtime->window) { printf("window: FAIL\n"); return 0; }

    hr = CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&runtime->factory);
    if (FAILED(hr)) { printf("factory: %s\n", hr_text(hr)); return 0; }
    hr = runtime->factory->lpVtbl->QueryInterface(runtime->factory,
            &IID_IDXGIFactory2, (void **)&runtime->factory2);
    if (FAILED(hr)) { printf("factory2: %s\n", hr_text(hr)); return 0; }
    runtime->factory->lpVtbl->MakeWindowAssociation(runtime->factory, runtime->window,
            DXGI_MWA_NO_ALT_ENTER);

    for (i = 0; runtime->factory->lpVtbl->EnumAdapters1(runtime->factory, i,
            &runtime->adapter) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 desc;
        runtime->adapter->lpVtbl->GetDesc1(runtime->adapter, &desc);
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) break;
        IDXGIAdapter1_Release(runtime->adapter);
        runtime->adapter = NULL;
    }
    if (!runtime->adapter) { printf("hardware adapter: FAIL\n"); return 0; }
    memset(&adapter_desc, 0, sizeof(adapter_desc));
    hr = runtime->adapter->lpVtbl->GetDesc1(runtime->adapter, &adapter_desc);
    if (FAILED(hr)) { printf("adapter identity: FAIL %s\n", hr_text(hr)); return 0; }
    printf("selected adapter: vendor=0x%04x device=0x%04x luid=%08lx-%08lx\n",
            adapter_desc.VendorId, adapter_desc.DeviceId,
            (unsigned long)adapter_desc.AdapterLuid.HighPart,
            (unsigned long)adapter_desc.AdapterLuid.LowPart);

    hr = D3D12CreateDevice((IUnknown *)runtime->adapter, D3D_FEATURE_LEVEL_11_0,
            &IID_ID3D12Device, (void **)&runtime->device);
    if (FAILED(hr)) { printf("D3D12CreateDevice: %s\n", hr_text(hr)); return 0; }
    memset(&device_luid, 0, sizeof(device_luid));
    runtime->device->lpVtbl->GetAdapterLuid(runtime->device, &device_luid);
    printf("D3D12 adapter LUID: %08lx-%08lx (%s)\n",
            (unsigned long)device_luid.HighPart,
            (unsigned long)device_luid.LowPart,
            memcmp(&adapter_desc.AdapterLuid, &device_luid, sizeof(LUID)) == 0 ? "MATCH" : "MISMATCH");
    if (memcmp(&adapter_desc.AdapterLuid, &device_luid, sizeof(LUID)) != 0) {
        printf("adapter identity: FAIL\n");
        return 0;
    }

    memset(&queue_desc, 0, sizeof(queue_desc));
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = runtime->device->lpVtbl->CreateCommandQueue(runtime->device, &queue_desc,
            &IID_ID3D12CommandQueue, (void **)&runtime->queue);
    if (FAILED(hr)) { printf("CreateCommandQueue: %s\n", hr_text(hr)); return 0; }
    hr = runtime->device->lpVtbl->CreateCommandAllocator(runtime->device,
            D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator,
            (void **)&runtime->allocator);
    if (FAILED(hr)) return 0;
    hr = runtime->device->lpVtbl->CreateCommandList(runtime->device, 0,
            D3D12_COMMAND_LIST_TYPE_DIRECT, runtime->allocator, NULL,
            &IID_ID3D12GraphicsCommandList, (void **)&runtime->list);
    if (FAILED(hr)) return 0;
    runtime->list->lpVtbl->Close(runtime->list);
    hr = runtime->device->lpVtbl->CreateFence(runtime->device, 0, D3D12_FENCE_FLAG_NONE,
            &IID_ID3D12Fence, (void **)&runtime->fence);
    if (FAILED(hr)) return 0;
    runtime->fence_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!runtime->fence_event) return 0;

    if (!load_file("dxgi-present/triangle_vs.dxil", &runtime->vs, &runtime->vs_size) ||
            !load_file("dxgi-present/triangle_ps.dxil", &runtime->ps, &runtime->ps_size)) {
        printf("shader files: MISSING\n");
        return 0;
    }
    memset(&root_desc, 0, sizeof(root_desc));
    hr = D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1,
            &root_blob, &root_error);
    if (FAILED(hr)) { printf("root signature: %s\n", hr_text(hr)); return 0; }
    hr = runtime->device->lpVtbl->CreateRootSignature(runtime->device, 0,
            root_blob->lpVtbl->GetBufferPointer(root_blob),
            root_blob->lpVtbl->GetBufferSize(root_blob), &IID_ID3D12RootSignature,
            (void **)&runtime->root_signature);
    if (root_blob) root_blob->lpVtbl->Release(root_blob);
    if (root_error) root_error->lpVtbl->Release(root_error);
    if (FAILED(hr)) return 0;

    memset(&pso_desc, 0, sizeof(pso_desc));
    pso_desc.pRootSignature = runtime->root_signature;
    pso_desc.VS.pShaderBytecode = runtime->vs;
    pso_desc.VS.BytecodeLength = runtime->vs_size;
    pso_desc.PS.pShaderBytecode = runtime->ps;
    pso_desc.PS.BytecodeLength = runtime->ps_size;
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.RasterizerState.DepthClipEnable = TRUE;
    pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso_desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    pso_desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
    pso_desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    pso_desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    pso_desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    pso_desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    pso_desc.SampleMask = 0xffffffffu;
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    pso_desc.SampleDesc.Count = 1;
    hr = runtime->device->lpVtbl->CreateGraphicsPipelineState(runtime->device,
            (const D3D12_GRAPHICS_PIPELINE_STATE_DESC *)(const void *)&pso_desc,
            &IID_ID3D12PipelineState, (void **)&runtime->pso);
    if (FAILED(hr)) { printf("graphics PSO: %s\n", hr_text(hr)); return 0; }

    {
        IDXGIFactory5 *factory5 = NULL;
        BOOL tearing = FALSE;
        hr = runtime->factory->lpVtbl->QueryInterface(runtime->factory,
                &IID_IDXGIFactory5, (void **)&factory5);
        if (SUCCEEDED(hr)) {
            hr = factory5->lpVtbl->CheckFeatureSupport(factory5,
                    DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
            printf("tearing support: hr=%s supported=%u\n", hr_text(hr), tearing);
            if (SUCCEEDED(hr)) runtime->tearing_supported = tearing;
            IDXGIFactory5_Release(factory5);
        } else {
            printf("tearing support: unavailable (%s)\n", hr_text(hr));
        }
    }
    printf("window/device/queue: PASS\n");
    return 1;
}

static int verify_pixels(ID3D12Resource *readback)
{
    D3D12_RANGE range = { 0, READBACK_SIZE };
    unsigned char *pixels = NULL;
    unsigned char red[4];
    unsigned char green[4];
    unsigned char blue[4];
    unsigned char corner[4];
    int ok;
    if (FAILED(readback->lpVtbl->Map(readback, 0, &range, (void **)&pixels)) || !pixels)
        return 0;
    /* Sample well inside each colored vertex region, away from edges. */
    memcpy(red, pixels + 360 * ROW_PITCH + 160 * 4, 4);
    memcpy(green, pixels + 360 * ROW_PITCH + 480 * 4, 4);
    memcpy(blue, pixels + 120 * ROW_PITCH + 320 * 4, 4);
    memcpy(corner, pixels, 4);
    /* BGRA8: RGB vertex colors, with a dark-blue clear outside the triangle. */
    ok = red[2] > 180 && red[1] < 80 && red[0] < 80 &&
            green[1] > 180 && green[2] < 80 && green[0] < 80 &&
            blue[0] > 180 && blue[1] < 80 && blue[2] < 80 &&
            corner[2] < 80 && corner[1] < 80 && corner[0] > 20;
    printf("  readback rgb-red=%02x%02x%02x%02x rgb-green=%02x%02x%02x%02x rgb-blue=%02x%02x%02x%02x clear=%02x%02x%02x%02x: %s\n",
            red[0], red[1], red[2], red[3],
            green[0], green[1], green[2], green[3],
            blue[0], blue[1], blue[2], blue[3],
            corner[0], corner[1], corner[2], corner[3], ok ? "PASS" : "FAIL");
    readback->lpVtbl->Unmap(readback, 0, NULL);
    return ok;
}

static int run_mode(Runtime *runtime, const char *name, int for_hwnd,
        DXGI_SWAP_EFFECT effect, int use_present1)
{
    DXGI_SWAP_CHAIN_DESC desc;
    DXGI_SWAP_CHAIN_DESC1 desc1;
    IDXGISwapChain *swapchain = NULL;
    IDXGISwapChain1 *swapchain1 = NULL;
    IDXGISwapChain1 *swapchain1_query = NULL;
    IDXGISwapChain3 *swapchain3 = NULL;
    ID3D12DescriptorHeap *rtv_heap = NULL;
    ID3D12Resource *buffers[BUFFER_COUNT] = { NULL, NULL };
    ID3D12Resource *readback = NULL;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_start;
    UINT descriptor_size;
    HRESULT hr;
    UINT i;
    int ok = 1;
    ModeResult result;
    memset(&result, 0, sizeof(result));
    result.name = name;
    result.for_hwnd = for_hwnd;
    result.effect = effect;
    result.present1 = use_present1;

    memset(&desc, 0, sizeof(desc));
    desc.BufferCount = BUFFER_COUNT;
    desc.BufferDesc.Width = WIDTH;
    desc.BufferDesc.Height = HEIGHT;
    desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = runtime->window;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = effect;
    desc.Flags = runtime->tearing_supported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

    memset(&desc1, 0, sizeof(desc1));
    desc1.Width = WIDTH;
    desc1.Height = HEIGHT;
    desc1.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc1.SampleDesc.Count = 1;
    desc1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc1.BufferCount = BUFFER_COUNT;
    desc1.Scaling = DXGI_SCALING_STRETCH;
    desc1.SwapEffect = effect;
    desc1.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc1.Flags = desc.Flags;

    if (for_hwnd) {
        hr = runtime->factory2->lpVtbl->CreateSwapChainForHwnd(runtime->factory2,
                (IUnknown *)runtime->queue, runtime->window, &desc1, NULL, NULL, &swapchain1);
        swapchain = (IDXGISwapChain *)swapchain1;
    } else {
        hr = runtime->factory->lpVtbl->CreateSwapChain(runtime->factory,
                (IUnknown *)runtime->queue, &desc, &swapchain);
    }
    result.create_hr = hr;
    printf("\n=== %s ===\n", name);
    printf("  create: hr=%s %s\n", hr_text(hr), SUCCEEDED(hr) ? "PASS" : "UNSUPPORTED/FAIL");
    if (FAILED(hr)) {
        result.supported = 0;
        return hr == DXGI_ERROR_UNSUPPORTED || hr == E_NOTIMPL;
    }
    result.supported = 1;
    if (!swapchain1) {
        swapchain->lpVtbl->QueryInterface(swapchain, &IID_IDXGISwapChain1,
                (void **)&swapchain1_query);
        swapchain1 = swapchain1_query;
    }
    swapchain->lpVtbl->QueryInterface(swapchain, &IID_IDXGISwapChain3, (void **)&swapchain3);
    if (!swapchain1 || !swapchain3) { printf("  swapchain interfaces: FAIL\n"); ok = 0; goto done; }

    hr = use_present1 ? swapchain1->lpVtbl->Present1(swapchain1, 0,
            DXGI_PRESENT_TEST, NULL) : swapchain->lpVtbl->Present(swapchain, 0,
            DXGI_PRESENT_TEST);
    result.present_test_hr = hr;
    printf("  Present%s(DXGI_PRESENT_TEST): hr=%s %s\n", use_present1 ? "1" : "",
            hr_text(hr), SUCCEEDED(hr) ? "PASS" : "FAIL");
    if (FAILED(hr)) ok = 0;

    {
        D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC buffer = { D3D12_RESOURCE_DIMENSION_BUFFER, 0,
                READBACK_SIZE, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 },
                D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE };
        hr = runtime->device->lpVtbl->CreateCommittedResource(runtime->device, &heap,
                D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                &IID_ID3D12Resource, (void **)&readback);
        if (FAILED(hr)) { printf("  readback buffer: FAIL %s\n", hr_text(hr)); ok = 0; goto done; }
    }
    {
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
                BUFFER_COUNT, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
        hr = runtime->device->lpVtbl->CreateDescriptorHeap(runtime->device, &heap_desc,
                &IID_ID3D12DescriptorHeap, (void **)&rtv_heap);
        if (FAILED(hr)) { printf("  RTV heap: FAIL %s\n", hr_text(hr)); ok = 0; goto done; }
        rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(rtv_heap, &rtv_start);
        descriptor_size = runtime->device->lpVtbl->GetDescriptorHandleIncrementSize(runtime->device,
                D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        for (i = 0; i < BUFFER_COUNT; i++) {
            hr = swapchain->lpVtbl->GetBuffer(swapchain, i, &IID_ID3D12Resource,
                    (void **)&buffers[i]);
            if (FAILED(hr)) { printf("  GetBuffer[%u]: FAIL %s\n", i, hr_text(hr)); ok = 0; goto done; }
            {
                D3D12_CPU_DESCRIPTOR_HANDLE handle = rtv_start;
                handle.ptr += (SIZE_T)i * descriptor_size;
                runtime->device->lpVtbl->CreateRenderTargetView(runtime->device,
                        buffers[i], NULL, handle);
            }
        }
    }

    for (i = 0; i < FRAME_COUNT && ok; i++) {
        UINT index;
        D3D12_RESOURCE_BARRIER barrier;
        D3D12_CPU_DESCRIPTOR_HANDLE handle;
        D3D12_VIEWPORT viewport = { 0, 0, WIDTH, HEIGHT, 0, 1 };
        D3D12_RECT scissor = { 0, 0, WIDTH, HEIGHT };
        D3D12_TEXTURE_COPY_LOCATION dst;
        D3D12_TEXTURE_COPY_LOCATION src;
        float clear[4] = { 0.02f, 0.02f, 0.20f, 1.0f };
        UINT sync = (i & 1) ? 1 : 0;
        UINT present_flags = (runtime->tearing_supported && sync == 0) ?
                DXGI_PRESENT_ALLOW_TEARING : 0;
        pump_messages();
        index = swapchain3->lpVtbl->GetCurrentBackBufferIndex(swapchain3);
        handle = rtv_start;
        handle.ptr += (SIZE_T)index * descriptor_size;
        runtime->allocator->lpVtbl->Reset(runtime->allocator);
        runtime->list->lpVtbl->Reset(runtime->list, runtime->allocator, runtime->pso);
        memset(&barrier, 0, sizeof(barrier));
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = buffers[index];
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        runtime->list->lpVtbl->ResourceBarrier(runtime->list, 1, &barrier);
        runtime->list->lpVtbl->OMSetRenderTargets(runtime->list, 1, &handle, FALSE, NULL);
        runtime->list->lpVtbl->ClearRenderTargetView(runtime->list, handle, clear, 0, NULL);
        runtime->list->lpVtbl->RSSetViewports(runtime->list, 1, &viewport);
        runtime->list->lpVtbl->RSSetScissorRects(runtime->list, 1, &scissor);
        runtime->list->lpVtbl->IASetPrimitiveTopology(runtime->list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        runtime->list->lpVtbl->DrawInstanced(runtime->list, 3, 1, 0, 0);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        runtime->list->lpVtbl->ResourceBarrier(runtime->list, 1, &barrier);
        memset(&dst, 0, sizeof(dst));
        dst.pResource = readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        dst.PlacedFootprint.Footprint.Width = WIDTH;
        dst.PlacedFootprint.Footprint.Height = HEIGHT;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = ROW_PITCH;
        memset(&src, 0, sizeof(src));
        src.pResource = buffers[index];
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        runtime->list->lpVtbl->CopyTextureRegion(runtime->list, &dst, 0, 0, 0, &src, NULL);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        runtime->list->lpVtbl->ResourceBarrier(runtime->list, 1, &barrier);
        runtime->list->lpVtbl->Close(runtime->list);
        {
            ID3D12CommandList *lists[] = { (ID3D12CommandList *)runtime->list };
            runtime->queue->lpVtbl->ExecuteCommandLists(runtime->queue, 1, lists);
        }
        if (!signal_and_wait(runtime)) { printf("  frame %u fence: FAIL\n", i); ok = 0; break; }
        if (i == 0) {
            result.pixel_ok = verify_pixels(readback);
            if (!result.pixel_ok) ok = 0;
        }
        hr = use_present1 ? swapchain1->lpVtbl->Present1(swapchain1, sync,
                present_flags, NULL) : swapchain->lpVtbl->Present(swapchain, sync,
                present_flags);
        if (i == 0) result.first_present_hr = hr;
        result.last_present_hr = hr;
        if (FAILED(hr)) {
            printf("  frame %u Present%s: FAIL %s\n", i, use_present1 ? "1" : "", hr_text(hr));
            ok = 0;
            break;
        }
        result.frame_count++;
    }
    printf("  frames: %llu / %u\n", (unsigned long long)result.frame_count, FRAME_COUNT);
    printf("  sync intervals exercised: 0 and 1; tearing flag on interval 0: %s\n",
            runtime->tearing_supported ? "ALLOW_TEARING" : "not-requested");
    if (result.frame_count != FRAME_COUNT) ok = 0;

    {
        DXGI_FRAME_STATISTICS stats;
        memset(&stats, 0, sizeof(stats));
        result.frame_stats_hr = swapchain->lpVtbl->GetFrameStatistics(swapchain, &stats);
        result.last_count_hr = swapchain->lpVtbl->GetLastPresentCount(swapchain, &result.last_count);
        printf("  GetFrameStatistics: hr=%s %s\n", hr_text(result.frame_stats_hr),
                hr_ok_or_unsupported(result.frame_stats_hr) ? "PASS/UNSUPPORTED" : "FAIL");
        printf("  GetLastPresentCount: hr=%s count=%u %s\n", hr_text(result.last_count_hr),
                result.last_count, hr_ok_or_unsupported(result.last_count_hr) ? "PASS/UNSUPPORTED" : "FAIL");
        if (!hr_ok_or_unsupported(result.frame_stats_hr) || !hr_ok_or_unsupported(result.last_count_hr)) ok = 0;
        if (SUCCEEDED(result.last_count_hr) && result.last_count == 0) ok = 0;
    }

done:
    printf("  mode result: %s\n", ok ? "PASS" : "FAIL");
    for (i = 0; i < BUFFER_COUNT; i++) if (buffers[i]) ID3D12Resource_Release(buffers[i]);
    if (readback) ID3D12Resource_Release(readback);
    if (rtv_heap) ID3D12DescriptorHeap_Release(rtv_heap);
    if (swapchain3) IDXGISwapChain3_Release(swapchain3);
    if (swapchain1_query) IDXGISwapChain1_Release(swapchain1_query);
    if (swapchain) IDXGISwapChain_Release(swapchain);
    return ok;
}

static int negative_tests(Runtime *runtime)
{
    DXGI_SWAP_CHAIN_DESC desc;
    IDXGISwapChain *swapchain = NULL;
    HRESULT hr;
    int failures = 0;
    memset(&desc, 0, sizeof(desc));
    desc.BufferCount = 0;
    desc.BufferDesc.Width = WIDTH;
    desc.BufferDesc.Height = HEIGHT;
    desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = runtime->window;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    hr = runtime->factory->lpVtbl->CreateSwapChain(runtime->factory,
            (IUnknown *)runtime->queue, &desc, &swapchain);
    printf("  invalid buffer count: hr=%s %s\n", hr_text(hr),
            FAILED(hr) ? "PASS" : "FAIL");
    if (SUCCEEDED(hr)) { failures++; IDXGISwapChain_Release(swapchain); }

    desc.BufferCount = BUFFER_COUNT;
    desc.BufferDesc.Format = DXGI_FORMAT_UNKNOWN;
    hr = runtime->factory->lpVtbl->CreateSwapChain(runtime->factory,
            (IUnknown *)runtime->queue, &desc, &swapchain);
    printf("  invalid format: hr=%s %s\n", hr_text(hr), FAILED(hr) ? "PASS" : "FAIL");
    if (SUCCEEDED(hr)) { failures++; IDXGISwapChain_Release(swapchain); }

    desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    hr = runtime->factory->lpVtbl->CreateSwapChain(runtime->factory,
            (IUnknown *)runtime->queue, &desc, &swapchain);
    if (SUCCEEDED(hr)) {
        IDXGISwapChain *held = swapchain;
        held->lpVtbl->AddRef(held);
        held->lpVtbl->Release(held);
        hr = held->lpVtbl->Present(held, 0, DXGI_PRESENT_TEST);
        printf("  post-release presentation (retained COM ref): hr=%s %s\n",
                hr_text(hr), SUCCEEDED(hr) ? "PASS" : "FAIL");
        if (FAILED(hr)) failures++;
        held->lpVtbl->Release(held);
    } else {
        printf("  post-release presentation setup: FAIL %s\n", hr_text(hr));
        failures++;
    }
    printf("negative result: %s (%d failure%s)\n", failures ? "FAIL" : "PASS",
            failures, failures == 1 ? "" : "s");
    return failures == 0;
}

int main(void)
{
    Runtime runtime;
    int ok = 1;
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== DXGI-2 windowed presentation probe ===\n");
    printf("module paths are emitted by the Wine/DXVK runtime logs\n");
    if (!create_runtime(&runtime)) {
        release_runtime(&runtime);
        return 1;
    }
    ok &= run_mode(&runtime, "CreateSwapChain flip-discard + Present", 0,
            DXGI_SWAP_EFFECT_FLIP_DISCARD, 0);
    ok &= run_mode(&runtime, "CreateSwapChain flip-sequential + Present", 0,
            DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, 0);
    ok &= run_mode(&runtime, "CreateSwapChainForHwnd flip-discard + Present1", 1,
            DXGI_SWAP_EFFECT_FLIP_DISCARD, 1);
    ok &= run_mode(&runtime, "CreateSwapChainForHwnd flip-sequential + Present1", 1,
            DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, 1);
    ok &= negative_tests(&runtime);
    release_runtime(&runtime);
    printf("DXGI-2 result: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
