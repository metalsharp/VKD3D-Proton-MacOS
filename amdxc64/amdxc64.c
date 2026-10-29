/*
 * amdxc64.dll for VKD3D-Proton on MetalSharp Wine: the part of AMD's D3D12 driver that AMD FSR's DLLs look for.
 *
 * FSR's providers (amd_fidelityfx_upscaler_dx12.dll from the FSR SDK) offer FSR Upscaling 4 only in a process
 * where amdxc64.dll, the name of AMD's D3D12 driver, is loaded, and ask it for AMD's extension interfaces through
 * AmdExtD3DCreateInterface. VKD3D-Proton loads this DLL when the D3D12 device is created and FSR 4 is turned on
 * (METALSHARP_FSR4=1); it answers those requests by forwarding them to VKD3D-Proton's device where it implements
 * them (Anti-Lag), and declines the rest. With FSR 4 off, it is never loaded and FSR falls back to 3.1.
 *
 * Based on Proton's amdxc64 (Wine dlls/amdxc64/main.c, Copyright 2023 Etaash Mathamsetty, LGPL-2.1-or-later),
 * which has the interface identifiers and layouts used here.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <fcntl.h>
#include <io.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* AMD's extension interfaces (Proton's amdxc_interfaces.idl). */
DEFINE_GUID(IID_IAmdExtFfxApi, 0xb58d6601, 0x7401, 0x4234, 0x81, 0x80, 0x6f, 0xeb, 0xfc, 0x0e, 0x48, 0x4c);
DEFINE_GUID(IID_IAmdExtAntiLagApi, 0x44085fbe, 0xe839, 0x40c5, 0xbf, 0x38, 0x0e, 0xbc, 0x5a, 0xb4, 0xd0, 0xa6);
DEFINE_GUID(IID_IAmdExtD3DFactory, 0x014937ec, 0x9288, 0x446f, 0xa9, 0xac, 0xd7, 0x5a, 0x8e, 0x3a, 0x98, 0x4f);
DEFINE_GUID(IID_IAmdExtD3DShaderIntrinsics, 0xba019d53, 0xccab, 0x4cbd, 0xb5, 0x6a, 0x72, 0x30, 0xed, 0x43, 0x30, 0xad);
DEFINE_GUID(IID_IAmdExtD3DDevice, 0x8104c0fc, 0x7413, 0x410f, 0x8e, 0x83, 0xaa, 0x61, 0x7e, 0x90, 0x86, 0x48);
DEFINE_GUID(IID_IAmdExtD3DDevice1, 0x4bbcaf68, 0xeaf7, 0x4fa4, 0xb6, 0x53, 0xcb, 0x45, 0x8c, 0x33, 0x4a, 0x4e);
DEFINE_GUID(IID_IAmdExtD3DDevice2, 0xa7becf5d, 0x2930, 0x4fda, 0x8e, 0xee, 0xc7, 0x97, 0xd8, 0xa5, 0x2b, 0x7e);
DEFINE_GUID(IID_IAmdExtD3DDevice3, 0x397e3533, 0x111e, 0x4a9d, 0xa1, 0x71, 0x2b, 0xae, 0x8e, 0xf6, 0xcb, 0x24);
DEFINE_GUID(IID_IAmdExtD3DDevice4, 0xbe9a8c6a, 0x868e, 0x490d, 0x8f, 0xbf, 0x29, 0xda, 0xc2, 0x65, 0x0f, 0x3b);
DEFINE_GUID(IID_IAmdExtD3DDevice5, 0xbdc14598, 0xb7d2, 0x4a8d, 0x9c, 0xa5, 0x67, 0x84, 0x8e, 0x2a, 0xf7, 0x45);
DEFINE_GUID(IID_IAmdExtD3DDevice6, 0xf764a768, 0x48b4, 0x46a5, 0x97, 0x79, 0x92, 0x8e, 0xd6, 0x89, 0x6d, 0x2a);
DEFINE_GUID(IID_IAmdExtD3DDevice7, 0xfee37afc, 0x3c50, 0x4abf, 0x86, 0xcc, 0x16, 0x22, 0x34, 0x9b, 0x29, 0xc0);
DEFINE_GUID(IID_IAmdExtD3DDevice8, 0xf714e11a, 0xb54e, 0x4e0f, 0xab, 0xc5, 0xdf, 0x58, 0xb1, 0x81, 0x33, 0xd1);

