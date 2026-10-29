/* hlsl/wave16b.hlsl (16-bit values in groupshared memory, raw buffers and a half-float typed UAV, wave operations
 * over several waves per group) on the D3D12 device; writes the results to the file named on the command line.
 * Compare a run with the bridge against one with METALSHARP_MSC=0 (SPIR-V): the files must be identical. */
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#define INITGUID
#include <windows.h>
#include <d3d12.h>
#include <stdio.h>
#include <string.h>
#include "wave16b.h"

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("FAIL %s = %#lx (line %d)\n", #x, (long)hr_, __LINE__); return 1; } } while (0)
#define THREADS 128
#define SLOTS 12
#define RAW_SIZE 2048

static ID3D12Device *dev;

static ID3D12Resource *make_buffer(UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp = { type };
    D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, size, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 },
            D3D12_TEXTURE_LAYOUT_ROW_MAJOR, flags };
    ID3D12Resource *r = NULL;
    ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, state, NULL, &IID_ID3D12Resource, (void **)&r);
    return r;
}

static unsigned short to_half(float f)
{
    union { float f; unsigned int u; } v = { f };
    unsigned int sign = (v.u >> 16) & 0x8000, exp = (v.u >> 23) & 0xff, man = v.u & 0x7fffff;
    if (!exp) return sign;
    return sign | ((exp - 112) << 10) | (man >> 13);   /* the test's values are exact, normal halves */
}

static void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
}

int main(int argc, char **argv)
{
    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 2, 0, 0 };
    D3D12_ROOT_PARAMETER params[3] = {{ D3D12_ROOT_PARAMETER_TYPE_UAV }, { D3D12_ROOT_PARAMETER_TYPE_UAV },
            { D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE }};
    D3D12_ROOT_SIGNATURE_DESC rs_desc = { 3, params };
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc = {0};
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    D3D12_HEAP_PROPERTIES default_heap = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC tex_desc = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 16, 8, 1, 1,
            DXGI_FORMAT_R16G16B16A16_FLOAT, { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS };
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = { DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_UAV_DIMENSION_TEXTURE2D };
    D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
    ID3D12Resource *out, *raw, *tex, *upload, *readback;
    ID3D12GraphicsCommandList *list;
    ID3D12CommandAllocator *alloc;
    ID3D12DescriptorHeap *heap;
    ID3D12RootSignature *rs;
    ID3D12PipelineState *pso;
    ID3D12CommandQueue *queue;
    ID3D12Fence *fence;
    ID3DBlob *blob;
    unsigned short *halves;
    unsigned char *p;
    unsigned int i;
    FILE *f;

    if (argc < 2) { printf("usage: wave16btest <output file>\n"); return 2; }
    range.BaseShaderRegister = 2;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &range;
    params[1].Descriptor.ShaderRegister = 1;

    CHECK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_12_0, &IID_ID3D12Device, (void **)&dev));
    CHECK(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue));
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc));
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list));
    CHECK(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence));
    CHECK(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, NULL));
    CHECK(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
            &IID_ID3D12RootSignature, (void **)&rs));
    pso_desc.pRootSignature = rs;
    pso_desc.CS.pShaderBytecode = wave16b_dxil;
    pso_desc.CS.BytecodeLength = sizeof(wave16b_dxil);
    CHECK(ID3D12Device_CreateComputePipelineState(dev, &pso_desc, &IID_ID3D12PipelineState, (void **)&pso));
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &heap_desc, &IID_ID3D12DescriptorHeap, (void **)&heap));

    out = make_buffer(THREADS * SLOTS * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    raw = make_buffer(RAW_SIZE, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    CHECK(ID3D12Device_CreateCommittedResource(dev, &default_heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&tex));
    upload = make_buffer(65536, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    readback = make_buffer(THREADS * SLOTS * 4 + RAW_SIZE, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Device_CreateUnorderedAccessView(dev, tex, NULL, &uav,
            ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap));

    /* raw buffer: halves i * 0.125 (wrapping at 64); texture: texel (x, y) = (x / 4, y / 2, x + y, 1) */
    CHECK(ID3D12Resource_Map(upload, 0, NULL, (void **)&p));
    halves = (unsigned short *)p;
    for (i = 0; i < RAW_SIZE / 2; i++) halves[i] = to_half((float)(i & 63) * 0.125f);
    for (i = 0; i < 16 * 8; i++)
    {
        unsigned short *t = (unsigned short *)(p + 4096 + (i >> 4) * 256 + (i & 15) * 8);
        t[0] = to_half((float)(i & 15) / 4.0f); t[1] = to_half((float)(i >> 4) / 2.0f);
        t[2] = to_half((float)((i & 15) + (i >> 4))); t[3] = to_half(1.0f);
    }
    ID3D12GraphicsCommandList_CopyBufferRegion(list, raw, 0, upload, 0, RAW_SIZE);
    dst.pResource = tex; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = upload; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = 4096;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    src.PlacedFootprint.Footprint.Width = 16; src.PlacedFootprint.Footprint.Height = 8;
    src.PlacedFootprint.Footprint.Depth = 1; src.PlacedFootprint.Footprint.RowPitch = 256;
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
    transition(list, raw, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(list, tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &heap);
    ID3D12GraphicsCommandList_SetComputeRootSignature(list, rs);
    ID3D12GraphicsCommandList_SetPipelineState(list, pso);
    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(list, 0, ID3D12Resource_GetGPUVirtualAddress(out));
    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(list, 1, ID3D12Resource_GetGPUVirtualAddress(raw));
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, 2,
            ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(heap));
    ID3D12GraphicsCommandList_Dispatch(list, 1, 1, 1);
    transition(list, out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(list, raw, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, readback, 0, out, 0, THREADS * SLOTS * 4);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, readback, THREADS * SLOTS * 4, raw, 0, RAW_SIZE);
    ID3D12GraphicsCommandList_Close(list);
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&list);
    ID3D12CommandQueue_Signal(queue, fence, 1);
    while (ID3D12Fence_GetCompletedValue(fence) < 1)
        Sleep(1);
    CHECK(ID3D12Resource_Map(readback, 0, NULL, (void **)&p));
    if (!(f = fopen(argv[1], "wb"))) { printf("FAIL can't write %s\n", argv[1]); return 1; }
    fwrite(p, 1, THREADS * SLOTS * 4 + RAW_SIZE, f);
    fclose(f);
    printf("wrote %s\n", argv[1]);
    return 0;
}
