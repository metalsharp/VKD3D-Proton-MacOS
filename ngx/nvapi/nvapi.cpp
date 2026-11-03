// VKD3D-Proton-MacOS's NVAPI: what NVIDIA's libraries (Streamline, NGX) and games ask NVIDIA's driver, answered for
// the NVIDIA GPU DXVK's DXGI shows (its adapters, their LUIDs and description), so DLSS finds the GPU it runs on.
// Nothing here reaches the Mac's GPU directly.
//
// From Mullion's NVAPI (src/nvapi/nvapi.cpp, LGPL-2.1), with DXGI's adapters and Win32's monitors in place of
// Mullion's Metal devices and displays.
//
// This library is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser General
// Public License as published by the Free Software Foundation; either version 2.1 of the License, or (at your
// option) any later version.
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d11.h>
#include <d3d12.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#define __NVAPI_EMPTY_SAL
#include "nvapi.h"
#include "nvapi_interface.h"
#include "nvShaderExtnEnums.h"
#undef __NVAPI_EMPTY_SAL

#include "../common/lane.hpp"
#include "dxvk_ext_context.h"

namespace {

std::atomic_uint32_t s_initialization_count = 0;

// DXGI's adapters, in its order; a GPU handle is an adapter's index + 1
std::vector<DXGI_ADAPTER_DESC1> Adapters() {
  std::vector<DXGI_ADAPTER_DESC1> adapters;
  IDXGIFactory1 *factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory)))
    return adapters;
  IDXGIAdapter1 *adapter = nullptr;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; i++) {
    DXGI_ADAPTER_DESC1 desc;
    if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
      adapters.push_back(desc);
    adapter->Release();
  }
  factory->Release();
  return adapters;
}

bool Adapter(NvPhysicalGpuHandle handle, DXGI_ADAPTER_DESC1 *desc) {
  auto adapters = Adapters();
  uintptr_t index = uintptr_t(handle);
  if (!index || index > adapters.size())
    return false;
  *desc = adapters[index - 1];
  return true;
}

// the monitors, the primary one first; a display ID is a monitor's index + 0x1000
std::vector<HMONITOR> Monitors() {
  std::vector<HMONITOR> monitors;
  EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
        auto list = reinterpret_cast<std::vector<HMONITOR> *>(data);
        MONITORINFO info = {sizeof(info)};
        if (GetMonitorInfoW(monitor, &info) && (info.dwFlags & MONITORINFOF_PRIMARY))
          list->insert(list->begin(), monitor);
        else
          list->push_back(monitor);
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&monitors));
  return monitors;
}

constexpr NvU32 kDisplayIdBase = 0x1000;

// the driver reported: the display adapter's in Windows' device list (DriverVersion, which MetalSharp Wine's
// winemac.conf sets; NVIDIA's 617.14 is x.x.16.1714, the last five digits), so NVAPI and the device list agree, else
// the one these NVAPI headers come from. The branch is r617_00, copied by NVIDIA's libraries with its NUL
NvU32 DriverVersion() {
  static NvU32 version = [] {
    WCHAR text[64];
    DWORD size = sizeof(text);
    unsigned a, b, c, d;
    if (RegGetValueW(HKEY_LOCAL_MACHINE,
                     L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}\\0000",
                     L"DriverVersion", RRF_RT_REG_SZ, nullptr, text, &size) == ERROR_SUCCESS &&
        swscanf(text, L"%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
      NvU32 nvidia = (c * 10000 + d) % 100000;
      if (nvidia >= 30000)
        return nvidia;
    }
    return NvU32(NVAPI_SDK_VERSION);
  }();
  return version;
}

void CopyString(NvAPI_ShortString out, const char *text) {
  memset(out, 0, sizeof(NvAPI_ShortString));
  strncpy(out, text, sizeof(NvAPI_ShortString) - 1);
}

void DriverBranch(NvAPI_ShortString branch) {
  char text[32];
  snprintf(text, sizeof(text), "r%u_00", unsigned(DriverVersion() / 100));
  CopyString(branch, text);
}

} // namespace

#define NVAPI_EXPORT extern "C" NvAPI_Status __cdecl

NVAPI_EXPORT NvAPI_Initialize() {
  auto adapters = Adapters();
  if (adapters.empty() || adapters[0].VendorId != 0x10de)
    return NVAPI_NVIDIA_DEVICE_NOT_FOUND;
  s_initialization_count++;
  return NVAPI_OK;
}

NVAPI_EXPORT NvAPI_Unload() {
  if (!s_initialization_count)
    return NVAPI_API_NOT_INITIALIZED;
  s_initialization_count--;
  return NVAPI_OK;
}

NVAPI_EXPORT NvAPI_SYS_GetDriverAndBranchVersion(NvU32 *pDriverVersion, NvAPI_ShortString szBuildBranchString) {
  if (!pDriverVersion || !szBuildBranchString)
    return NVAPI_INVALID_ARGUMENT;
  DriverBranch(szBuildBranchString);
  *pDriverVersion = DriverVersion();
  return NVAPI_OK;
}

