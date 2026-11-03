// VKD3D-Proton-MacOS's NGX core: DLSS Super Resolution on MetalFX's temporal scaler, DLSS Ray Reconstruction on its
// denoised scaler and DLSS Frame Generation on its frame interpolator, recorded in VKD3D-Proton's command lists
// (ID3D12GraphicsCommandListMetalFX) and, for
// Super Resolution in D3D11, DXVK's contexts (ID3D11VkMetalFXContext), which MoltenVK runs. NVIDIA's NGX library loads it as its core (nvngx.dll) where the NVIDIA driver's registry values name it.
//
// From Mullion's NGX core (src/nvngx, LGPL-2.1), with VKD3D-Proton's interface in place of Mullion's.
//
// This library is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser General
// Public License as published by the Free Software Foundation; either version 2.1 of the License, or (at your
// option) any later version.
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ngx.hpp"
#include "parameters.hpp"
#include "../common/lane.hpp"

extern "C" {
#include "vkd3d_metalfx.h"
}
#include "d3d11_metalfx_interface.h"

namespace ngx {

#define NVNGX_SUCCEED(result) (!(NVNGX_FAILED(result)))

#define NVNGX_Parameter_DLSS_Roughness_Mode "DLSS.Roughness.Mode"
#define NVNGX_Parameter_Use_HW_Depth "DLSS.Use.HW.Depth"
#define NVNGX_Parameter_DiffuseAlbedo "DLSS.Input.DiffuseAlbedo"
#define NVNGX_Parameter_SpecularAlbedo "DLSS.Input.SpecularAlbedo"
#define NVNGX_Parameter_GBuffer_Albedo "GBuffer.Albedo"
#define NVNGX_Parameter_GBuffer_Specular "GBuffer.Specular"
#define NVNGX_Parameter_GBuffer_Normals "GBuffer.Normals"
#define NVNGX_Parameter_GBuffer_Roughness "GBuffer.Roughness"
#define NVNGX_Parameter_DLSSD_SpecularHitDistance "DLSSD.SpecularHitDistance"
#define NVNGX_Parameter_WorldToViewMatrix "WorldToViewMatrix"
#define NVNGX_Parameter_ViewToClipMatrix "ViewToClipMatrix"

struct CommonFeature {
  unsigned int handle;
  NVNGX_FEATURE feature;
};

struct DLSSFeature : CommonFeature {
  uint32_t width;
  uint32_t height;
  uint32_t target_width;
  uint32_t target_height;
  int quality;
  int flag;
  int enable_output_subrects;
  // the MetalFX scaler the command list (D3D12) or context (D3D11) made for it
  ID3D12MetalFXScaler *scaler = nullptr;
  ID3D11VkMetalFXScaler *scaler11 = nullptr;
};

// DLSS Ray Reconstruction, on MetalFX's denoised scaler
struct DLSSDFeature : DLSSFeature {
  unsigned int roughness_mode; // 1: roughness in the normals' alpha
  unsigned int hw_depth;       // 0: linear depth, which MetalFX does not take
};

// DLSS Frame Generation (D3D12), on MetalFX's frame interpolator
struct DLSSGFeature : CommonFeature {
  uint32_t width;
  uint32_t height;
  // when the last frame was interpolated to, for the time between frames
  int64_t last_counter = 0;
  ID3D12MetalFXScaler *interpolator = nullptr;
};

// NGX's NVSDK_NGX_FeatureCommonInfo: where the application keeps its feature libraries (nvsdk_ngx_defs.h)
struct NVNGX_PathListInfo {
  const wchar_t *const *Path;
  unsigned int Length;
};
struct NVNGX_FeatureCommonInfo {
  NVNGX_PathListInfo PathListInfo;
  void *InternalData;
};

// the folders an Init named for feature libraries
static std::mutex library_paths_mutex;
static std::vector<std::wstring> library_paths;

static void RememberLibraryPaths(const void *feature_info) {
  auto info = static_cast<const NVNGX_FeatureCommonInfo *>(feature_info);
  if (!info || !info->PathListInfo.Path)
    return;
  std::lock_guard<std::mutex> lock(library_paths_mutex);
  for (unsigned i = 0; i < info->PathListInfo.Length; i++)
    if (info->PathListInfo.Path[i])
      library_paths.emplace_back(info->PathListInfo.Path[i]);
}

// whether the application ships a feature's library (`name`): NVIDIA's NGX core offers a feature only when it finds
// its snippet (nvngx_dlssd.dll for Ray Reconstruction, nvngx_dlssg.dll for Frame Generation) loaded, beside the
// executable, or in a folder Init or the discovery names. a game that ships none asks for the feature anyway where it
// is offered, and then has none of its inputs (Control turns Ray Reconstruction on, and passes none of its textures)
static bool HasFeatureLibrary(const wchar_t *name, const void *feature_info = nullptr) {
  if (GetModuleHandleW(name))
    return true;
  std::vector<std::wstring> folders;
  wchar_t exe[MAX_PATH];
  DWORD length = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  if (length && length < MAX_PATH) {
    std::wstring path(exe, length);
    folders.push_back(path.substr(0, path.find_last_of(L"\\/")));
  }
  {
    std::lock_guard<std::mutex> lock(library_paths_mutex);
    folders.insert(folders.end(), library_paths.begin(), library_paths.end());
  }
  auto info = static_cast<const NVNGX_FeatureCommonInfo *>(feature_info);
  if (info && info->PathListInfo.Path)
    for (unsigned i = 0; i < info->PathListInfo.Length; i++)
      if (info->PathListInfo.Path[i])
        folders.emplace_back(info->PathListInfo.Path[i]);
  for (auto &folder : folders) {
    auto path = folder;
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/')
      path += L'\\';
    if (GetFileAttributesW((path + name).c_str()) != INVALID_FILE_ATTRIBUTES)
      return true;
  }
  return false;
}

// the command list's MetalFX interface, released by the caller
static ID3D12GraphicsCommandListMetalFX *GetMetalFX(ID3D12GraphicsCommandList *list) {
  void *metalfx = nullptr;
  if (!list || FAILED(list->QueryInterface(IID_ID3D12GraphicsCommandListMetalFX, &metalfx)))
    return nullptr;
  return static_cast<ID3D12GraphicsCommandListMetalFX *>(metalfx);
}

struct MetalFXRef {
  ID3D12GraphicsCommandListMetalFX *iface;
  explicit MetalFXRef(ID3D12GraphicsCommandList *list) : iface(GetMetalFX(list)) {}
  ~MetalFXRef() {
    if (iface)
      iface->lpVtbl->Release(iface);
  }
};

static ID3D12Resource *GetResource(const ParametersImpl *params, const char *name, const char *alternative = nullptr) {
  ID3D12Resource *resource = nullptr;
  if (NVNGX_FAILED(params->Get(name, &resource)) && alternative)
    params->Get(alternative, &resource);
  return resource;
}

template <typename T> static T GetOr(const ParametersImpl *params, const char *name, T fallback) {
  T value;
  return NVNGX_FAILED(params->Get(name, &value)) ? fallback : value;
}

static NVNGX_RESULT ReadCreate(const ParametersImpl *params, DLSSFeature &dlss) {
  if (NVNGX_FAILED(params->Get(NVNGX_Parameter_Width, &dlss.width)) ||
      NVNGX_FAILED(params->Get(NVNGX_Parameter_Height, &dlss.height)) ||
      NVNGX_FAILED(params->Get(NVNGX_Parameter_OutWidth, &dlss.target_width)) ||
      NVNGX_FAILED(params->Get(NVNGX_Parameter_OutHeight, &dlss.target_height)))
    return NVNGX_RESULT_INVALID_PARAMETER;
  dlss.quality = GetOr(params, NVNGX_Parameter_PerfQualityValue, 0);
  dlss.flag = GetOr(params, NVNGX_Parameter_DLSS_Feature_Create_Flags, 0);
  dlss.enable_output_subrects = GetOr(params, "DLSS.Enable.Output.Subrects", 0);
  return NVNGX_RESULT_OK;
}

static void ReadCommon(const ParametersImpl *params, float &mv_x, float &mv_y, float &jitter_x, float &jitter_y,
                       float &pre_exposure, BOOL &reset) {
  mv_x = GetOr(params, NVNGX_Parameter_MV_Scale_X, 1.0f);
  mv_y = GetOr(params, NVNGX_Parameter_MV_Scale_Y, 1.0f);
  jitter_x = GetOr(params, NVNGX_Parameter_Jitter_Offset_X, 0.0f);
  jitter_y = GetOr(params, NVNGX_Parameter_Jitter_Offset_Y, 0.0f);
  // DLSS takes 0 (or none) for no pre-exposure, which MetalFX, dividing by it, takes as 1
  pre_exposure = GetOr(params, NVNGX_Parameter_DLSS_Pre_Exposure, 0.0f);
  if (pre_exposure <= 0.0f)
    pre_exposure = 1.0f;
  reset = GetOr(params, NVNGX_Parameter_Reset, 0) != 0;
}

static NVNGX_RESULT GetOptimalSettingsCallback(NVNGXParameter *params) {
  unsigned int width, height, out_width, out_height;
  float scale;
  NVNGX_PERFQUALITY quality;

  if (params->Get(NVNGX_Parameter_Width, &width) != NVNGX_RESULT_OK ||
      params->Get(NVNGX_Parameter_Height, &height) != NVNGX_RESULT_OK ||
      params->Get(NVNGX_Parameter_PerfQualityValue, (int *)&quality) != NVNGX_RESULT_OK)
    return NVNGX_RESULT_FAIL;

  switch (quality) {
  case NVNGX_PERFQUALITY_ULTRA_PERFORMANCE:
    scale = 1.0f / 3.0f;
    break;
  case NVNGX_PERFQUALITY_MAXPERF:
    scale = 0.5f;
    break;
  case NVNGX_PERFQUALITY_MAXQUALITY:
    scale = 1.0f / 1.5f;
    break;
  case NVNGX_PERFQUALITY_ULTRA_QUALITY:
    scale = 1.0f / 1.3f;
    break;
  case NVNGX_PERFQUALITY_DLAA:
    scale = 1.0f;
    break;
  default:
    scale = 58.0f / 100.0f;
    break;
  }

  out_width = std::ceil(width * scale);
  out_height = std::ceil(height * scale);
  params->Set(NVNGX_Parameter_Scale, scale);
  params->Set(NVNGX_Parameter_SuperSampling_ScaleFactor, scale);
  params->Set(NVNGX_Parameter_OutWidth, out_width);
  params->Set(NVNGX_Parameter_OutHeight, out_height);
  params->Set(NVNGX_Parameter_Sharpness, 0.0f);
  params->Set(NVNGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width, width / 3);
  params->Set(NVNGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height, height / 3);
  params->Set(NVNGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width, width);
  params->Set(NVNGX_Parameter_DLSS_Get_Dynamic_Max_Render_Height, height);
  params->Set("DLSSMode", 1);
  params->Set(NVNGX_Parameter_DLSS_Hint_Render_Preset_DLAA, 0u);
  params->Set(NVNGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality, 0u);
  params->Set(NVNGX_Parameter_DLSS_Hint_Render_Preset_Quality, 0u);
  params->Set(NVNGX_Parameter_DLSS_Hint_Render_Preset_Balanced, 0u);
  params->Set(NVNGX_Parameter_DLSS_Hint_Render_Preset_Performance, 0u);
  params->Set(NVNGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, 0u);
  return NVNGX_RESULT_OK;
}

static NVNGX_RESULT GetStatsCallback(NVNGXParameter *params) {
  params->Set("SizeInBytes", 0ull);
  return NVNGX_RESULT_OK;
}

// what Streamline asks of a Frame Generation feature at each frame: that it evaluates every frame (when it is off too,
// so the real frame is copied out), and that no capture is running
static NVNGX_RESULT DLSSG_GetCurrentSettings(unsigned int *handle, NVNGXParameter *params) {
  if (!params)
    return NVNGX_RESULT_INVALID_PARAMETER;
  params->Set("DLSSG.MustCallEval", 1u);
  params->Set("DLSSG.BurstCaptureRunning", 0u);
  return NVNGX_RESULT_OK;
}

// the video memory Frame Generation needs at a size, which Streamline asks before turning it on: MetalFX's interpolator
// keeps a few frames at the output size
static NVNGX_RESULT DLSSG_EstimateVRAM(unsigned int width, unsigned int height, unsigned int, unsigned int, unsigned int,
                                      unsigned int, unsigned int, unsigned int, unsigned int, size_t *estimated) {
  if (estimated)
    *estimated = size_t(std::max(width, 1u)) * std::max(height, 1u) * 4 * 8 + (64 << 20);
  return NVNGX_RESULT_OK;
}

// what the core offers: Super Resolution, Ray Reconstruction (whose availability games read the same way) where the
// application ships its library, and, in D3D12, Frame Generation where it ships its library
static ParametersImpl *CapabilityParameters(bool d3d12) {
  auto params = new ParametersImpl();
  params->Set("SuperSampling.Available", 1u);
  params->Set(NVSDK_NGX_EParameter_SuperSampling_Available, 1u);
  params->Set("SuperSampling.MinDriverVersionMajor", 0);
  params->Set("SuperSampling.MinDriverVersionMinor", 0);
  params->Set("SuperSampling.NeedsUpdatedDriver", 0);
  params->Set("SuperSampling.FeatureInitResult", 1u);
  params->Set("SuperSamplingDenoising.Available", unsigned(HasFeatureLibrary(L"nvngx_dlssd.dll")));
  params->Set("SuperSamplingDenoising.NeedsUpdatedDriver", 0);
  params->Set("SuperSamplingDenoising.MinDriverVersionMajor", 0);
  params->Set("SuperSamplingDenoising.MinDriverVersionMinor", 0);
  params->Set("SuperSamplingDenoising.FeatureInitResult", 1u);
  params->Set("Snippet.OptLevel", 0);
  params->Set("Snippet.IsDevBranch", 0);
  params->Set(NVNGX_Parameter_DLSSOptimalSettingsCallback, (void *)&GetOptimalSettingsCallback);
  params->Set("DLSSGetStatsCallback", (void *)&GetStatsCallback);
  params->Set("DLSSDOptimalSettingsCallback", (void *)&GetOptimalSettingsCallback);
  params->Set("DLSSDGetStatsCallback", (void *)&GetStatsCallback);
  params->Set(NVNGX_Parameter_Sharpness, 0.0f);
  params->Set(NVNGX_Parameter_MV_Scale_X, 1.0f);
  params->Set(NVNGX_Parameter_MV_Scale_Y, 1.0f);
  params->Set("MV.Offset.X", 0.0f);
  params->Set("MV.Offset.Y", 0.0f);
  params->Set("DLSS.Exposure.Scale", 1.0f);
  params->Set(NVNGX_Parameter_PerfQualityValue, 2);
  params->Set("CreationNodeMask", 1);
  params->Set("VisibilityNodeMask", 1);
  params->Set("DLSS.Enable.Output.Subrects", 1);
  params->Set("RTXValue", 0);
  // Frame Generation, as the game's Streamline (sl.dlss_g) reads it: one frame between two, no Reflex frame warp
  unsigned frame_generation = d3d12 && HasFeatureLibrary(L"nvngx_dlssg.dll");
  params->Set("FrameGeneration.Available", frame_generation);
  params->Set("FrameInterpolation.Available", frame_generation);
  params->Set("FrameGeneration.NeedsUpdatedDriver", 0);
  params->Set("FrameGeneration.MinDriverVersionMajor", 0);
  params->Set("FrameGeneration.MinDriverVersionMinor", 0);
  params->Set("FrameGeneration.FeatureInitResult", 1u);
  params->Set("DLSSG.GetCurrentSettingsCallback", (void *)&DLSSG_GetCurrentSettings);
  params->Set("DLSSG.EstimateVRAMCallback", (void *)&DLSSG_EstimateVRAM);
  params->Set("DLSSG.MultiFrameCountMax", 1u);
  params->Set("DLSSG.ReflexWarp.Available", 0u);
  return params;
}

static NVNGX_RESULT FeatureRequirements(const NVNGX_FeatureDiscoveryInfo *discovery, NVNGX_FeatureRequirement *requirement,
                                        bool d3d12) {
  if (!requirement || !discovery)
    return NVNGX_RESULT_INVALID_PARAMETER;
  if (discovery->FeatureID == NVNGX_FEATURE_SUPERSAMPLING ||
      (d3d12 && discovery->FeatureID == NVNGX_FEATURE_RAYRECONSTRUCTION &&
       HasFeatureLibrary(L"nvngx_dlssd.dll", discovery->FeatureInfo)) ||
      (d3d12 && discovery->FeatureID == NVNGX_FEATURE_FRAMEGENERATION &&
       HasFeatureLibrary(L"nvngx_dlssg.dll", discovery->FeatureInfo))) {
    requirement->FeatureSupported = NVNGX_FEATURE_SUPPORT_RESULT_SUPPORTED;
    requirement->MinHWArchitecture = 0;
    strcpy(requirement->MinOSVersion, "10.0.16299.0");
    return NVNGX_RESULT_OK;
  }
  requirement->FeatureSupported = NVNGX_FEATURE_SUPPORT_RESULT_UNSUPPORTED;
  return NVNGX_RESULT_FAIL;
}

} // namespace ngx