/* METALSHARP_FSR4_LOG: every request and its answer, to see what an FSR DLL asks for; 1 writes them on stderr, any
 * other value is a file to append them to (created with the default security descriptor). */
static FILE *open_log(const char *path)
{
    HANDLE file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    int fd;

    if (file == INVALID_HANDLE_VALUE) return NULL;
    if ((fd = _open_osfhandle((intptr_t)file, _O_APPEND)) < 0) { CloseHandle(file); return NULL; }
    return _fdopen(fd, "a");
}

static void amdxc_log(const char *format, ...)
{
    static FILE *out;
    static int enabled = -1;
    va_list args;
    if (enabled < 0)
    {
        const char *env = getenv("METALSHARP_FSR4_LOG");
        enabled = env && *env && strcmp(env, "0");
        if (enabled) out = strcmp(env, "1") ? open_log(env) : stderr;
        if (!out) enabled = 0;
    }
    if (!enabled) return;
    va_start(args, format);
    fputs("amdxc64: ", out);
    vfprintf(out, format, args);
    fputc('\n', out);
    va_end(args);
    fflush(out);
}

static BOOL fsr4_enabled(void)
{
    const char *env = getenv("METALSHARP_FSR4");
    return env && !strcmp(env, "1");
}

static const char *guid_str(REFIID iid, char *buf)
{
    snprintf(buf, 40, "{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", (unsigned long)iid->Data1, iid->Data2, iid->Data3,
             iid->Data4[0], iid->Data4[1], iid->Data4[2], iid->Data4[3], iid->Data4[4], iid->Data4[5], iid->Data4[6], iid->Data4[7]);
    return buf;
}

/* IAmdExtFfxApi: lets AMD's driver hand FSR a newer provider than the DLLs the game ships. There is no AMD driver
 * here, so there is never a driver provider: FSR uses the providers in the game's FSR DLLs. */
typedef struct IAmdExtFfxApi IAmdExtFfxApi;
typedef struct IAmdExtFfxApiVtbl
{
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IAmdExtFfxApi *iface, REFIID iid, void **out);
    ULONG (STDMETHODCALLTYPE *AddRef)(IAmdExtFfxApi *iface);
    ULONG (STDMETHODCALLTYPE *Release)(IAmdExtFfxApi *iface);
    HRESULT (STDMETHODCALLTYPE *UpdateFfxApiProvider)(IAmdExtFfxApi *iface, void *data, unsigned int size);
} IAmdExtFfxApiVtbl;
struct IAmdExtFfxApi { const IAmdExtFfxApiVtbl *lpVtbl; };

struct ffx_api
{
    IAmdExtFfxApi IAmdExtFfxApi_iface;
    LONG ref;
};

static struct ffx_api *impl_from_IAmdExtFfxApi(IAmdExtFfxApi *iface)
{
    return CONTAINING_RECORD(iface, struct ffx_api, IAmdExtFfxApi_iface);
}

