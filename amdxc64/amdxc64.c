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

/* METALSHARP_FSR4_LOG=1: every request and its answer on stderr, to see what an FSR DLL asks for. */
static void amdxc_log(const char *format, ...)
{
    static int enabled = -1;
    va_list args;
    if (enabled < 0)
    {
        const char *env = getenv("METALSHARP_FSR4_LOG");
        enabled = env && *env == '1';
    }
    if (!enabled) return;
    va_start(args, format);
    fputs("amdxc64: ", stderr);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
    fflush(stderr);
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

/* IAmdExtD3DDevice8 (AMD's driver extension headers): FSR asks it which wave matrix (WMMA) shapes and types the
 * GPU multiplies. Apple GPUs have none that FSR's matrix models use, so there are none, and FSR runs a model that
 * doesn't use them. Its pipeline creation entry points take AMD shader binaries and are not supported. */
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
    amdxc_log("IAmdExtD3DDevice8::GetWaveMatrixProperties: none");
    if (!count) return E_INVALIDARG;
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

static HRESULT STDMETHODCALLTYPE factory_CreateInterface(IAmdExtD3DFactory *iface, IUnknown *outer, REFIID iid, void **out)
{
    char buf[40];
    HRESULT hr = E_NOINTERFACE;

    *out = NULL;
    if (is_ext_device_iid(iid))
    {
        *out = &ext_device;
        hr = S_OK;
    }
    else if (IsEqualGUID(iid, &IID_IAmdExtAntiLagApi) && outer)
        hr = IUnknown_QueryInterface(outer, iid, out);
    /* IAmdExtD3DShaderIntrinsics (AMD's shader intrinsics through a magic UAV) is declined: FSR then doesn't use them. */
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