using namespace ngx;

#define NVNGX_EXPORT extern "C" __declspec(dllexport)

NVNGX_EXPORT NVNGX_RESULT NVSDK_NGX_UpdateFeature(const void *app_id, const unsigned int feature) {
  return NVNGX_RESULT_OK;
}

// D3D12

NVNGX_EXPORT NVNGX_RESULT NVSDK_NGX_D3D12_Init(unsigned long long id, const wchar_t *path, ID3D12Device *device,
                                               const void *feature_info, unsigned int sdk_version) {
  RememberLibraryPaths(feature_info);
  return NVNGX_RESULT_OK;
}

NVNGX_EXPORT NVNGX_RESULT NVSDK_NGX_D3D12_Init_Ext(unsigned long long id, const wchar_t *path, ID3D12Device *device,
                                                   unsigned int sdk_version, const void *parameters) {
  return NVNGX_RESULT_OK;
}

NVNGX_EXPORT NVNGX_RESULT NVSDK_NGX_D3D12_Init_ProjectID(const char *project, unsigned int engine_type,
                                                         const char *engine_version, const wchar_t *path,
                                                         ID3D12Device *device, unsigned int sdk_version,
                                                         const void *feature_info) {
  RememberLibraryPaths(feature_info);
  return NVNGX_RESULT_OK;
}