static HRESULT STDMETHODCALLTYPE ffx_api_QueryInterface(IAmdExtFfxApi *iface, REFIID iid, void **out)
{
    char buf[40];
    amdxc_log("IAmdExtFfxApi::QueryInterface %s: no", guid_str(iid, buf));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE ffx_api_AddRef(IAmdExtFfxApi *iface)
{
    return InterlockedIncrement(&impl_from_IAmdExtFfxApi(iface)->ref);
}

static ULONG STDMETHODCALLTYPE ffx_api_Release(IAmdExtFfxApi *iface)
{
    struct ffx_api *api = impl_from_IAmdExtFfxApi(iface);
    ULONG ref = InterlockedDecrement(&api->ref);
    if (!ref) free(api);
    return ref;
}

static HRESULT STDMETHODCALLTYPE ffx_api_UpdateFfxApiProvider(IAmdExtFfxApi *iface, void *data, unsigned int size)
{
    amdxc_log("IAmdExtFfxApi::UpdateFfxApiProvider %p, %u bytes: no driver provider", data, size);
    return E_NOINTERFACE;
}

static const IAmdExtFfxApiVtbl ffx_api_vtbl =
{
    ffx_api_QueryInterface,
    ffx_api_AddRef,
    ffx_api_Release,
    ffx_api_UpdateFfxApiProvider,
};

/* VKD3D-Proton's ID3D12DeviceExt3 (include/vkd3d_device_vkd3d_ext.idl), which says which of AMD's shader intrinsics
 * (AGS) its shader compiler implements on this device: WMMA_FP8 with Vulkan cooperative matrices, WMMA_FP8_NATIVE when
 * they also take FP8 (VK_EXT_shader_float8; MoltenVK has both on Apple GPUs). */
DEFINE_GUID(IID_ID3D12DeviceExt3, 0x3aefa75c, 0xc9f3, 0x4b7f, 0xa1, 0x80, 0x5d, 0xf6, 0x95, 0xd0, 0x83, 0xbf);
enum
{
    AGS_EXTENSION_INTRINSICS_16 = 0, AGS_EXTENSION_INTRINSICS_17 = 1, AGS_EXTENSION_INTRINSICS_19 = 5,
    AGS_EXTENSION_BASE_VERTEX = 6, AGS_EXTENSION_BASE_INSTANCE = 7, AGS_EXTENSION_GET_WAVE_SIZE = 8,
    AGS_EXTENSION_FLOAT_CONVERSION = 9, AGS_EXTENSION_READ_LANE_AT = 10, AGS_EXTENSION_RAY_HIT_TOKEN = 11,
    AGS_EXTENSION_SHADER_CLOCK = 12, AGS_EXTENSION_WMMA_FP8 = 100, AGS_EXTENSION_WMMA_FP8_NATIVE = 101,
};
typedef struct ID3D12DeviceExt3 ID3D12DeviceExt3;
typedef struct
{
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ID3D12DeviceExt3 *iface, REFIID iid, void **out);
    ULONG (STDMETHODCALLTYPE *AddRef)(ID3D12DeviceExt3 *iface);
    ULONG (STDMETHODCALLTYPE *Release)(ID3D12DeviceExt3 *iface);
    void *ext[7], *ext1[2], *ext2[4];   /* ID3D12DeviceExt, ID3D12DeviceExt1, ID3D12DeviceExt2 */
    BOOL (STDMETHODCALLTYPE *SupportsAGSExtension)(ID3D12DeviceExt3 *iface, UINT extension);
    HRESULT (STDMETHODCALLTYPE *SetAGSUAVSlot)(ID3D12DeviceExt3 *iface, UINT uav_slot);
} ID3D12DeviceExt3Vtbl;
struct ID3D12DeviceExt3 { const ID3D12DeviceExt3Vtbl *lpVtbl; };

/* The application's D3D12 device, which the factory's interfaces answer for (the last one it was created with). */
static IUnknown *d3d12_device;

/* METALSHARP_FSR4_GPU=rdna3: Wine answers as an RDNA 3 GPU, and FSR should run its INT8 model, so the wave matrix
 * intrinsics (and with them the FP8 models) are not offered. */
static BOOL wave_matrix_allowed(void)
{
    const char *gpu = getenv("METALSHARP_FSR4_GPU");
    return !gpu || strcmp(gpu, "rdna3");
}

static BOOL device_supports_ags(UINT extension)
{
    ID3D12DeviceExt3 *ext;
    BOOL supported;

    if ((extension == AGS_EXTENSION_WMMA_FP8 || extension == AGS_EXTENSION_WMMA_FP8_NATIVE) && !wave_matrix_allowed())
        return FALSE;
    if (!d3d12_device || FAILED(IUnknown_QueryInterface(d3d12_device, &IID_ID3D12DeviceExt3, (void **)&ext)))
        return FALSE;
    supported = ext->lpVtbl->SupportsAGSExtension(ext, extension);
    ext->lpVtbl->Release(ext);
    return supported;
}

/* IAmdExtD3DDevice8 (AMD's driver extension headers): FSR asks it which wave matrix (WMMA) shapes and types the
 * GPU multiplies: 16x16x16 FP8 into FP32 where VKD3D-Proton runs them natively, which selects FSR's FP8 models
 * (FSR Upscaling 4's RDNA 4 model, FSR Frame Generation 4). Its pipeline creation entry points take AMD shader
 * binaries and are not supported. */
