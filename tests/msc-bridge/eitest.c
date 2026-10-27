/* ExecuteIndirect with root constant changes on Metal Shader Converter bridge PSOs (compute and graphics):
 * plain work before and after uses the bridge pipeline, the indirect commands the PSO's SPIR-V pipeline. */
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#define INITGUID
#include <windows.h>
#include <d3d12.h>
#include <stdio.h>
#include <string.h>
#include "cs.h"
#include "vs.h"
#include "ps.h"

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("FAIL %s = %#lx (line %d)\n", #x, (long)hr_, __LINE__); return 1; } } while (0)

static ID3D12Device *dev;
static ID3D12CommandQueue *queue;
static ID3D12CommandAllocator *alloc;
static ID3D12GraphicsCommandList *list;
static ID3D12Fence *fence;
static UINT64 fence_value;

static ID3D12Resource *make_buffer(UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp = { type };
    D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, size, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 },
            D3D12_TEXTURE_LAYOUT_ROW_MAJOR, flags };
    ID3D12Resource *r = NULL;
    ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, state, NULL, &IID_ID3D12Resource, (void **)&r);
    return r;
}

static void submit_and_wait(void)
{
    ID3D12GraphicsCommandList_Close(list);
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&list);
    ID3D12CommandQueue_Signal(queue, fence, ++fence_value);
    while (ID3D12Fence_GetCompletedValue(fence) < fence_value)
        Sleep(1);
    ID3D12CommandAllocator_Reset(alloc);
    ID3D12GraphicsCommandList_Reset(list, alloc, NULL);
}

static void barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
}