NVNGX_EXPORT NVNGX_RESULT NVSDK_NGX_D3D12_Init_with_ProjectID(const char *project, unsigned int engine_type,
                                                              const char *engine_version, const wchar_t *path,
                                                              ID3D12Device *device, const void *feature_info,
                                                              unsigned int sdk_version) {
  RememberLibraryPaths(feature_info);
  return NVNGX_RESULT_OK;
}

NVNGX_EXPORT NVNGX_RESULT NVSDK_NGX_D3D12_Shutdown() {
  return NVNGX_RESULT_OK;
}

NVNGX_EXPORT NVNGX_RESULT NVSDK_NGX_D3D12_Shutdown1(ID3D12Device *device) {
  return NVNGX_RESULT_OK;
}

NVNGX_EXPORT NVNGX_RESULT NVSDK_NGX_D3D12_GetScratchBufferSize(unsigned int feature, const NVNGXParameter *params,
                                                               size_t *out_size) {
  if (out_size)
    *out_size = 0;
  return NVNGX_RESULT_OK;
}

NVNGX_EXPORT NVNGX_RESULT NVSDK_NGX_D3D12_AllocateParameters(NVNGXParameter **out_params) {
  *out_params = new ParametersImpl();
  return NVNGX_RESULT_OK;
}

