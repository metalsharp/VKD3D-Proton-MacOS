/* tess_stress: Red Dead Redemption 2's pattern of Metal memory growth. Many command allocators (VKD3D-Proton makes a
 * Vulkan command pool for each) record tessellated draws of different sizes, which MoltenVK runs with large temporary
 * buffers. Run it with METALSHARP_TRACE=memory: the Metal device's allocated size must level off, not grow with every
 * allocator.
 *   tess_stress.exe [seconds] [allocators] [patches] [patch step]
 * Defaults: 40 s, 48 allocators, 400 + 150 * k patches with k moving through 32 sizes over the frames, which
 * MoltenVK gives temporary buffers between 256 KB and 16 MB (RDR2's range: each command pool kept every one it ever
 * needed at once).
 * Build: tests/memory/build.sh. */
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#define INITGUID
#include <windows.h>
#include <d3d12.h>
#include <stdio.h>
#include <stdlib.h>
#include "tess_stress_shaders.h"

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("FAIL %s = %#lx (line %d)\n", #x, (long)hr_, __LINE__); return 1; } } while (0)
#define MAX_ALLOCATORS 256

int main(int argc, char **argv)
{
    unsigned int seconds = argc > 1 ? atoi(argv[1]) : 40, count = argc > 2 ? atoi(argv[2]) : 48;
    unsigned int patches = argc > 3 ? atoi(argv[3]) : 400, step = argc > 4 ? atoi(argv[4]) : 150;
    D3D12_ROOT_SIGNATURE_DESC rs_desc = { 0, NULL, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {0};
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    D3D12_HEAP_PROPERTIES default_heap = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC rt_desc = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 256, 256, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
            { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET };
    D3D12_VIEWPORT vp = { 0, 0, 256, 256, 0, 1 };
    D3D12_RECT sci = { 0, 0, 256, 256 };
    ID3D12CommandAllocator *allocs[MAX_ALLOCATORS];
    UINT64 alloc_fence[MAX_ALLOCATORS] = {0};
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    ID3D12GraphicsCommandList *list;
    ID3D12DescriptorHeap *rtv_heap;
    ID3D12RootSignature *rs;
    ID3D12PipelineState *pso;
    ID3D12CommandQueue *queue;
    ID3D12Resource *rt;
    ID3D12Device *dev;
    ID3D12Fence *fence;
    ID3DBlob *blob;
    DWORD start, last;
    UINT64 frame = 0;
    unsigned int i;

    if (count > MAX_ALLOCATORS) count = MAX_ALLOCATORS;
    CHECK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_12_0, &IID_ID3D12Device, (void **)&dev));
    CHECK(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue));
    for (i = 0; i < count; i++)
        CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&allocs[i]));
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocs[0], NULL, &IID_ID3D12GraphicsCommandList, (void **)&list));
    ID3D12GraphicsCommandList_Close(list);
    CHECK(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence));
    CHECK(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, NULL));
    CHECK(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
            &IID_ID3D12RootSignature, (void **)&rs));
    pso_desc.pRootSignature = rs;
    pso_desc.VS.pShaderBytecode = vs_dxil; pso_desc.VS.BytecodeLength = sizeof(vs_dxil);
    pso_desc.HS.pShaderBytecode = hs_dxil; pso_desc.HS.BytecodeLength = sizeof(hs_dxil);
    pso_desc.DS.pShaderBytecode = ds_dxil; pso_desc.DS.BytecodeLength = sizeof(ds_dxil);
    pso_desc.PS.pShaderBytecode = ps_dxil; pso_desc.PS.BytecodeLength = sizeof(ps_dxil);
    pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso_desc.SampleMask = ~0u;
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso_desc.SampleDesc.Count = 1;
    CHECK(ID3D12Device_CreateGraphicsPipelineState(dev, &pso_desc, &IID_ID3D12PipelineState, (void **)&pso));
    CHECK(ID3D12Device_CreateCommittedResource(dev, &default_heap, D3D12_HEAP_FLAG_NONE, &rt_desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void **)&rt));
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &rtv_heap_desc, &IID_ID3D12DescriptorHeap, (void **)&rtv_heap));
    rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap);
    ID3D12Device_CreateRenderTargetView(dev, rt, NULL, rtv);

    printf("%u allocators, %u s, %u + %u * k patches (k < 32)\n", count, seconds, patches, step);
    fflush(stdout);
    start = last = GetTickCount();
    while (GetTickCount() - start < seconds * 1000)
    {
        unsigned int a = frame % count;
        /* Each allocator draws its own size, so its command pool needs temporary buffers of its own sizes. */
        UINT vertices = 3 * (patches + step * ((a + frame / count) % 32)), d;

        if (ID3D12Fence_GetCompletedValue(fence) < alloc_fence[a])
            ID3D12Fence_SetEventOnCompletion(fence, alloc_fence[a], NULL);
        ID3D12CommandAllocator_Reset(allocs[a]);
        ID3D12GraphicsCommandList_Reset(list, allocs[a], pso);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, rs);
        ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv, FALSE, NULL);
        ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
        ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sci);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
        for (d = 0; d < 4; d++)
            ID3D12GraphicsCommandList_DrawInstanced(list, vertices, 1, 0, 0);
        ID3D12GraphicsCommandList_Close(list);
        ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&list);
        alloc_fence[a] = ++frame;
        ID3D12CommandQueue_Signal(queue, fence, frame);
        if (GetTickCount() - last >= 5000)
        {
            last = GetTickCount();
            printf("%5.1f s: %llu frames\n", (last - start) / 1000.0, (unsigned long long)frame);
            fflush(stdout);
        }
    }
    ID3D12Fence_SetEventOnCompletion(fence, frame, NULL);
    printf("done: %llu frames\n", (unsigned long long)frame);
    return 0;
}
