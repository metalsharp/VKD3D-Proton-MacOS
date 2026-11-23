#pragma once

#include <d3d11.h>

/**
 * \brief MetalSharp: MetalFX upscaling recorded in a D3D11 context
 *
 * For VKD3D-Proton-MacOS's NGX core (ngx/nvngx), which runs DLSS Super
 * Resolution on MetalFX's temporal scaler, which MoltenVK runs
 * (src/dxvk/mvk_metalfx.h). Self-contained: the NGX core carries a copy.
 */
struct D3D11_VK_METALFX_TEMPORAL_DESC {
  UINT InputContentWidth;           ///< 0: the color's width
  UINT InputContentHeight;          ///< 0: the color's height
  BOOL AutoExposure;
  BOOL InReset;
  BOOL DepthReversed;
  BOOL MotionVectorInDisplayRes;
  ID3D11Resource* Color;
  ID3D11Resource* Depth;
  ID3D11Resource* MotionVector;
  ID3D11Resource* Output;
  ID3D11Resource* ExposureTexture;  ///< optional
  FLOAT MotionVectorScaleX;
  FLOAT MotionVectorScaleY;
  FLOAT PreExposure;
  FLOAT JitterOffsetX;
  FLOAT JitterOffsetY;
};

/**
 * \brief A feature's MetalFX scaler
 *
 * Its owner destroys it when it is done with the feature.
 */
struct ID3D11VkMetalFXScaler {
  virtual void STDMETHODCALLTYPE Destroy() = 0;
};

MIDL_INTERFACE("7c24a006-aee3-4c70-8281-3d7c7f467d6c")
ID3D11VkMetalFXContext : public IUnknown {
  virtual BOOL STDMETHODCALLTYPE SupportsTemporalUpscale() = 0;

  /// *ppScaler is the feature's scaler, made at its first upscale
  virtual HRESULT STDMETHODCALLTYPE TemporalUpscale(
    const D3D11_VK_METALFX_TEMPORAL_DESC* pDesc,
          ID3D11VkMetalFXScaler**         ppScaler) = 0;
};

#ifndef _MSC_VER
__CRT_UUID_DECL(ID3D11VkMetalFXContext, 0x7c24a006,0xaee3,0x4c70,0x82,0x81,0x3d,0x7c,0x7f,0x46,0x7d,0x6c);
#endif