typedef struct
{
    SIZE_T mSize, nSize, kSize;
    ULONG aType, bType, cType, resultType;
    BOOL saturatingAccumulation;
} AmdExtWaveMatrixProperties;

typedef struct IAmdExtD3DDevice8 IAmdExtD3DDevice8;
typedef struct IAmdExtD3DDevice8Vtbl
{
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IAmdExtD3DDevice8 *iface, REFIID iid, void **out);
    ULONG (STDMETHODCALLTYPE *AddRef)(IAmdExtD3DDevice8 *iface);
    ULONG (STDMETHODCALLTYPE *Release)(IAmdExtD3DDevice8 *iface);
    /* IAmdExtD3DDevice */
    HRESULT (STDMETHODCALLTYPE *CreateGraphicsPipelineState)(IAmdExtD3DDevice8 *iface, const void *info,
            const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID iid, void **pso);
    /* IAmdExtD3DDevice1 */
    void (STDMETHODCALLTYPE *PushMarker)(IAmdExtD3DDevice8 *iface, ID3D12GraphicsCommandList *list, const char *marker);
    void (STDMETHODCALLTYPE *PopMarker)(IAmdExtD3DDevice8 *iface, ID3D12GraphicsCommandList *list);
    void (STDMETHODCALLTYPE *SetMarker)(IAmdExtD3DDevice8 *iface, ID3D12GraphicsCommandList *list, const char *marker);
    /* IAmdExtD3DDevice2 */
    HRESULT (STDMETHODCALLTYPE *CheckExtFeatureSupport)(IAmdExtD3DDevice8 *iface, UINT type, void *data, SIZE_T size);
    HRESULT (STDMETHODCALLTYPE *CreateComputePipelineState)(IAmdExtD3DDevice8 *iface, const void *info,
            const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, REFIID iid, void **pso);
    /* IAmdExtD3DDevice3 */
    HRESULT (STDMETHODCALLTYPE *CreatePipelineState)(IAmdExtD3DDevice8 *iface, const void *info,
            const D3D12_PIPELINE_STATE_STREAM_DESC *desc, REFIID iid, void **pso);
    /* IAmdExtD3DDevice4 */
    void (STDMETHODCALLTYPE *SetPrimitiveTopology)(IAmdExtD3DDevice8 *iface, ID3D12GraphicsCommandList *list, UINT topology);
    /* IAmdExtD3DDevice5 */
    HRESULT (STDMETHODCALLTYPE *CreateComputePipelineFromElf)(IAmdExtD3DDevice8 *iface, void *info, REFIID iid, void **pso);
    void (STDMETHODCALLTYPE *SetKernelArguments)(IAmdExtD3DDevice8 *iface, ID3D12GraphicsCommandList *list, ULONG first,
            ULONG count, const void *values);
    /* IAmdExtD3DDevice6 */
    void (STDMETHODCALLTYPE *GetGpuRtInterfaceVersion)(IAmdExtD3DDevice8 *iface, void *version);
    void (STDMETHODCALLTYPE *GetGpuRtBinaryVersion)(IAmdExtD3DDevice8 *iface, void *version);
    /* IAmdExtD3DDevice7 */
    HRESULT (STDMETHODCALLTYPE *CreateComputePipelineCrossCompile)(IAmdExtD3DDevice8 *iface, const void *info, REFIID iid, void **pso);
    /* IAmdExtD3DDevice8 */
    HRESULT (STDMETHODCALLTYPE *GetWaveMatrixProperties)(IAmdExtD3DDevice8 *iface, SIZE_T *count, AmdExtWaveMatrixProperties *properties);
} IAmdExtD3DDevice8Vtbl;
struct IAmdExtD3DDevice8 { const IAmdExtD3DDevice8Vtbl *lpVtbl; };

static BOOL is_ext_device_iid(REFIID iid)
{
    return IsEqualGUID(iid, &IID_IAmdExtD3DDevice) || IsEqualGUID(iid, &IID_IAmdExtD3DDevice1) ||
           IsEqualGUID(iid, &IID_IAmdExtD3DDevice2) || IsEqualGUID(iid, &IID_IAmdExtD3DDevice3) ||
           IsEqualGUID(iid, &IID_IAmdExtD3DDevice4) || IsEqualGUID(iid, &IID_IAmdExtD3DDevice5) ||
           IsEqualGUID(iid, &IID_IAmdExtD3DDevice6) || IsEqualGUID(iid, &IID_IAmdExtD3DDevice7) ||
           IsEqualGUID(iid, &IID_IAmdExtD3DDevice8);
}