NVNGX_EXPORT NVNGX_RESULT NVSDK_NGX_D3D12_DestroyParameters(NVNGXParameter *params) {
  delete static_cast<ParametersImpl *>(params);
  return NVNGX_RESULT_OK;
}

NVNGX_EXPORT NVNGX_RESULT NVSDK_NGX_D3D12_GetCapabilityParameters(NVNGXParameter **out_params) {
  *out_params = CapabilityParameters(true);
  return NVNGX_RESULT_OK;
}

NVNGX_EXPORT NVNGX_RESULT NVSDK_NGX_D3D12_GetParameters(NVNGXParameter **out_params) {
  return NVSDK_NGX_D3D12_GetCapabilityParameters(out_params);
}

NVNGX_EXPORT NVNGX_RESULT NVSDK_NGX_D3D12_GetFeatureRequirements(IDXGIAdapter *adapter,
                                                                 const NVNGX_FeatureDiscoveryInfo *discovery_info,
                                                                 NVNGX_FeatureRequirement *requirement) {
  return FeatureRequirements(discovery_info, requirement, true);
}

NVNGX_EXPORT NVNGX_RESULT NVSDK_NGX_D3D12_CreateFeature(ID3D12GraphicsCommandList *list, unsigned int feature,
                                                        NVNGXParameter *params, unsigned int **out_handle) {
  auto parameters = static_cast<ParametersImpl *>(params);
  MetalFXRef metalfx(list);
  if (!metalfx.iface || !out_handle || !parameters)
    return NVNGX_RESULT_INVALID_PARAMETER;
  if (feature == NVNGX_FEATURE_SUPERSAMPLING) {
    if (!metalfx.iface->lpVtbl->SupportsTemporalUpscale(metalfx.iface))
      return NVNGX_RESULT_FEATURE_NOT_SUPPORTED;
    auto dlss = std::make_unique<DLSSFeature>();
    dlss->feature = NVNGX_FEATURE_SUPERSAMPLING;
    if (NVNGX_FAILED(ReadCreate(parameters, *dlss)))
      return NVNGX_RESULT_INVALID_PARAMETER;
    lane::log("info", "Super Resolution %ux%u to %ux%u on MetalFX's temporal scaler", dlss->width, dlss->height,
              dlss->target_width, dlss->target_height);
    *out_handle = &dlss.release()->handle;
    return NVNGX_RESULT_OK;
  }
  if (feature == NVNGX_FEATURE_RAYRECONSTRUCTION) {
    if (!metalfx.iface->lpVtbl->SupportsDenoisedUpscale(metalfx.iface) || !HasFeatureLibrary(L"nvngx_dlssd.dll"))
      return NVNGX_RESULT_FEATURE_NOT_SUPPORTED;
    auto dlssd = std::make_unique<DLSSDFeature>();
    dlssd->feature = NVNGX_FEATURE_RAYRECONSTRUCTION;
    if (NVNGX_FAILED(ReadCreate(parameters, *dlssd)))
      return NVNGX_RESULT_INVALID_PARAMETER;
    dlssd->roughness_mode = GetOr(parameters, NVNGX_Parameter_DLSS_Roughness_Mode, 0u);
    dlssd->hw_depth = GetOr(parameters, NVNGX_Parameter_Use_HW_Depth, 1u);
    if (!dlssd->hw_depth)
      lane::log("warn", "Ray Reconstruction with linear depth, which MetalFX reads as device depth");
    lane::log("info", "Ray Reconstruction %ux%u to %ux%u on MetalFX's denoised scaler", dlssd->width, dlssd->height,
              dlssd->target_width, dlssd->target_height);
    *out_handle = &dlssd.release()->handle;
    return NVNGX_RESULT_OK;
  }
  if (feature == NVNGX_FEATURE_FRAMEGENERATION) {
    if (!metalfx.iface->lpVtbl->SupportsFrameInterpolation(metalfx.iface) || !HasFeatureLibrary(L"nvngx_dlssg.dll"))
      return NVNGX_RESULT_FEATURE_NOT_SUPPORTED;
    auto dlssg = std::make_unique<DLSSGFeature>();
    dlssg->feature = NVNGX_FEATURE_FRAMEGENERATION;
    dlssg->width = GetOr(parameters, NVNGX_Parameter_Width, 0u);
    dlssg->height = GetOr(parameters, NVNGX_Parameter_Height, 0u);
    parameters->Set("DLSSG.MustCallEval", 1u);
    lane::log("info", "Frame Generation at %ux%u on MetalFX's frame interpolator", dlssg->width, dlssg->height);
    *out_handle = &dlssg.release()->handle;
    return NVNGX_RESULT_OK;
  }
  lane::log("err", "CreateFeature: feature %u is not supported", feature);
  return NVNGX_RESULT_FEATURE_NOT_SUPPORTED;
}