NVAPI_EXPORT NvAPI_GetDisplayDriverVersion(NvDisplayHandle hNvDisplay, NV_DISPLAY_DRIVER_VERSION *pVersion) {
  if (!pVersion)
    return NVAPI_INVALID_ARGUMENT;
  if (pVersion->version != NV_DISPLAY_DRIVER_VERSION_VER)
    return NVAPI_INCOMPATIBLE_STRUCT_VERSION;
  pVersion->drvVersion = DriverVersion();
  pVersion->bldChangeListNum = 0;
  DriverBranch(pVersion->szBuildBranchString);
  CopyString(pVersion->szAdapterString, "NVIDIA GeForce RTX");
  return NVAPI_OK;
}

NVAPI_EXPORT NvAPI_GetInterfaceVersionString(NvAPI_ShortString szDesc) {
  if (!szDesc)
    return NVAPI_INVALID_ARGUMENT;
  CopyString(szDesc, "NVAPI Open Source Interface (VKD3D-Proton-MacOS)");
  return NVAPI_OK;
}

NVAPI_EXPORT NvAPI_GetErrorMessage(NvAPI_Status nr, NvAPI_ShortString szDesc) {
  if (!szDesc)
    return NVAPI_INVALID_ARGUMENT;
  szDesc[0] = '\0';
  return NVAPI_OK;
}

NVAPI_EXPORT NvAPI_D3D_GetCurrentSLIState(IUnknown *pDevice, NV_GET_CURRENT_SLI_STATE *pSliState) {
  if (!pDevice || !pSliState)
    return NVAPI_INVALID_ARGUMENT;
  switch (pSliState->version) {
  case NV_GET_CURRENT_SLI_STATE_VER2:
    pSliState->numVRSLIGpus = 0;
    [[fallthrough]];
  case NV_GET_CURRENT_SLI_STATE_VER1:
    pSliState->maxNumAFRGroups = 1;
    pSliState->numAFRGroups = 1;
    pSliState->currentAFRIndex = 0;
    pSliState->nextFrameAFRIndex = 0;
    pSliState->previousFrameAFRIndex = 0;
    pSliState->bIsCurAFRGroupNew = FALSE;
    return NVAPI_OK;
  default:
    return NVAPI_INCOMPATIBLE_STRUCT_VERSION;
  }
}

// D3D11's NVIDIA extensions, on DXVK's context extension (ID3D11VkExtContext), as DXVK-NVAPI does
namespace {

ID3D11VkExtContext *GetExtContext(IUnknown *pDeviceOrContext) {
  ID3D11VkExtContext *ext = nullptr;
  ID3D11DeviceContext *context = nullptr;
  ID3D11Device *device = nullptr;
  if (!pDeviceOrContext)
    return nullptr;
  if (SUCCEEDED(pDeviceOrContext->QueryInterface(__uuidof(ID3D11VkExtContext), (void **)&ext)))
    return ext;
  if (FAILED(pDeviceOrContext->QueryInterface(__uuidof(ID3D11Device), (void **)&device)))
    return nullptr;
  device->GetImmediateContext(&context);
  device->Release();
  if (context) {
    if (FAILED(context->QueryInterface(__uuidof(ID3D11VkExtContext), (void **)&ext)))
      ext = nullptr;
    context->Release();
  }
  return ext;
}

NvAPI_Status SetBarrierControl(IUnknown *pDeviceOrContext, UINT flags) {
  auto ext = GetExtContext(pDeviceOrContext);
  if (!ext)
    return NVAPI_INVALID_ARGUMENT;
  ext->SetBarrierControl(flags);
  ext->Release();
  return NVAPI_OK;
}

} // namespace

NVAPI_EXPORT NvAPI_D3D11_BeginUAVOverlap(IUnknown *pDeviceOrContext) {
  return SetBarrierControl(pDeviceOrContext, D3D11_VK_BARRIER_CONTROL_IGNORE_WRITE_AFTER_WRITE);
}

NVAPI_EXPORT NvAPI_D3D11_EndUAVOverlap(IUnknown *pDeviceOrContext) {
  return SetBarrierControl(pDeviceOrContext, 0);
}

NVAPI_EXPORT NvAPI_D3D11_SetDepthBoundsTest(IUnknown *pDeviceOrContext, NvU32 bEnable, float fMinDepth,
                                            float fMaxDepth) {
  auto ext = GetExtContext(pDeviceOrContext);
  if (!ext)
    return NVAPI_INVALID_ARGUMENT;
  ext->SetDepthBoundsTest(bEnable, fMinDepth, fMaxDepth);
  ext->Release();
  return NVAPI_OK;
}

// no NVIDIA shader extension opcodes: Unreal Engine (64-bit atomics, shuffles) and Tomb Raider (FP16 atomics) ask
NVAPI_EXPORT NvAPI_D3D11_IsNvShaderExtnOpCodeSupported(IUnknown *pDev, NvU32 opCode, bool *pSupported) {
  if (!pSupported)
    return NVAPI_INVALID_ARGUMENT;
  *pSupported = false;
  return NVAPI_OK;
}

NVAPI_EXPORT NvAPI_D3D12_IsNvShaderExtnOpCodeSupported(ID3D12Device *pDevice, NvU32 opCode, bool *pSupported) {
  if (!pSupported)
    return NVAPI_INVALID_ARGUMENT;
  *pSupported = false;
  return NVAPI_OK;
}