static IAmdExtD3DDevice8 ext_device;

static HRESULT STDMETHODCALLTYPE ext_device_QueryInterface(IAmdExtD3DDevice8 *iface, REFIID iid, void **out)
{
    char buf[40];
    if (IsEqualGUID(iid, &IID_IUnknown) || is_ext_device_iid(iid))
    {
        *out = &ext_device;
        return S_OK;
    }
    amdxc_log("IAmdExtD3DDevice8::QueryInterface %s: no", guid_str(iid, buf));
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE ext_device_AddRef(IAmdExtD3DDevice8 *iface) { return 2; }
static ULONG STDMETHODCALLTYPE ext_device_Release(IAmdExtD3DDevice8 *iface) { return 1; }

static HRESULT STDMETHODCALLTYPE ext_device_CreateGraphicsPipelineState(IAmdExtD3DDevice8 *iface, const void *info,
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID iid, void **pso)
{
    amdxc_log("IAmdExtD3DDevice::CreateGraphicsPipelineState: not supported");
    return E_NOTIMPL;
}
static void STDMETHODCALLTYPE ext_device_PushMarker(IAmdExtD3DDevice8 *iface, ID3D12GraphicsCommandList *list, const char *marker) {}
static void STDMETHODCALLTYPE ext_device_PopMarker(IAmdExtD3DDevice8 *iface, ID3D12GraphicsCommandList *list) {}
static void STDMETHODCALLTYPE ext_device_SetMarker(IAmdExtD3DDevice8 *iface, ID3D12GraphicsCommandList *list, const char *marker) {}
static HRESULT STDMETHODCALLTYPE ext_device_CheckExtFeatureSupport(IAmdExtD3DDevice8 *iface, UINT type, void *data, SIZE_T size)
{
    amdxc_log("IAmdExtD3DDevice2::CheckExtFeatureSupport %u (%lu bytes): not supported", type, (unsigned long)size);
    return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE ext_device_CreateComputePipelineState(IAmdExtD3DDevice8 *iface, const void *info,
        const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, REFIID iid, void **pso)
{
    amdxc_log("IAmdExtD3DDevice2::CreateComputePipelineState: not supported");
    return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE ext_device_CreatePipelineState(IAmdExtD3DDevice8 *iface, const void *info,
        const D3D12_PIPELINE_STATE_STREAM_DESC *desc, REFIID iid, void **pso)
{
    amdxc_log("IAmdExtD3DDevice3::CreatePipelineState: not supported");
    return E_NOTIMPL;
}
static void STDMETHODCALLTYPE ext_device_SetPrimitiveTopology(IAmdExtD3DDevice8 *iface, ID3D12GraphicsCommandList *list, UINT topology)
{
    amdxc_log("IAmdExtD3DDevice4::SetPrimitiveTopology %u: ignored", topology);
}
static HRESULT STDMETHODCALLTYPE ext_device_CreateComputePipelineFromElf(IAmdExtD3DDevice8 *iface, void *info, REFIID iid, void **pso)
{
    amdxc_log("IAmdExtD3DDevice5::CreateComputePipelineFromElf: not supported");
    return E_NOTIMPL;
}
static void STDMETHODCALLTYPE ext_device_SetKernelArguments(IAmdExtD3DDevice8 *iface, ID3D12GraphicsCommandList *list,
        ULONG first, ULONG count, const void *values)
{
    amdxc_log("IAmdExtD3DDevice5::SetKernelArguments: ignored");
}
static void STDMETHODCALLTYPE ext_device_GetGpuRtInterfaceVersion(IAmdExtD3DDevice8 *iface, void *version)
{
    if (version) memset(version, 0, 4);
}
static void STDMETHODCALLTYPE ext_device_GetGpuRtBinaryVersion(IAmdExtD3DDevice8 *iface, void *version)
{
    if (version) memset(version, 0, 4);
}
static HRESULT STDMETHODCALLTYPE ext_device_CreateComputePipelineCrossCompile(IAmdExtD3DDevice8 *iface, const void *info,
        REFIID iid, void **pso)
{
    amdxc_log("IAmdExtD3DDevice7::CreateComputePipelineCrossCompile: not supported");
    return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE ext_device_GetWaveMatrixProperties(IAmdExtD3DDevice8 *iface, SIZE_T *count,
        AmdExtWaveMatrixProperties *properties)
{
    static const AmdExtWaveMatrixProperties fp8 = {16, 16, 16, 11 /* FP8 */, 11, 1 /* FP32 */, 1 /* FP32 */, FALSE};

    if (!count) return E_INVALIDARG;
    if (device_supports_ags(AGS_EXTENSION_WMMA_FP8_NATIVE))
    {
        amdxc_log("IAmdExtD3DDevice8::GetWaveMatrixProperties: 16x16x16 FP8");
        if (properties && *count >= 1) properties[0] = fp8;
        *count = 1;
        return S_OK;
    }
    amdxc_log("IAmdExtD3DDevice8::GetWaveMatrixProperties: none");
    *count = 0;
    return S_OK;
}

static const IAmdExtD3DDevice8Vtbl ext_device_vtbl =
{
    ext_device_QueryInterface, ext_device_AddRef, ext_device_Release,
    ext_device_CreateGraphicsPipelineState,
    ext_device_PushMarker, ext_device_PopMarker, ext_device_SetMarker,
    ext_device_CheckExtFeatureSupport, ext_device_CreateComputePipelineState,
    ext_device_CreatePipelineState,
    ext_device_SetPrimitiveTopology,
    ext_device_CreateComputePipelineFromElf, ext_device_SetKernelArguments,
    ext_device_GetGpuRtInterfaceVersion, ext_device_GetGpuRtBinaryVersion,
    ext_device_CreateComputePipelineCrossCompile,
    ext_device_GetWaveMatrixProperties,
};
static IAmdExtD3DDevice8 ext_device = { &ext_device_vtbl };

/* IAmdExtD3DFactory: hands out the driver's other extension interfaces for a device. */
typedef struct IAmdExtD3DFactory IAmdExtD3DFactory;
typedef struct IAmdExtD3DFactoryVtbl
{
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IAmdExtD3DFactory *iface, REFIID iid, void **out);
    ULONG (STDMETHODCALLTYPE *AddRef)(IAmdExtD3DFactory *iface);
    ULONG (STDMETHODCALLTYPE *Release)(IAmdExtD3DFactory *iface);
    HRESULT (STDMETHODCALLTYPE *CreateInterface)(IAmdExtD3DFactory *iface, IUnknown *outer, REFIID iid, void **out);
} IAmdExtD3DFactoryVtbl;
struct IAmdExtD3DFactory { const IAmdExtD3DFactoryVtbl *lpVtbl; };

/* IAmdExtD3DShaderIntrinsics: which of AMD's shader intrinsics (called through a UAV in register space 0x7fff0ade)
 * shaders may use. VKD3D-Proton's shader compiler implements them; it says which (ID3D12DeviceExt3). */
enum
{
    INTRINSIC_READFIRSTLANE = 0x1, INTRINSIC_BARYCENTRICS = 0x8, INTRINSIC_WAVE_REDUCE = 0x9, INTRINSIC_WAVE_SCAN = 0xa,
    INTRINSIC_DRAW_INDEX = 0xe, INTRINSIC_ATOMIC_U64 = 0xf, INTRINSIC_BASE_INSTANCE = 0x10,
    INTRINSIC_BASE_VERTEX = 0x11, INTRINSIC_FLOAT_CONVERSION = 0x12, INTRINSIC_GET_WAVE_SIZE = 0x13,
    INTRINSIC_READLANE_AT = 0x14, INTRINSIC_RAY_TRACE_HIT_TOKEN = 0x15, INTRINSIC_SHADER_CLOCK = 0x16,
    INTRINSIC_SHADER_REALTIME_CLOCK = 0x17, INTRINSIC_WAVE_MATRIX = 0x1f, INTRINSIC_FLOAT8_CONVERSION = 0x20,
};
typedef struct IAmdExtD3DShaderIntrinsics IAmdExtD3DShaderIntrinsics;
typedef struct
{
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IAmdExtD3DShaderIntrinsics *iface, REFIID iid, void **out);
    ULONG (STDMETHODCALLTYPE *AddRef)(IAmdExtD3DShaderIntrinsics *iface);
    ULONG (STDMETHODCALLTYPE *Release)(IAmdExtD3DShaderIntrinsics *iface);
    HRESULT (STDMETHODCALLTYPE *GetInfo)(IAmdExtD3DShaderIntrinsics *iface, void *info);
    HRESULT (STDMETHODCALLTYPE *CheckSupport)(IAmdExtD3DShaderIntrinsics *iface, UINT intrinsic);
    HRESULT (STDMETHODCALLTYPE *Enable)(IAmdExtD3DShaderIntrinsics *iface);
} IAmdExtD3DShaderIntrinsicsVtbl;
struct IAmdExtD3DShaderIntrinsics { const IAmdExtD3DShaderIntrinsicsVtbl *lpVtbl; };

static HRESULT STDMETHODCALLTYPE intrinsics_QueryInterface(IAmdExtD3DShaderIntrinsics *iface, REFIID iid, void **out);
static ULONG STDMETHODCALLTYPE intrinsics_AddRef(IAmdExtD3DShaderIntrinsics *iface) { return 2; }
static ULONG STDMETHODCALLTYPE intrinsics_Release(IAmdExtD3DShaderIntrinsics *iface) { return 1; }
static HRESULT STDMETHODCALLTYPE intrinsics_GetInfo(IAmdExtD3DShaderIntrinsics *iface, void *info)
{
    amdxc_log("IAmdExtD3DShaderIntrinsics::GetInfo");
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE intrinsics_CheckSupport(IAmdExtD3DShaderIntrinsics *iface, UINT intrinsic)
{
    BOOL supported;

    if (intrinsic == INTRINSIC_WAVE_MATRIX)
        supported = device_supports_ags(AGS_EXTENSION_WMMA_FP8);
    else if (intrinsic == INTRINSIC_FLOAT8_CONVERSION)
        supported = device_supports_ags(AGS_EXTENSION_WMMA_FP8_NATIVE);
    else if (intrinsic >= INTRINSIC_READFIRSTLANE && intrinsic <= INTRINSIC_BARYCENTRICS)
        supported = device_supports_ags(AGS_EXTENSION_INTRINSICS_16);
    else if (intrinsic == INTRINSIC_WAVE_REDUCE || intrinsic == INTRINSIC_WAVE_SCAN)
        supported = device_supports_ags(AGS_EXTENSION_INTRINSICS_17);
    else if (intrinsic == INTRINSIC_DRAW_INDEX || intrinsic == INTRINSIC_ATOMIC_U64)
        supported = device_supports_ags(AGS_EXTENSION_INTRINSICS_19);
    else if (intrinsic == INTRINSIC_SHADER_CLOCK || intrinsic == INTRINSIC_SHADER_REALTIME_CLOCK)
        supported = device_supports_ags(AGS_EXTENSION_SHADER_CLOCK);
    else if (intrinsic == INTRINSIC_BASE_INSTANCE)
        supported = device_supports_ags(AGS_EXTENSION_BASE_INSTANCE);
    else if (intrinsic == INTRINSIC_BASE_VERTEX)
        supported = device_supports_ags(AGS_EXTENSION_BASE_VERTEX);
    else if (intrinsic == INTRINSIC_GET_WAVE_SIZE)
        supported = device_supports_ags(AGS_EXTENSION_GET_WAVE_SIZE);
    else if (intrinsic == INTRINSIC_FLOAT_CONVERSION)
        supported = device_supports_ags(AGS_EXTENSION_FLOAT_CONVERSION);
    else if (intrinsic == INTRINSIC_READLANE_AT)
        supported = device_supports_ags(AGS_EXTENSION_READ_LANE_AT);
    else if (intrinsic == INTRINSIC_RAY_TRACE_HIT_TOKEN)
        supported = device_supports_ags(AGS_EXTENSION_RAY_HIT_TOKEN);
    else
        supported = FALSE;
    amdxc_log("IAmdExtD3DShaderIntrinsics::CheckSupport %#x: %s", intrinsic, supported ? "yes" : "no");
    return supported ? S_OK : E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE intrinsics_Enable(IAmdExtD3DShaderIntrinsics *iface)
{
    /* VKD3D-Proton compiles them whenever a shader uses them. */
    amdxc_log("IAmdExtD3DShaderIntrinsics::Enable");
    return S_OK;
}
static const IAmdExtD3DShaderIntrinsicsVtbl intrinsics_vtbl =
{
    intrinsics_QueryInterface, intrinsics_AddRef, intrinsics_Release,
    intrinsics_GetInfo, intrinsics_CheckSupport, intrinsics_Enable,
};
static IAmdExtD3DShaderIntrinsics intrinsics = { &intrinsics_vtbl };
static HRESULT STDMETHODCALLTYPE intrinsics_QueryInterface(IAmdExtD3DShaderIntrinsics *iface, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IAmdExtD3DShaderIntrinsics))
    {
        *out = &intrinsics;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static HRESULT STDMETHODCALLTYPE factory_CreateInterface(IAmdExtD3DFactory *iface, IUnknown *outer, REFIID iid, void **out)
{
    char buf[40];
    HRESULT hr = E_NOINTERFACE;

    *out = NULL;
    if (outer)
        d3d12_device = outer;
    if (is_ext_device_iid(iid))
    {
        *out = &ext_device;
        hr = S_OK;
    }
    else if (IsEqualGUID(iid, &IID_IAmdExtD3DShaderIntrinsics))
    {
        *out = &intrinsics;
        hr = S_OK;
    }
    else if (IsEqualGUID(iid, &IID_IAmdExtAntiLagApi) && outer)
        hr = IUnknown_QueryInterface(outer, iid, out);
    amdxc_log("IAmdExtD3DFactory::CreateInterface %s: %#lx", guid_str(iid, buf), (long)hr);
    return hr;
}
static HRESULT STDMETHODCALLTYPE factory_QueryInterface(IAmdExtD3DFactory *iface, REFIID iid, void **out)
{
    return factory_CreateInterface(iface, NULL, iid, out);
}
static ULONG STDMETHODCALLTYPE factory_AddRef(IAmdExtD3DFactory *iface) { return 2; }
static ULONG STDMETHODCALLTYPE factory_Release(IAmdExtD3DFactory *iface) { return 1; }

static const IAmdExtD3DFactoryVtbl factory_vtbl = { factory_QueryInterface, factory_AddRef, factory_Release, factory_CreateInterface };
static IAmdExtD3DFactory factory = { &factory_vtbl };

/* The driver's entry point: outer is the application's D3D12 device (VKD3D-Proton's). */
HRESULT __cdecl AmdExtD3DCreateInterface(IUnknown *outer, REFIID iid, void **out)
{
    char buf[40];
    HRESULT hr;

    if (!out) return E_POINTER;
    *out = NULL;
    /* Inert unless FSR 4 is turned on, in case something other than VKD3D-Proton loads this DLL. */
    if (!fsr4_enabled())
    {
        amdxc_log("AmdExtD3DCreateInterface %s: METALSHARP_FSR4 is not 1", guid_str(iid, buf));
        return E_NOINTERFACE;
    }
    if (outer)
        d3d12_device = outer;
    if (IsEqualGUID(iid, &IID_IAmdExtFfxApi))
    {
        struct ffx_api *api = calloc(1, sizeof(*api));
        if (!api) return E_OUTOFMEMORY;
        api->IAmdExtFfxApi_iface.lpVtbl = &ffx_api_vtbl;
        api->ref = 1;
        *out = &api->IAmdExtFfxApi_iface;
        hr = S_OK;
    }
    else if (IsEqualGUID(iid, &IID_IAmdExtAntiLagApi))
        hr = outer ? IUnknown_QueryInterface(outer, iid, out) : E_NOINTERFACE;
    else if (IsEqualGUID(iid, &IID_IAmdExtD3DFactory))
    {
        *out = &factory;
        hr = S_OK;
    }
    else
        hr = E_NOINTERFACE;
    amdxc_log("AmdExtD3DCreateInterface %p %s: %#lx", outer, guid_str(iid, buf), (long)hr);
    return hr;
}

HMODULE WINAPI AmdGetDxcModuleHandle(void)
{
    amdxc_log("AmdGetDxcModuleHandle");
    return GetModuleHandleA(NULL);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);
        amdxc_log("loaded");
    }
    return TRUE;
}