// the camera Frame Generation is given: near and far planes, and the vertical field of view in degrees. Streamline's
// is in radians, which some games give in degrees; without one it comes from the projection (row vectors: [1][1] is
// 1 / tan(fov / 2))
static void ReadCamera(const ParametersImpl *params, VKD3D_METALFX_FRAME_INTERPOLATE_DESC &desc) {
  desc.Near = GetOr(params, "DLSSG.CameraNear", 0.0f);
  desc.Far = GetOr(params, "DLSSG.CameraFar", 0.0f);
  float fov = GetOr(params, "DLSSG.CameraFOV", 0.0f);
  float *view_to_clip = nullptr;
  params->Get("DLSSG.CameraViewToClip", (void **)&view_to_clip);
  if (!(fov > 0.0f) && view_to_clip && view_to_clip[5] > 0.0f)
    fov = 2.0f * std::atan(1.0f / view_to_clip[5]);
  if (!(fov > 0.0f))
    fov = 1.0f;
  desc.FieldOfView = fov > 10.0f ? fov : fov * 180.0f / 3.14159265f;
  // 0 is a far plane at infinity
  if (!(desc.Far > 0.0f) || !std::isfinite(desc.Far))
    desc.Far = std::max(desc.Near, 0.1f) * 1.0e5f;
  if (!(desc.Near > 0.0f))
    desc.Near = 0.1f;
}

static NVNGX_RESULT EvaluateFrameGeneration(ID3D12GraphicsCommandList *list, ID3D12GraphicsCommandListMetalFX *metalfx,
                                            DLSSGFeature *dlssg, ParametersImpl *params) {
  params->Set("DLSSG.FlushRequired", 0u);
  // Streamline records the feature's work in a list it hands over closed unless it says it is recording
  bool recording = GetOr(params, "DLSSG.IsRecording", 0u) != 0;
  if (!recording) {
    ID3D12CommandAllocator *allocator = nullptr;
    params->Get("DLSSG.CmdAlloc", (void **)&allocator);
    if (!allocator || FAILED(list->Reset(allocator, nullptr))) {
      lane::log("err", "EvaluateFeature: Frame Generation without a command allocator");
      return NVNGX_RESULT_INVALID_PARAMETER;
    }
  }
  auto backbuffer = GetResource(params, "DLSSG.Backbuffer");
  auto real = GetResource(params, "DLSSG.OutputReal");
  NVNGX_RESULT result = NVNGX_RESULT_OK;
  if (GetOr(params, "DLSSG.EnableInterp", 0u)) {
    VKD3D_METALFX_FRAME_INTERPOLATE_DESC desc = {};
    auto hudless = GetResource(params, "DLSSG.HUDLess");
    desc.UI = GetResource(params, "DLSSG.UI");
    // the frame without its UI, and the UI on top, where the game gives both; else the frame as presented, with the
    // UI it has in it where the game gives that
    desc.Color = hudless && desc.UI ? hudless : backbuffer;
    desc.UIComposited = desc.Color == backbuffer;
    desc.Depth = GetResource(params, "DLSSG.Depth");
    desc.MotionVector = GetResource(params, "DLSSG.MVecs");
    desc.Output = GetResource(params, "DLSSG.OutputInterpolated");
    if (!desc.Color || !desc.Depth || !desc.MotionVector || !desc.Output) {
      lane::log("err", "EvaluateFeature: Frame Generation without %s%s%s%s", !desc.Color ? "color " : "",
                !desc.Depth ? "depth " : "", !desc.MotionVector ? "motion vectors " : "", !desc.Output ? "an output" : "");
      result = NVNGX_RESULT_INVALID_PARAMETER;
    } else {
      // NGX's scale takes the motion vectors to pixels, as DLSS's MV.Scale (Streamline's normalizing scale times the
      // render size)
      desc.MotionVectorScaleX = GetOr(params, "DLSSG.MvecScaleX", 1.0f);
      desc.MotionVectorScaleY = GetOr(params, "DLSSG.MvecScaleY", 1.0f);
      desc.JitterOffsetX = GetOr(params, "DLSSG.JitterOffsetX", 0.0f);
      desc.JitterOffsetY = GetOr(params, "DLSSG.JitterOffsetY", 0.0f);
      desc.DepthReversed = GetOr(params, "DLSSG.DepthInverted", 0u) != 0;
      desc.InReset = GetOr(params, "DLSSG.Reset", 0u) != 0 || !dlssg->last_counter;
      ReadCamera(params, desc);
      D3D12_RESOURCE_DESC output = desc.Output->GetDesc();
      desc.AspectRatio = GetOr(params, "DLSSG.CameraAspectRatio", 0.0f);
      if (!(desc.AspectRatio > 0.0f))
        desc.AspectRatio = float(output.Width) / float(std::max(output.Height, 1u));
      LARGE_INTEGER now, frequency;
      QueryPerformanceCounter(&now);
      QueryPerformanceFrequency(&frequency);
      desc.DeltaTime = dlssg->last_counter ? float(now.QuadPart - dlssg->last_counter) / float(frequency.QuadPart)
                                           : 1.0f / 60.0f;
      if (!(desc.DeltaTime > 0.0f) || desc.DeltaTime > 0.25f)
        desc.DeltaTime = 1.0f / 60.0f;
      dlssg->last_counter = now.QuadPart;
      if (FAILED(metalfx->lpVtbl->FrameInterpolate(metalfx, &desc, &dlssg->interpolator)))
        result = NVNGX_RESULT_FAIL;
    }
  }
  // the real frame, which Streamline presents after the interpolated one
  if (backbuffer && real && backbuffer != real)
    list->CopyResource(real, backbuffer);
  if (!recording)
    list->Close();
  return result;
}