NVAPI_EXPORT NvAPI_D3D11_SetNvShaderExtnSlot(IUnknown *pDev, NvU32 uavSlot) {
  return NVAPI_NOT_SUPPORTED;
}

NVAPI_EXPORT NvAPI_DISP_GetDisplayIdByDisplayName(const char *displayName, NvU32 *displayId) {
  if (!displayName || !displayId)
    return NVAPI_INVALID_ARGUMENT;
  auto monitors = Monitors();
  for (size_t i = 0; i < monitors.size(); i++) {
    MONITORINFOEXA info;
    info.cbSize = sizeof(info);
    if (GetMonitorInfoA(monitors[i], &info) && !strcmp(info.szDevice, displayName)) {
      *displayId = kDisplayIdBase + NvU32(i);
      return NVAPI_OK;
    }
  }
  return NVAPI_NVIDIA_DEVICE_NOT_FOUND;
}

NVAPI_EXPORT NvAPI_DISP_GetGDIPrimaryDisplayId(NvU32 *displayId) {
  if (!displayId)
    return NVAPI_INVALID_ARGUMENT;
  *displayId = kDisplayIdBase;
  return NVAPI_OK;
}

// SLI's: the resource as its handle, and no hints
NVAPI_EXPORT NvAPI_D3D_GetObjectHandleForResource(IUnknown *pDevice, IUnknown *pResource, NVDX_ObjectHandle *pHandle) {
  if (!pHandle)
    return NVAPI_INVALID_ARGUMENT;
  *pHandle = (NVDX_ObjectHandle)pResource;
  return NVAPI_OK;
}

NVAPI_EXPORT NvAPI_D3D_SetResourceHint(IUnknown *pDev, NVDX_ObjectHandle obj,
                                       NVAPI_D3D_SETRESOURCEHINT_CATEGORY dwHintCategory, NvU32 dwHintName,
                                       NvU32 *pdwHintValue) {
  return NVAPI_OK;
}

NVAPI_EXPORT NvAPI_EnumPhysicalGPUs(NvPhysicalGpuHandle nvGPUHandle[NVAPI_MAX_PHYSICAL_GPUS], NvU32 *pGpuCount) {
  if (!nvGPUHandle || !pGpuCount)
    return NVAPI_INVALID_ARGUMENT;
  auto count = std::min<size_t>(Adapters().size(), NVAPI_MAX_PHYSICAL_GPUS);
  if (!count)
    return NVAPI_NVIDIA_DEVICE_NOT_FOUND;
  for (size_t i = 0; i < count; i++)
    nvGPUHandle[i] = (NvPhysicalGpuHandle)(i + 1);
  *pGpuCount = NvU32(count);
  return NVAPI_OK;
}

// a logical GPU is one physical one, with the same handle
NVAPI_EXPORT NvAPI_EnumLogicalGPUs(NvLogicalGpuHandle nvGPUHandle[NVAPI_MAX_LOGICAL_GPUS], NvU32 *pGpuCount) {
  return NvAPI_EnumPhysicalGPUs((NvPhysicalGpuHandle *)nvGPUHandle, pGpuCount);
}

NVAPI_EXPORT NvAPI_GetLogicalGPUFromPhysicalGPU(NvPhysicalGpuHandle hPhysicalGPU, NvLogicalGpuHandle *pLogicalGPU) {
  DXGI_ADAPTER_DESC1 desc;
  if (!pLogicalGPU)
    return NVAPI_INVALID_ARGUMENT;
  if (!Adapter(hPhysicalGPU, &desc))
    return NVAPI_EXPECTED_PHYSICAL_GPU_HANDLE;
  *pLogicalGPU = (NvLogicalGpuHandle)hPhysicalGPU;
  return NVAPI_OK;
}

NVAPI_EXPORT NvAPI_GetPhysicalGPUsFromDisplay(NvDisplayHandle hNvDisp, NvPhysicalGpuHandle nvGPUHandle[NVAPI_MAX_PHYSICAL_GPUS],
                                              NvU32 *pGpuCount) {
  if (!hNvDisp)
    return NVAPI_INVALID_ARGUMENT;
  return NvAPI_EnumPhysicalGPUs(nvGPUHandle, pGpuCount);
}

NVAPI_EXPORT NvAPI_EnumNvidiaDisplayHandle(NvU32 thisEnum, NvDisplayHandle *pNvDispHandle) {
  if (!pNvDispHandle)
    return NVAPI_INVALID_ARGUMENT;
  auto monitors = Monitors();
  if (thisEnum >= monitors.size())
    return NVAPI_END_ENUMERATION;
  *pNvDispHandle = (NvDisplayHandle)monitors[thisEnum];
  return NVAPI_OK;
}

