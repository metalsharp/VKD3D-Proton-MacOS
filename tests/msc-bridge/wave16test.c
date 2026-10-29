/* Wave operations on native 16-bit values in a compute shader (hlsl/wave16.hlsl), checked against the CPU. Run it
 * with the bridge (default) and with METALSHARP_MSC=0 (SPIR-V): both must pass. */
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#define INITGUID
#include <windows.h>
#include <d3d12.h>
#include <stdio.h>
#include <string.h>
#include "wave16.h"

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("FAIL %s = %#lx (line %d)\n", #x, (long)hr_, __LINE__); return 1; } } while (0)
#define LANES 32
#define SLOTS 24

static ID3D12Resource *make_buffer(ID3D12Device *dev, UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_FLAGS flags,
        D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp = { type };
    D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, size, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 },
            D3D12_TEXTURE_LAYOUT_ROW_MAJOR, flags };
    ID3D12Resource *r = NULL;
    ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, state, NULL, &IID_ID3D12Resource, (void **)&r);
    return r;
}

static float lane_h(unsigned int l) { return (float)(l & 15) * 0.25f; }

int main(void)
{
    static const char *names[SLOTS] = { "WaveGetLaneCount", "WaveActiveSum(half)", "WaveActiveMax(half)",
            "WavePrefixSum(half)", "WaveReadLaneAt(half)", "QuadReadAcrossX(half)", "WaveReadLaneFirst(half)",
            "WaveActiveSum(int16)", "WaveActiveMin(int16)", "WavePrefixSum(int16)", "WaveReadLaneAt(int16)",
            "QuadReadAcrossDiagonal(int16)", "WaveActiveMin(uint16)", "WaveReadLaneAt(uint16)",
            "WaveActiveMax(uint16)", "WaveActiveSum(half4).x", "WaveActiveSum(half4).y", "WaveActiveSum(half4).z",
            "WaveActiveSum(half4).w", "WaveReadLaneAt(half4).y", "WaveActiveMin(half)",
            "WaveActiveAllEqual(half)", "WaveActiveCountBits(half > 1)", "QuadReadLaneAt(half, 2)" };
    D3D12_ROOT_PARAMETER param = { D3D12_ROOT_PARAMETER_TYPE_UAV };
    D3D12_ROOT_SIGNATURE_DESC rs_desc = { 1, &param };
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc = {0};
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    ID3D12Resource *out, *readback;
    ID3D12GraphicsCommandList *list;
    ID3D12CommandAllocator *alloc;
    ID3D12RootSignature *rs;
    ID3D12PipelineState *pso;
    ID3D12CommandQueue *queue;
    ID3D12Device *dev;
    ID3D12Fence *fence;
    ID3DBlob *blob;
    int failures = 0;
    unsigned int l, k;
    const UINT size = LANES * SLOTS * 4;
    union { float f; int i; unsigned int u; } *data;

    CHECK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_12_0, &IID_ID3D12Device, (void **)&dev));
    CHECK(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue));
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc));
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list));
    CHECK(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence));
    CHECK(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, NULL));
    CHECK(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
            &IID_ID3D12RootSignature, (void **)&rs));
    pso_desc.pRootSignature = rs;
    pso_desc.CS.pShaderBytecode = wave16_dxil;
    pso_desc.CS.BytecodeLength = sizeof(wave16_dxil);
    CHECK(ID3D12Device_CreateComputePipelineState(dev, &pso_desc, &IID_ID3D12PipelineState, (void **)&pso));

    out = make_buffer(dev, size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    readback = make_buffer(dev, size, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12GraphicsCommandList_SetComputeRootSignature(list, rs);
    ID3D12GraphicsCommandList_SetPipelineState(list, pso);
    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(list, 0, ID3D12Resource_GetGPUVirtualAddress(out));
    ID3D12GraphicsCommandList_Dispatch(list, 1, 1, 1);
    {
        D3D12_RESOURCE_BARRIER b = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
        b.Transition.pResource = out;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
    }
    ID3D12GraphicsCommandList_CopyResource(list, readback, out);
    ID3D12GraphicsCommandList_Close(list);
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&list);
    ID3D12CommandQueue_Signal(queue, fence, 1);
    while (ID3D12Fence_GetCompletedValue(fence) < 1)
        Sleep(1);
    CHECK(ID3D12Resource_Map(readback, 0, NULL, (void **)&data));

    if (data[0].i != LANES)
    {
        printf("FAIL wave size %d, the test expects %d lanes\n", data[0].i, LANES);
        return 1;
    }
    for (k = 1; k < SLOTS; k++)
    {
        int bad = 0;
        for (l = 0; l < LANES; l++)
        {
            float hs = 0, hmax = 0, prefix = 0;
            int ss = 0, smin = 32767, sprefix = 0, cnt = 0;
            unsigned int umax = 0, i;
            float want = 0;
            int wanti = 0, is_int = 0;

            for (i = 0; i < LANES; i++)
            {
                int s = (int)i * 3 - 40;
                unsigned int u = (i * 1000 + 7) & 0xffff;
                hs += lane_h(i); if (lane_h(i) > hmax) hmax = lane_h(i);
                if (i < l) { prefix += lane_h(i); sprefix += s; }
                ss += s; if (s < smin) smin = s;
                if (u > umax) umax = u;
                cnt += lane_h(i) > 1.0f;
            }
            switch (k)
            {
                case 1: want = hs; break;
                case 2: want = hmax; break;
                case 3: want = prefix; break;
                case 4: want = lane_h(5); break;
                case 5: want = lane_h(l ^ 1); break;
                case 6: want = lane_h(0) + 1.0f; break;
                case 7: is_int = 1; wanti = ss; break;
                case 8: is_int = 1; wanti = smin; break;
                case 9: is_int = 1; wanti = sprefix; break;
                case 10: is_int = 1; wanti = 7 * 3 - 40; break;
                case 11: is_int = 1; wanti = (int)(l ^ 3) * 3 - 40; break;
                case 12: is_int = 1; wanti = 7; break;
                case 13: is_int = 1; wanti = 30007; break;
                case 14: is_int = 1; wanti = (int)umax; break;
                case 15: want = hs; break;
                case 16: want = 2 * hs; break;
                case 17: want = -hs; break;
                case 18: want = LANES; break;
                case 19: want = 2 * lane_h(9); break;
                case 20: want = 0.0f; break;
                case 21: is_int = 1; wanti = 0; break;
                case 22: is_int = 1; wanti = cnt; break;
                case 23: want = lane_h((l & ~3u) | 2); break;
            }
            if (is_int ? data[l * SLOTS + k].i != wanti : data[l * SLOTS + k].f != want)
            {
                if (!bad++)
                {
                    if (is_int)
                        printf("FAIL %s: lane %u got %d, want %d\n", names[k], l, data[l * SLOTS + k].i, wanti);
                    else
                        printf("FAIL %s: lane %u got %g, want %g\n", names[k], l, data[l * SLOTS + k].f, want);
                }
            }
        }
        if (!bad)
            printf("ok   %s\n", names[k]);
        failures += !!bad;
    }
    printf("%d of %d checks failed\n", failures, SLOTS - 1);
    return failures ? 1 : 0;
}