NVNGX_EXPORT NVNGX_RESULT NVSDK_NGX_D3D12_EvaluateFeature(ID3D12GraphicsCommandList *list, const unsigned int *handle,
                                                          NVNGXParameter *params, void *callback) {
  auto parameters = static_cast<ParametersImpl *>(params);
  MetalFXRef metalfx(list);
  if (!metalfx.iface || !handle || !parameters) {
    lane::log("err", "EvaluateFeature: %s", !metalfx.iface ? "a command list without MetalFX" : "no feature");
    return NVNGX_RESULT_INVALID_PARAMETER;
  }
  auto common = static_cast<CommonFeature *>((void *)handle);
  switch (common->feature) {
  case NVNGX_FEATURE_SUPERSAMPLING: {
    auto dlss = static_cast<DLSSFeature *>(common);
    VKD3D_METALFX_TEMPORAL_DESC desc = {};
    desc.Color = GetResource(parameters, NVNGX_Parameter_Color);
    desc.Output = GetResource(parameters, NVNGX_Parameter_Output);
    desc.Depth = GetResource(parameters, NVNGX_Parameter_Depth);
    desc.MotionVector = GetResource(parameters, NVNGX_Parameter_MotionVectors);
    desc.ExposureTexture = GetResource(parameters, NVNGX_Parameter_ExposureTexture);
    if (!desc.Color || !desc.Output || !desc.Depth || !desc.MotionVector) {
      lane::log("err", "EvaluateFeature: Super Resolution without its color, output, depth or motion vectors");
      return NVNGX_RESULT_INVALID_PARAMETER;
    }
    desc.InputContentWidth = GetOr(parameters, NVNGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, 0u);
    desc.InputContentHeight = GetOr(parameters, NVNGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, 0u);
    desc.DepthReversed = bool(dlss->flag & NVNGX_DLSS_FLAG_DEPTH_INVERTED);
    desc.AutoExposure = bool(dlss->flag & NVNGX_DLSS_FLAG_AUTO_EXPOSURE);
    desc.MotionVectorInDisplayRes = !bool(dlss->flag & NVNGX_DLSS_FLAG_MV_LOWRES);
    ReadCommon(parameters, desc.MotionVectorScaleX, desc.MotionVectorScaleY, desc.JitterOffsetX, desc.JitterOffsetY,
               desc.PreExposure, desc.InReset);
    return FAILED(metalfx.iface->lpVtbl->TemporalUpscale(metalfx.iface, &desc, &dlss->scaler)) ? NVNGX_RESULT_FAIL
                                                                                                 : NVNGX_RESULT_OK;
  }
  case NVNGX_FEATURE_RAYRECONSTRUCTION: {
    auto dlssd = static_cast<DLSSDFeature *>(common);
    VKD3D_METALFX_DENOISED_DESC desc = {};
    desc.Color = GetResource(parameters, NVNGX_Parameter_Color);
    desc.Output = GetResource(parameters, NVNGX_Parameter_Output);
    desc.Depth = GetResource(parameters, NVNGX_Parameter_Depth);
    desc.MotionVector = GetResource(parameters, NVNGX_Parameter_MotionVectors);
    desc.DiffuseAlbedo = GetResource(parameters, NVNGX_Parameter_DiffuseAlbedo, NVNGX_Parameter_GBuffer_Albedo);
    desc.SpecularAlbedo = GetResource(parameters, NVNGX_Parameter_SpecularAlbedo, NVNGX_Parameter_GBuffer_Specular);
    desc.Normals = GetResource(parameters, NVNGX_Parameter_GBuffer_Normals);
    desc.Roughness = GetResource(parameters, NVNGX_Parameter_GBuffer_Roughness);
    desc.SpecularHitDistance = GetResource(parameters, NVNGX_Parameter_DLSSD_SpecularHitDistance);
    desc.ExposureTexture = GetResource(parameters, NVNGX_Parameter_ExposureTexture);
    desc.RoughnessInNormalAlpha = dlssd->roughness_mode == 1;
    if (!desc.Color || !desc.Output || !desc.Depth || !desc.MotionVector || !desc.DiffuseAlbedo ||
        !desc.SpecularAlbedo || !desc.Normals || (!desc.Roughness && !desc.RoughnessInNormalAlpha)) {
      lane::log("err", "EvaluateFeature: Ray Reconstruction without all of its inputs");
      return NVNGX_RESULT_INVALID_PARAMETER;
    }
    float *world_to_view = nullptr, *view_to_clip = nullptr;
    parameters->Get(NVNGX_Parameter_WorldToViewMatrix, (void **)&world_to_view);
    parameters->Get(NVNGX_Parameter_ViewToClipMatrix, (void **)&view_to_clip);
    if (!world_to_view || !view_to_clip) {
      lane::log("err", "EvaluateFeature: Ray Reconstruction without its view matrices");
      return NVNGX_RESULT_INVALID_PARAMETER;
    }
    memcpy(desc.WorldToView, world_to_view, sizeof(desc.WorldToView));
    memcpy(desc.ViewToClip, view_to_clip, sizeof(desc.ViewToClip));
    desc.DepthReversed = bool(dlssd->flag & NVNGX_DLSS_FLAG_DEPTH_INVERTED);
    desc.AutoExposure = bool(dlssd->flag & NVNGX_DLSS_FLAG_AUTO_EXPOSURE);
    ReadCommon(parameters, desc.MotionVectorScaleX, desc.MotionVectorScaleY, desc.JitterOffsetX, desc.JitterOffsetY,
               desc.PreExposure, desc.InReset);
    return FAILED(metalfx.iface->lpVtbl->DenoisedUpscale(metalfx.iface, &desc, &dlssd->scaler)) ? NVNGX_RESULT_FAIL
                                                                                                  : NVNGX_RESULT_OK;
  }
  case NVNGX_FEATURE_FRAMEGENERATION:
    return EvaluateFrameGeneration(list, metalfx.iface, static_cast<DLSSGFeature *>(common), parameters);
  default:
    break;
  }
  return NVNGX_RESULT_FAIL;
}