NVAPI_EXPORT NvAPI_GPU_GetConnectedDisplayIds(NvPhysicalGpuHandle hPhysicalGpu, NV_GPU_DISPLAYIDS *pDisplayIds,
                                              NvU32 *pDisplayIdCount, NvU32 flags) {
  if (!pDisplayIdCount)
    return NVAPI_INVALID_ARGUMENT;
  NvU32 count = NvU32(Monitors().size());
  if (!pDisplayIds) {
    *pDisplayIdCount = count;
    return NVAPI_OK;
  }
  if (*pDisplayIdCount < count) {
    *pDisplayIdCount = count;
    return NVAPI_INSUFFICIENT_BUFFER;
  }
  for (NvU32 i = 0; i < count; i++) {
    auto version = pDisplayIds[i].version;
    if (version != NV_GPU_DISPLAYIDS_VER1 && version != NV_GPU_DISPLAYIDS_VER2)
      return NVAPI_INCOMPATIBLE_STRUCT_VERSION;
    pDisplayIds[i] = {};
    pDisplayIds[i].version = version;
    pDisplayIds[i].displayId = kDisplayIdBase + i;
    pDisplayIds[i].connectorType = NV_MONITOR_CONN_TYPE_UNKNOWN;
    pDisplayIds[i].isActive = true;
    pDisplayIds[i].isConnected = true;
    pDisplayIds[i].isPhysicallyConnected = true;
  }
  *pDisplayIdCount = count;
  return NVAPI_OK;
}

// no HDR through NVAPI: the swapchain's color space is DXGI's
NVAPI_EXPORT NvAPI_Disp_GetHdrCapabilities(NvU32 displayId, NV_HDR_CAPABILITIES *pHdrCapabilities) {
  if (!pHdrCapabilities)
    return NVAPI_INVALID_ARGUMENT;
  switch (pHdrCapabilities->version) {
  case NV_HDR_CAPABILITIES_VER1:
    *reinterpret_cast<NV_HDR_CAPABILITIES_V1 *>(pHdrCapabilities) = {};
    break;
  case NV_HDR_CAPABILITIES_VER2:
    *reinterpret_cast<NV_HDR_CAPABILITIES_V2 *>(pHdrCapabilities) = {};
    break;
  case NV_HDR_CAPABILITIES_VER3:
    *pHdrCapabilities = {};
    break;
  default:
    return NVAPI_INCOMPATIBLE_STRUCT_VERSION;
  }
  pHdrCapabilities->version = pHdrCapabilities->version ? pHdrCapabilities->version : NV_HDR_CAPABILITIES_VER3;
  return NVAPI_OK;
}

NVAPI_EXPORT NvAPI_Disp_HdrColorControl(NvU32 displayId, NV_HDR_COLOR_DATA *pHdrColorData) {
  if (!pHdrColorData)
    return NVAPI_INVALID_ARGUMENT;
  if (pHdrColorData->version != NV_HDR_COLOR_DATA_VER1 && pHdrColorData->version != NV_HDR_COLOR_DATA_VER2)
    return NVAPI_INCOMPATIBLE_STRUCT_VERSION;
  if (pHdrColorData->cmd != NV_HDR_CMD_GET)
    return NVAPI_NOT_SUPPORTED;
  pHdrColorData->hdrMode = NV_HDR_MODE_OFF;
  if (pHdrColorData->version == NV_HDR_COLOR_DATA_VER2) {
    auto v2 = reinterpret_cast<NV_HDR_COLOR_DATA_V2 *>(pHdrColorData);
    v2->hdrColorFormat = NV_COLOR_FORMAT_RGB;
    v2->hdrDynamicRange = NV_DYNAMIC_RANGE_VESA;
    v2->hdrBpc = NV_BPC_8;
  }
  return NVAPI_OK;
}

NVAPI_EXPORT NvAPI_DISP_GetMonitorCapabilities(NvU32 displayId, NV_MONITOR_CAPABILITIES *pMonitorCapabilities) {
  return pMonitorCapabilities ? NVAPI_NOT_SUPPORTED : NVAPI_INVALID_ARGUMENT;
}

// an Ada GPU, as the device list's RTX 4080
NVAPI_EXPORT NvAPI_GPU_GetArchInfo(NvPhysicalGpuHandle hPhysicalGpu, NV_GPU_ARCH_INFO *pGpuArchInfo) {
  DXGI_ADAPTER_DESC1 desc;
  if (!pGpuArchInfo)
    return NVAPI_INVALID_ARGUMENT;
  if (!Adapter(hPhysicalGpu, &desc))
    return NVAPI_EXPECTED_PHYSICAL_GPU_HANDLE;
  switch (pGpuArchInfo->version) {
  case NV_GPU_ARCH_INFO_VER_1: {
    auto v1 = (NV_GPU_ARCH_INFO_V1 *)pGpuArchInfo;
    v1->architecture = NV_GPU_ARCHITECTURE_AD100;
    v1->implementation = NV_GPU_ARCH_IMPLEMENTATION_AD103;
    v1->revision = NV_GPU_CHIP_REV_UNKNOWN;
    return NVAPI_OK;
  }
  case NV_GPU_ARCH_INFO_VER_2:
    pGpuArchInfo->architecture_id = NV_GPU_ARCHITECTURE_AD100;
    pGpuArchInfo->implementation_id = NV_GPU_ARCH_IMPLEMENTATION_AD103;
    pGpuArchInfo->revision_id = NV_GPU_CHIP_REV_UNKNOWN;
    return NVAPI_OK;
  default:
    return NVAPI_INCOMPATIBLE_STRUCT_VERSION;
  }
}

