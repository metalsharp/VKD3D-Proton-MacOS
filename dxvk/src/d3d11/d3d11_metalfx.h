#pragma once

#include "d3d11_metalfx_interface.h"

namespace dxvk {

  /**
   * \brief MetalSharp: a context's MetalFX upscaling (ID3D11VkMetalFXContext)
   *
   * Records MetalFX scales that MoltenVK runs (mvk_metalfx.h), for
   * VKD3D-Proton-MacOS's NGX core.
   */
  template<typename ContextType>
  class D3D11MetalFXContext : public ID3D11VkMetalFXContext {

  public:

    D3D11MetalFXContext(
            ContextType*            pContext);

    ULONG STDMETHODCALLTYPE AddRef();

    ULONG STDMETHODCALLTYPE Release();

    HRESULT STDMETHODCALLTYPE QueryInterface(
            REFIID                  riid,
            void**                  ppvObject);

    BOOL STDMETHODCALLTYPE SupportsTemporalUpscale();

    HRESULT STDMETHODCALLTYPE TemporalUpscale(
      const D3D11_VK_METALFX_TEMPORAL_DESC* pDesc,
            ID3D11VkMetalFXScaler**         ppScaler);

  private:

    ContextType* m_ctx;

  };

}