NVNGX_EXPORT NVNGX_RESULT NVSDK_NGX_D3D12_EvaluateFeature_C(ID3D12GraphicsCommandList *list,
                                                            const unsigned int *handle, NVNGXParameter *params,
                                                            void *callback) {
  return NVSDK_NGX_D3D12_EvaluateFeature(list, handle, params, callback);
}

NVNGX_EXPORT NVNGX_RESULT NVSDK_NGX_D3D12_ReleaseFeature(unsigned int *handle) {
  if (!handle)
    return NVNGX_RESULT_INVALID_PARAMETER;
  auto common = static_cast<CommonFeature *>((void *)handle);
  if (common->feature == NVNGX_FEATURE_SUPERSAMPLING || common->feature == NVNGX_FEATURE_RAYRECONSTRUCTION) {
    auto dlss = static_cast<DLSSFeature *>(common);
    if (dlss->scaler)
      dlss->scaler->lpVtbl->Destroy(dlss->scaler);
    if (common->feature == NVNGX_FEATURE_RAYRECONSTRUCTION)
      delete static_cast<DLSSDFeature *>(common);
    else
      delete dlss;
  } else if (common->feature == NVNGX_FEATURE_FRAMEGENERATION) {
    auto dlssg = static_cast<DLSSGFeature *>(common);
    if (dlssg->interpolator)
      dlssg->interpolator->lpVtbl->Destroy(dlssg->interpolator);
    delete dlssg;
  }
  return NVNGX_RESULT_OK;
}

// D3D11 (DXVK's): Super Resolution

static ID3D11VkMetalFXContext *GetMetalFX11(ID3D11DeviceContext *context) {
  ID3D11VkMetalFXContext *metalfx = nullptr;
  if (!context || FAILED(context->QueryInterface(__uuidof(ID3D11VkMetalFXContext), (void **)&metalfx)))
    return nullptr;
  return metalfx;
}

NVNGX_EXPORT NVNGX_RESULT NVSDK_NGX_D3D11_Init(unsigned long long id, const wchar_t *path, ID3D11Device *device,
                                               const void *feature_info, unsigned int sdk_version) {
  return NVNGX_RESULT_OK;
}

NVNGX_EXPORT NVNGX_RESULT NVSDK_NGX_D3D11_Init_Ext(unsigned long long id, const wchar_t *path, ID3D11Device *device,
                                                   unsigned int sdk_version, const void *feature_info) {
  return NVNGX_RESULT_OK;
}

NVNGX_EXPORT NVNGX_RESULT NVSDK_NGX_D3D11_Init_ProjectID(const char *project, unsigned int engine_type,
                                                         const char *engine_version, const wchar_t *path,
                                                         ID3D11Device *device, unsigned int sdk_version,
                                                         const void *feature_info) {
  return NVNGX_RESULT_OK;
}

NVNGX_EXPORT NVNGX_RESULT NVSDK_NGX_D3D11_Init_with_ProjectID(const char *project, unsigned int engine_type,
                                                              const char *engine_version, const wchar_t *path,
                                                              ID3D11Device *device, const void *feature_info,
                                                              unsigned int sdk_version) {
  return NVNGX_RESULT_OK;
}

NVNGX_EXPORT NVNGX_RESULT NVSDK_NGX_D3D11_Shutdown() {
  return NVNGX_RESULT_OK;
}

NVNGX_EXPORT NVNGX_RESULT NVSDK_NGX_D3D11_Shutdown1(ID3D11Device *device) {
  return NVNGX_RESULT_OK;
}

NVNGX_EXPORT NVNGX_RESULT NVSDK_NGX_D3D11_GetScratchBufferSize(unsigned int feature, const NVNGXParameter *params,
                                                               size_t *out_size) {
  if (out_size)
    *out_size = 0;
  return NVNGX_RESULT_OK;
}

NVNGX_EXPORT NVNGX_RESULT NVSDK_NGX_D3D11_AllocateParameters(NVNGXParameter **out_params) {
  *out_params = new ParametersImpl();
  return NVNGX_RESULT_OK;
}

NVNGX_EXPORT NVNGX_RESULT NVSDK_NGX_D3D11_DestroyParameters(NVNGXParameter *params) {
  delete static_cast<ParametersImpl *>(params);
  return NVNGX_RESULT_OK;
}

NVNGX_EXPORT NVNGX_RESULT NVSDK_NGX_D3D11_GetCapabilityParameters(NVNGXParameter **out_params) {
  *out_params = CapabilityParameters(false);
  return NVNGX_RESULT_OK;
}