NVAPI_EXPORT NvAPI_GPU_GetFullName(NvPhysicalGpuHandle hPhysicalGpu, NvAPI_ShortString szName) {
  DXGI_ADAPTER_DESC1 desc;
  char name[128];
  if (!szName)
    return NVAPI_INVALID_ARGUMENT;
  if (!Adapter(hPhysicalGpu, &desc))
    return NVAPI_EXPECTED_PHYSICAL_GPU_HANDLE;
  WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
  CopyString(szName, name);
  return NVAPI_OK;
}

NVAPI_EXPORT NvAPI_GPU_GetMemoryInfo(NvPhysicalGpuHandle hPhysicalGpu, NV_DISPLAY_DRIVER_MEMORY_INFO *pMemoryInfo) {
  DXGI_ADAPTER_DESC1 desc;
  if (!pMemoryInfo)
    return NVAPI_INVALID_ARGUMENT;
  if (!Adapter(hPhysicalGpu, &desc))
    return NVAPI_EXPECTED_PHYSICAL_GPU_HANDLE;
  if (pMemoryInfo->version != NV_DISPLAY_DRIVER_MEMORY_INFO_VER_1 &&
      pMemoryInfo->version != NV_DISPLAY_DRIVER_MEMORY_INFO_VER_2 &&
      pMemoryInfo->version != NV_DISPLAY_DRIVER_MEMORY_INFO_VER_3)
    return NVAPI_INCOMPATIBLE_STRUCT_VERSION;
  NvU32 version = pMemoryInfo->version;
  NvU32 kb = NvU32(std::min<SIZE_T>(desc.DedicatedVideoMemory / 1024, 0xffffffffu));
  memset(pMemoryInfo, 0, sizeof(NV_DISPLAY_DRIVER_MEMORY_INFO_V1));
  pMemoryInfo->version = version;
  pMemoryInfo->dedicatedVideoMemory = kb;
  pMemoryInfo->availableDedicatedVideoMemory = kb;
  pMemoryInfo->curAvailableDedicatedVideoMemory = kb;
  pMemoryInfo->sharedSystemMemory = NvU32(std::min<SIZE_T>(desc.SharedSystemMemory / 1024, 0xffffffffu));
  return NVAPI_OK;
}

NVAPI_EXPORT NvAPI_GPU_GetMemoryInfoEx(NvPhysicalGpuHandle hPhysicalGpu, NV_GPU_MEMORY_INFO_EX *pMemoryInfo) {
  return pMemoryInfo ? NVAPI_NOT_SUPPORTED : NVAPI_INVALID_ARGUMENT;
}

NVAPI_EXPORT NvAPI_GPU_GetAllClockFrequencies(NvPhysicalGpuHandle hPhysicalGPU, NV_GPU_CLOCK_FREQUENCIES *pClkFreqs) {
  return pClkFreqs ? NVAPI_NOT_SUPPORTED : NVAPI_INVALID_ARGUMENT;
}

NVAPI_EXPORT NvAPI_GPU_GetGpuCoreCount(NvPhysicalGpuHandle hPhysicalGpu, NvU32 *pCount) {
  return pCount ? NVAPI_NOT_SUPPORTED : NVAPI_INVALID_ARGUMENT;
}

NVAPI_EXPORT NvAPI_GPU_GetBusId(NvPhysicalGpuHandle hPhysicalGpu, NvU32 *pBusId) {
  return pBusId ? NVAPI_NOT_SUPPORTED : NVAPI_INVALID_ARGUMENT;
}

NVAPI_EXPORT NvAPI_GPU_GetBusType(NvPhysicalGpuHandle hPhysicalGpu, NV_GPU_BUS_TYPE *pBusType) {
  return pBusType ? NVAPI_NOT_SUPPORTED : NVAPI_INVALID_ARGUMENT;
}

NVAPI_EXPORT NvAPI_GPU_GetPstates20(NvPhysicalGpuHandle hPhysicalGpu, NV_GPU_PERF_PSTATES20_INFO *pPstatesInfo) {
  return pPstatesInfo ? NVAPI_NO_IMPLEMENTATION : NVAPI_INVALID_ARGUMENT;
}

// the adapter's LUID as DXGI reports it: Streamline and games match NVIDIA's GPUs to DXGI's adapters by it
NVAPI_EXPORT NvAPI_GPU_GetAdapterIdFromPhysicalGpu(NvPhysicalGpuHandle hPhysicalGpu, void *pOSAdapterId) {
  DXGI_ADAPTER_DESC1 desc;
  if (!pOSAdapterId)
    return NVAPI_INVALID_ARGUMENT;
  if (!Adapter(hPhysicalGpu, &desc))
    return NVAPI_INVALID_ARGUMENT;
  *reinterpret_cast<LUID *>(pOSAdapterId) = desc.AdapterLuid;
  return NVAPI_OK;
}