int main(void)
{
    ID3D12RootSignature *cs_rs, *gfx_rs;
    ID3D12PipelineState *cs_pso, *gfx_pso;
    ID3D12CommandSignature *cs_sig, *gfx_sig;
    int failures = 0;
    unsigned int i;
    UINT32 *p;

    CHECK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev));
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    CHECK(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue));
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc));
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list));
    CHECK(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence));

    /* ---- Compute: o[v] = v * 10 + 1; plain v = 4, indirect v = 0..3, plain v = 5. ---- */
    CHECK(ID3D12Device_CreateRootSignature(dev, 0, cs_dxil, sizeof(cs_dxil), &IID_ID3D12RootSignature, (void **)&cs_rs));
    D3D12_COMPUTE_PIPELINE_STATE_DESC cpd = { cs_rs, { cs_dxil, sizeof(cs_dxil) } };
    CHECK(ID3D12Device_CreateComputePipelineState(dev, &cpd, &IID_ID3D12PipelineState, (void **)&cs_pso));
    D3D12_INDIRECT_ARGUMENT_DESC cs_args[2] = { { D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT }, { D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH } };
    cs_args[0].Constant.RootParameterIndex = 0;
    cs_args[0].Constant.DestOffsetIn32BitValues = 0;
    cs_args[0].Constant.Num32BitValuesToSet = 1;
    D3D12_COMMAND_SIGNATURE_DESC csd = { 16, 2, cs_args };
    CHECK(ID3D12Device_CreateCommandSignature(dev, &csd, cs_rs, &IID_ID3D12CommandSignature, (void **)&cs_sig));

    ID3D12Resource *out = make_buffer(64, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource *cs_argbuf = make_buffer(4 * 16, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource_Map(cs_argbuf, 0, NULL, (void **)&p);
    for (i = 0; i < 4; i++) { p[i * 4] = i; p[i * 4 + 1] = 1; p[i * 4 + 2] = 1; p[i * 4 + 3] = 1; }
    ID3D12Resource_Unmap(cs_argbuf, 0, NULL);

    ID3D12GraphicsCommandList_SetComputeRootSignature(list, cs_rs);
    ID3D12GraphicsCommandList_SetPipelineState(list, cs_pso);
    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(list, 1, ID3D12Resource_GetGPUVirtualAddress(out));
    ID3D12GraphicsCommandList_SetComputeRoot32BitConstant(list, 0, 4, 0);
    ID3D12GraphicsCommandList_Dispatch(list, 1, 1, 1);
    ID3D12GraphicsCommandList_ExecuteIndirect(list, cs_sig, 4, cs_argbuf, 0, NULL, 0);
    ID3D12GraphicsCommandList_SetComputeRoot32BitConstant(list, 0, 5, 0);
    ID3D12GraphicsCommandList_Dispatch(list, 1, 1, 1);
    barrier(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12Resource *rb = make_buffer(64, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, rb, 0, out, 0, 64);
    submit_and_wait();
    ID3D12Resource_Map(rb, 0, NULL, (void **)&p);
    printf("compute:");
    for (i = 0; i < 6; i++)
    {
        printf(" o[%u]=%u", i, p[i]);
        if (p[i] != i * 10 + 1) failures++;
    }
    printf(" (want v * 10 + 1)\n");
    ID3D12Resource_Unmap(rb, 0, NULL);

    /* ---- Graphics: one point per draw at pixel v, red = v + 1; plain v = 4, indirect v = 0..3, plain v = 5. ---- */
    CHECK(ID3D12Device_CreateRootSignature(dev, 0, vs_dxil, sizeof(vs_dxil), &IID_ID3D12RootSignature, (void **)&gfx_rs));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC gpd;
    memset(&gpd, 0, sizeof(gpd));
    gpd.pRootSignature = gfx_rs;
    gpd.VS.pShaderBytecode = vs_dxil; gpd.VS.BytecodeLength = sizeof(vs_dxil);
    gpd.PS.pShaderBytecode = ps_dxil; gpd.PS.BytecodeLength = sizeof(ps_dxil);
    gpd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    gpd.SampleMask = ~0u;
    gpd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    gpd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    gpd.RasterizerState.DepthClipEnable = TRUE;
    gpd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    gpd.NumRenderTargets = 1;
    gpd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    gpd.SampleDesc.Count = 1;
    CHECK(ID3D12Device_CreateGraphicsPipelineState(dev, &gpd, &IID_ID3D12PipelineState, (void **)&gfx_pso));
    D3D12_INDIRECT_ARGUMENT_DESC gfx_args[2] = { { D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT }, { D3D12_INDIRECT_ARGUMENT_TYPE_DRAW } };
    gfx_args[0].Constant.RootParameterIndex = 0;
    gfx_args[0].Constant.DestOffsetIn32BitValues = 0;
    gfx_args[0].Constant.Num32BitValuesToSet = 1;
    D3D12_COMMAND_SIGNATURE_DESC gsd = { 20, 2, gfx_args };
    CHECK(ID3D12Device_CreateCommandSignature(dev, &gsd, gfx_rs, &IID_ID3D12CommandSignature, (void **)&gfx_sig));

    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 6, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 },
            D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET };
    D3D12_CLEAR_VALUE cv = { DXGI_FORMAT_R8G8B8A8_UNORM };
    ID3D12Resource *rt;
    CHECK(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
            &IID_ID3D12Resource, (void **)&rt));
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    ID3D12DescriptorHeap *rtv_heap;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtv_heap));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap);
    ID3D12Device_CreateRenderTargetView(dev, rt, NULL, rtv);

    ID3D12Resource *gfx_argbuf = make_buffer(4 * 20, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource_Map(gfx_argbuf, 0, NULL, (void **)&p);
    for (i = 0; i < 4; i++) { p[i * 5] = i; p[i * 5 + 1] = 1; p[i * 5 + 2] = 1; p[i * 5 + 3] = 0; p[i * 5 + 4] = 0; }
    ID3D12Resource_Unmap(gfx_argbuf, 0, NULL);

    const float black[4] = { 0, 0, 0, 0 };
    D3D12_VIEWPORT vp = { 0, 0, 6, 1, 0, 1 };
    D3D12_RECT sc = { 0, 0, 6, 1 };
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, black, 0, NULL);
    ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, gfx_rs);
    ID3D12GraphicsCommandList_SetPipelineState(list, gfx_pso);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstant(list, 0, 4, 0);
    ID3D12GraphicsCommandList_DrawInstanced(list, 1, 1, 0, 0);
    ID3D12GraphicsCommandList_ExecuteIndirect(list, gfx_sig, 4, gfx_argbuf, 0, NULL, 0);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstant(list, 0, 5, 0);
    ID3D12GraphicsCommandList_DrawInstanced(list, 1, 1, 0, 0);
    barrier(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12Resource *trb = make_buffer(256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst = { trb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = 6;
    dst.PlacedFootprint.Footprint.Height = 1;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = 256;
    D3D12_TEXTURE_COPY_LOCATION src = { rt, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
    submit_and_wait();
    ID3D12Resource_Map(trb, 0, NULL, (void **)&p);
    printf("graphics:");
    for (i = 0; i < 6; i++)
    {
        printf(" px[%u].r=%u", i, p[i] & 0xff);
        if ((p[i] & 0xff) != i + 1) failures++;
    }
    printf(" (want v + 1)\n");
    ID3D12Resource_Unmap(trb, 0, NULL);

    printf("%s\n", failures ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED");
    return failures ? 1 : 0;
}