NVNGX_EXPORT NVNGX_RESULT NVSDK_NGX_D3D11_GetParameters(NVNGXParameter **out_params) {
  return NVSDK_NGX_D3D11_GetCapabilityParameters(out_params);
}

NVNGX_EXPORT NVNGX_RESULT NVSDK_NGX_D3D11_GetFeatureRequirements(IDXGIAdapter *adapter,
                                                                 const NVNGX_FeatureDiscoveryInfo *discovery_info,
                                                                 NVNGX_FeatureRequirement *requirement) {
  return FeatureRequirements(discovery_info, requirement, false);
}

NVNGX_EXPORT NVNGX_RESULT NVSDK_NGX_D3D11_CreateFeature(ID3D11DeviceContext *context, unsigned int feature,
                                                        NVNGXParameter *params, unsigned int **out_handle) {
  auto parameters = static_cast<ParametersImpl *>(params);
  auto metalfx = GetMetalFX11(context);
  if (!metalfx || !out_handle || !parameters) {
    if (metalfx)
      metalfx->Release();
    return NVNGX_RESULT_INVALID_PARAMETER;
  }
  bool supported = feature == NVNGX_FEATURE_SUPERSAMPLING && metalfx->SupportsTemporalUpscale();
  metalfx->Release();
  if (!supported) {
    lane::log("err", "D3D11 CreateFeature: feature %u is not supported", feature);
    return NVNGX_RESULT_FEATURE_NOT_SUPPORTED;
  }
  auto dlss = std::make_unique<DLSSFeature>();
  dlss->feature = NVNGX_FEATURE_SUPERSAMPLING;
  if (NVNGX_FAILED(ReadCreate(parameters, *dlss)))
    return NVNGX_RESULT_INVALID_PARAMETER;
  lane::log("info", "D3D11 Super Resolution %ux%u to %ux%u on MetalFX's temporal scaler", dlss->width, dlss->height,
            dlss->target_width, dlss->target_height);
  *out_handle = &dlss.release()->handle;
  return NVNGX_RESULT_OK;
}

NVNGX_EXPORT NVNGX_RESULT NVSDK_NGX_D3D11_EvaluateFeature(ID3D11DeviceContext *context, const unsigned int *handle,
                                                          NVNGXParameter *params, void *callback) {
  auto parameters = static_cast<ParametersImpl *>(params);
  auto common = static_cast<CommonFeature *>((void *)handle);
  if (!handle || !parameters || common->feature != NVNGX_FEATURE_SUPERSAMPLING)
    return NVNGX_RESULT_INVALID_PARAMETER;
  auto dlss = static_cast<DLSSFeature *>(common);
  auto metalfx = GetMetalFX11(context);
  if (!metalfx) {
    lane::log("err", "D3D11 EvaluateFeature: a context without MetalFX");
    return NVNGX_RESULT_INVALID_PARAMETER;
  }
  D3D11_VK_METALFX_TEMPORAL_DESC desc = {};
  ID3D11Resource *resource = nullptr;
  if (NVNGX_SUCCEED(parameters->Get(NVNGX_Parameter_Color, &resource)))
    desc.Color = resource;
  resource = nullptr;
  if (NVNGX_SUCCEED(parameters->Get(NVNGX_Parameter_Output, &resource)))
    desc.Output = resource;
  resource = nullptr;
  if (NVNGX_SUCCEED(parameters->Get(NVNGX_Parameter_Depth, &resource)))
    desc.Depth = resource;
  resource = nullptr;
  if (NVNGX_SUCCEED(parameters->Get(NVNGX_Parameter_MotionVectors, &resource)))
    desc.MotionVector = resource;
  resource = nullptr;
  if (NVNGX_SUCCEED(parameters->Get(NVNGX_Parameter_ExposureTexture, &resource)))
    desc.ExposureTexture = resource;
  if (!desc.Color || !desc.Output || !desc.Depth || !desc.MotionVector) {
    metalfx->Release();
    lane::log("err", "D3D11 EvaluateFeature: Super Resolution without its color, output, depth or motion vectors");
    return NVNGX_RESULT_INVALID_PARAMETER;
  }
  desc.InputContentWidth = GetOr(parameters, NVNGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, 0u);
  desc.InputContentHeight = GetOr(parameters, NVNGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, 0u);
  desc.DepthReversed = bool(dlss->flag & NVNGX_DLSS_FLAG_DEPTH_INVERTED);
  desc.AutoExposure = bool(dlss->flag & NVNGX_DLSS_FLAG_AUTO_EXPOSURE);
  desc.MotionVectorInDisplayRes = !bool(dlss->flag & NVNGX_DLSS_FLAG_MV_LOWRES);
  ReadCommon(parameters, desc.MotionVectorScaleX, desc.MotionVectorScaleY, desc.JitterOffsetX, desc.JitterOffsetY,
             desc.PreExposure, desc.InReset);
  HRESULT hr = metalfx->TemporalUpscale(&desc, &dlss->scaler11);
  metalfx->Release();
  return FAILED(hr) ? NVNGX_RESULT_FAIL : NVNGX_RESULT_OK;
}

NVNGX_EXPORT NVNGX_RESULT NVSDK_NGX_D3D11_ReleaseFeature(unsigned int *handle) {
  if (!handle)
    return NVNGX_RESULT_INVALID_PARAMETER;
  auto common = static_cast<CommonFeature *>((void *)handle);
  if (common->feature == NVNGX_FEATURE_SUPERSAMPLING) {
    auto dlss = static_cast<DLSSFeature *>(common);
    if (dlss->scaler11)
      dlss->scaler11->Destroy();
    delete dlss;
  }
  return NVNGX_RESULT_OK;
}

// only in the programs DXVK's DXGI shows an NVIDIA GPU
extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  if (reason != DLL_PROCESS_ATTACH)
    return TRUE;
  if (!lane::shown_nvidia_gpu())
    return FALSE;
  DisableThreadLibraryCalls(instance);
  return TRUE;
}