NVAPI_EXPORT NvAPI_GPU_GetLogicalGpuInfo(NvLogicalGpuHandle hLogicalGpu, NV_LOGICAL_GPU_DATA *pLogicalGpuData) {
  DXGI_ADAPTER_DESC1 desc;
  if (!pLogicalGpuData)
    return NVAPI_INVALID_ARGUMENT;
  if (!Adapter((NvPhysicalGpuHandle)hLogicalGpu, &desc))
    return NVAPI_NVIDIA_DEVICE_NOT_FOUND;
  if (pLogicalGpuData->version != NV_LOGICAL_GPU_DATA_VER1)
    return NVAPI_INCOMPATIBLE_STRUCT_VERSION;
  pLogicalGpuData->physicalGpuCount = 1;
  memset(pLogicalGpuData->physicalGpuHandles, 0, sizeof(pLogicalGpuData->physicalGpuHandles));
  pLogicalGpuData->physicalGpuHandles[0] = (NvPhysicalGpuHandle)hLogicalGpu;
  if (pLogicalGpuData->pOSAdapterId)
    *reinterpret_cast<LUID *>(pLogicalGpuData->pOSAdapterId) = desc.AdapterLuid;
  return NVAPI_OK;
}

// Reflex: no low-latency mode, markers accepted
NVAPI_EXPORT NvAPI_D3D_GetSleepStatus(IUnknown *pDev, NV_GET_SLEEP_STATUS_PARAMS *pGetSleepStatusParams) {
  if (!pDev || !pGetSleepStatusParams)
    return NVAPI_INVALID_ARGUMENT;
  if (pGetSleepStatusParams->version != NV_GET_SLEEP_STATUS_PARAMS_VER1)
    return NVAPI_INCOMPATIBLE_STRUCT_VERSION;
  pGetSleepStatusParams->bLowLatencyMode = false;
  pGetSleepStatusParams->bFsVrr = false;
  pGetSleepStatusParams->bCplVsyncOn = false;
  pGetSleepStatusParams->sleepIntervalUs = 0;
  pGetSleepStatusParams->bUseGameSleep = false;
  return NVAPI_OK;
}

NVAPI_EXPORT NvAPI_D3D_SetSleepMode(IUnknown *pDev, NV_SET_SLEEP_MODE_PARAMS *pSetSleepModeParams) {
  if (!pDev || !pSetSleepModeParams)
    return NVAPI_INVALID_ARGUMENT;
  return pSetSleepModeParams->version == NV_SET_SLEEP_MODE_PARAMS_VER1 ? NVAPI_OK : NVAPI_INCOMPATIBLE_STRUCT_VERSION;
}

NVAPI_EXPORT NvAPI_D3D_SetLatencyMarker(IUnknown *pDev, NV_LATENCY_MARKER_PARAMS *pSetLatencyMarkerParams) {
  if (!pDev || !pSetLatencyMarkerParams)
    return NVAPI_INVALID_ARGUMENT;
  return pSetLatencyMarkerParams->version == NV_LATENCY_MARKER_PARAMS_VER1 ? NVAPI_OK : NVAPI_INCOMPATIBLE_STRUCT_VERSION;
}

// Reflex Sync, which Streamline's Frame Generation pacing (RSYNC) hands its queue timing to every frame: the timing is
// for the driver's present metering, and Streamline paces the frames itself, as it does on GPUs without metering
NVAPI_EXPORT NvAPI_D3D_SetReflexSync(IUnknown *pDev, NV_SET_REFLEX_SYNC_PARAMS *pSetReflexSyncParams) {
  if (!pDev || !pSetReflexSyncParams)
    return NVAPI_INVALID_ARGUMENT;
  return pSetReflexSyncParams->version == NV_SET_REFLEX_SYNC_PARAMS_VER1 ? NVAPI_OK : NVAPI_INCOMPATIBLE_STRUCT_VERSION;
}

// the frame markers of a queue whose work is not the frame's (Frame Generation's), and which queues those are: for
// Reflex's latency reports, which have no figures here (NvAPI_D3D_GetLatency)
NVAPI_EXPORT NvAPI_D3D12_SetAsyncFrameMarker(ID3D12CommandQueue *pCommandQueue,
                                             NV_ASYNC_FRAME_MARKER_PARAMS *pSetAsyncFrameMarkerParams) {
  if (!pCommandQueue || !pSetAsyncFrameMarkerParams)
    return NVAPI_INVALID_ARGUMENT;
  return pSetAsyncFrameMarkerParams->version == NV_ASYNC_FRAME_MARKER_PARAMS_VER1 ? NVAPI_OK
                                                                                  : NVAPI_INCOMPATIBLE_STRUCT_VERSION;
}

NVAPI_EXPORT NvAPI_D3D12_NotifyOutOfBandCommandQueue(ID3D12CommandQueue *pCommandQueue, NV_OUT_OF_BAND_CQ_TYPE cqType) {
  return pCommandQueue ? NVAPI_OK : NVAPI_INVALID_ARGUMENT;
}

NVAPI_EXPORT NvAPI_D3D_Sleep(IUnknown *pDev) {
  return pDev ? NVAPI_OK : NVAPI_INVALID_ARGUMENT;
}

NVAPI_EXPORT NvAPI_D3D_GetLatency(IUnknown *pDev, NV_LATENCY_RESULT_PARAMS *pGetLatencyParams) {
  if (!pDev || !pGetLatencyParams)
    return NVAPI_INVALID_ARGUMENT;
  if (pGetLatencyParams->version != NV_LATENCY_RESULT_PARAMS_VER1)
    return NVAPI_INCOMPATIBLE_STRUCT_VERSION;
  memset(pGetLatencyParams->frameReport, 0, sizeof(pGetLatencyParams->frameReport));
  return NVAPI_OK;
}

NVAPI_EXPORT NvAPI_D3D_SetVerticalSyncMode(IUnknown *pDev, NVAPI_VSYNC_MODE vsyncMode) {
  return pDev ? NVAPI_NO_IMPLEMENTATION : NVAPI_INVALID_ARGUMENT;
}

NVAPI_EXPORT NvAPI_Mosaic_GetDisplayViewportsByResolution(NvU32 displayId, NvU32 srcWidth, NvU32 srcHeight,
                                                          NV_RECT viewports[NV_MOSAIC_MAX_DISPLAYS], NvU8 *bezelCorrected) {
  return NVAPI_MOSAIC_NOT_ACTIVE;
}

NVAPI_EXPORT NvAPI_Stereo_IsEnabled(NvU8 *pIsStereoEnabled) {
  return pIsStereoEnabled ? NVAPI_STEREO_NOT_INITIALIZED : NVAPI_INVALID_ARGUMENT;
}

NVAPI_EXPORT NvAPI_Stereo_SetDriverMode(NV_STEREO_DRIVER_MODE mode) {
  return NVAPI_STEREO_NOT_INITIALIZED;
}

// driver settings (DRS) as a driver that has none for any program: a session with its base and global profile, no
// application profiles, and no setting anywhere, so what asks (Streamline, which needs a session for Frame Generation)
// uses its defaults (from Mullion's nvapi)
static int drs_session, drs_base_profile;

NVAPI_EXPORT NvAPI_DRS_CreateSession(NvDRSSessionHandle *phSession) {
  if (!phSession)
    return NVAPI_INVALID_ARGUMENT;
  *phSession = reinterpret_cast<NvDRSSessionHandle>(&drs_session);
  return NVAPI_OK;
}

NVAPI_EXPORT NvAPI_DRS_DestroySession(NvDRSSessionHandle hSession) {
  return hSession == reinterpret_cast<NvDRSSessionHandle>(&drs_session) ? NVAPI_OK : NVAPI_INVALID_HANDLE;
}

NVAPI_EXPORT NvAPI_DRS_LoadSettings(NvDRSSessionHandle hSession) {
  return hSession == reinterpret_cast<NvDRSSessionHandle>(&drs_session) ? NVAPI_OK : NVAPI_INVALID_HANDLE;
}

NVAPI_EXPORT NvAPI_DRS_GetBaseProfile(NvDRSSessionHandle hSession, NvDRSProfileHandle *phProfile) {
  if (!phProfile)
    return NVAPI_INVALID_ARGUMENT;
  *phProfile = reinterpret_cast<NvDRSProfileHandle>(&drs_base_profile);
  return NVAPI_OK;
}

NVAPI_EXPORT NvAPI_DRS_GetCurrentGlobalProfile(NvDRSSessionHandle hSession, NvDRSProfileHandle *phProfile) {
  if (!phProfile)
    return NVAPI_INVALID_ARGUMENT;
  *phProfile = reinterpret_cast<NvDRSProfileHandle>(&drs_base_profile);
  return NVAPI_OK;
}

NVAPI_EXPORT NvAPI_DRS_FindProfileByName(NvDRSSessionHandle hSession, NvAPI_UnicodeString profileName,
                                         NvDRSProfileHandle *phProfile) {
  return NVAPI_PROFILE_NOT_FOUND;
}

NVAPI_EXPORT NvAPI_DRS_FindApplicationByName(NvDRSSessionHandle hSession, NvAPI_UnicodeString appName,
                                             NvDRSProfileHandle *phProfile, NVDRS_APPLICATION *pApplication) {
  return NVAPI_EXECUTABLE_NOT_FOUND;
}

NVAPI_EXPORT NvAPI_DRS_GetSetting(NvDRSSessionHandle hSession, NvDRSProfileHandle hProfile, NvU32 settingId,
                                  NVDRS_SETTING *pSetting) {
  return pSetting ? NVAPI_SETTING_NOT_FOUND : NVAPI_INVALID_ARGUMENT;
}

namespace {

void *QueryInterfaceImpl(NvU32 id) {
  switch (id) {
  case 0x0150e828: return (void *)&NvAPI_Initialize;
  case 0x2926aaad: return (void *)&NvAPI_SYS_GetDriverAndBranchVersion;
  case 0xf951a4d1: return (void *)&NvAPI_GetDisplayDriverVersion;
  case 0x01053fa5: return (void *)&NvAPI_GetInterfaceVersionString;
  case 0x4b708b54: return (void *)&NvAPI_D3D_GetCurrentSLIState;
  case 0xd22bdd7e: return (void *)&NvAPI_Unload;
  case 0x6c2d048c: return (void *)&NvAPI_GetErrorMessage;
  case 0x65b93ca8: return (void *)&NvAPI_D3D11_BeginUAVOverlap;
  case 0x2216a357: return (void *)&NvAPI_D3D11_EndUAVOverlap;
  case 0x7aaf7a04: return (void *)&NvAPI_D3D11_SetDepthBoundsTest;
  case 0x5f68da40: return (void *)&NvAPI_D3D11_IsNvShaderExtnOpCodeSupported;
  case 0x3dfacec8: return (void *)&NvAPI_D3D12_IsNvShaderExtnOpCodeSupported;
  case 0x8e90bb9f: return (void *)&NvAPI_D3D11_SetNvShaderExtnSlot;
  case 0xae457190: return (void *)&NvAPI_DISP_GetDisplayIdByDisplayName;
  case 0xfceac864: return (void *)&NvAPI_D3D_GetObjectHandleForResource;
  case 0x6c0ed98c: return (void *)&NvAPI_D3D_SetResourceHint;
  case 0xe5ac921f: return (void *)&NvAPI_EnumPhysicalGPUs;
  case 0x48b3ea59: return (void *)&NvAPI_EnumLogicalGPUs;
  case 0xadd604d1: return (void *)&NvAPI_GetLogicalGPUFromPhysicalGPU;
  case 0x34ef9506: return (void *)&NvAPI_GetPhysicalGPUsFromDisplay;
  case 0x351da224: return (void *)&NvAPI_Disp_HdrColorControl;
  case 0x84f2a8df: return (void *)&NvAPI_Disp_GetHdrCapabilities;
  case 0x3b05c7e1: return (void *)&NvAPI_DISP_GetMonitorCapabilities;
  case 0x9abdd40d: return (void *)&NvAPI_EnumNvidiaDisplayHandle;
  case 0x0078dba2: return (void *)&NvAPI_GPU_GetConnectedDisplayIds;
  case 0x1e9d8a31: return (void *)&NvAPI_DISP_GetGDIPrimaryDisplayId;
  case 0xd8265d24: return (void *)&NvAPI_GPU_GetArchInfo;
  case 0xdcb616c3: return (void *)&NvAPI_GPU_GetAllClockFrequencies;
  case 0xc7026a87: return (void *)&NvAPI_GPU_GetGpuCoreCount;
  case 0x1be0b8e5: return (void *)&NvAPI_GPU_GetBusId;
  case 0x1bb18724: return (void *)&NvAPI_GPU_GetBusType;
  case 0x07f9b368: return (void *)&NvAPI_GPU_GetMemoryInfo;
  case 0xc0599498: return (void *)&NvAPI_GPU_GetMemoryInfoEx;
  case 0xceee8e9f: return (void *)&NvAPI_GPU_GetFullName;
  case 0x6ff81213: return (void *)&NvAPI_GPU_GetPstates20;
  case 0xaef96ca1: return (void *)&NvAPI_D3D_GetSleepStatus;
  case 0xac1ca9e0: return (void *)&NvAPI_D3D_SetSleepMode;
  case 0xd9984c05: return (void *)&NvAPI_D3D_SetLatencyMarker;
  case 0x852cd1d2: return (void *)&NvAPI_D3D_Sleep;
  case 0x1a587f9c: return (void *)&NvAPI_D3D_GetLatency;
  case 0x5526cfd1: return (void *)&NvAPI_D3D_SetVerticalSyncMode;
  case 0xdc6dc8d3: return (void *)&NvAPI_Mosaic_GetDisplayViewportsByResolution;
  case 0x348ff8e1: return (void *)&NvAPI_Stereo_IsEnabled;
  case 0x5e8f0bec: return (void *)&NvAPI_Stereo_SetDriverMode;
  case 0x0694d52e: return (void *)&NvAPI_DRS_CreateSession;
  case 0x375dbd6b: return (void *)&NvAPI_DRS_LoadSettings;
  case 0xdad9cff8: return (void *)&NvAPI_DRS_DestroySession;
  case 0xda8466a0: return (void *)&NvAPI_DRS_GetBaseProfile;
  case 0x617bff9f: return (void *)&NvAPI_DRS_GetCurrentGlobalProfile;
  case 0x7e4a9a0b: return (void *)&NvAPI_DRS_FindProfileByName;
  case 0xeee566b2: return (void *)&NvAPI_DRS_FindApplicationByName;
  case 0x73bf8338: return (void *)&NvAPI_DRS_GetSetting;
  case 0xb9f6faff: return (void *)&NvAPI_D3D_SetReflexSync;
  case 0x13c98f73: return (void *)&NvAPI_D3D12_SetAsyncFrameMarker;
  case 0x03d6e8cb: return (void *)&NvAPI_D3D12_NotifyOutOfBandCommandQueue;
  case 0x0ff07fde: return (void *)&NvAPI_GPU_GetAdapterIdFromPhysicalGpu;
  case 0x842b066e: return (void *)&NvAPI_GPU_GetLogicalGpuInfo;
  default: break;
  }
  for (auto &iface : nvapi_interface_table) {
    if (iface.id == id) {
      lane::log("warn", "NVAPI: %s is not implemented", iface.func);
      return nullptr;
    }
  }
  return nullptr;
}

} // namespace

extern "C" void *__cdecl nvapi_QueryInterface(NvU32 id) {
  return QueryInterfaceImpl(id);
}

// only in the programs DXVK's DXGI shows an NVIDIA GPU: any other (Steam's GPU queries among them) finds no NVAPI,
// as it would on another vendor's GPU
extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  if (reason != DLL_PROCESS_ATTACH)
    return TRUE;
  if (!lane::shown_nvidia_gpu())
    return FALSE;
  DisableThreadLibraryCalls(instance);
  return TRUE;
}
