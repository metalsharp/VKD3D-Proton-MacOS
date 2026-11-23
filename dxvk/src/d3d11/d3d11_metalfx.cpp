#include <atomic>
#include <cstring>
#include <mutex>

#include "d3d11_context_imm.h"
#include "d3d11_context_def.h"
#include "d3d11_device.h"
#include "d3d11_metalfx.h"
#include "d3d11_texture.h"

#include "../dxvk/mvk_metalfx.h"

namespace dxvk {

  /**
   * \brief A feature's scaler: MoltenVK's, under an id of ours
   *
   * Destroy() can come after the device is gone, so the ids it releases
   * wait here for any context's next scale to pass on to MoltenVK.
   */
  class D3D11VkMetalFXScaler : public ID3D11VkMetalFXScaler {

  public:

    explicit D3D11VkMetalFXScaler(uint64_t Id)
    : m_id(Id) { }

    void STDMETHODCALLTYPE Destroy() {
      std::lock_guard lock(s_releaseLock);
      s_released.push_back(m_id);
      delete this;
    }

    uint64_t id() const {
      return m_id;
    }

    static uint64_t nextId() {
      return ++s_nextId;
    }

    static std::vector<uint64_t> takeReleased() {
      std::lock_guard lock(s_releaseLock);
      return std::exchange(s_released, std::vector<uint64_t>());
    }

  private:

    uint64_t m_id;

    static std::mutex                   s_releaseLock;
    static std::vector<uint64_t>        s_released;
    static std::atomic<uint64_t>        s_nextId;

  };

  std::mutex             D3D11VkMetalFXScaler::s_releaseLock;
  std::vector<uint64_t>  D3D11VkMetalFXScaler::s_released;
  std::atomic<uint64_t>  D3D11VkMetalFXScaler::s_nextId = { 0u };


  // Whether MoltenVK's driverInfo has the word (mvk_metalfx.h)
  static bool HasDriverInfoWord(const DxvkDevice& device, const char* word) {
    const auto& props = device.properties().vk12;

    if (props.driverID != VK_DRIVER_ID_MOLTENVK)
      return false;

    size_t length = std::strlen(word);

    for (const char* found = std::strstr(props.driverInfo, word); found; found = std::strstr(found + 1, word)) {
      if ((found == props.driverInfo || found[-1] == ' ') && (!found[length] || found[length] == ' '))
        return true;
    }

    return false;
  }


  // The format MetalFX reads an image as: a float for an integer-typed one
  // (a typeless depth or exposure), and a float pair or quad for motion
  // vectors; VK_FORMAT_UNDEFINED for the image's own
  static VkFormat MetalFXReadFormat(VkFormat format, bool motion) {
    switch (format) {
      case VK_FORMAT_R32_UINT:
      case VK_FORMAT_R32_SINT:
        return VK_FORMAT_R32_SFLOAT;
      case VK_FORMAT_R16_UINT:
      case VK_FORMAT_R16_SINT:
        return VK_FORMAT_R16_SFLOAT;
      case VK_FORMAT_R32G32_UINT:
      case VK_FORMAT_R32G32_SINT:
        return VK_FORMAT_R32G32_SFLOAT;
      case VK_FORMAT_R16G16_UINT:
      case VK_FORMAT_R16G16_SINT:
      case VK_FORMAT_R16G16_UNORM:
      case VK_FORMAT_R16G16_SNORM:
        return motion ? VK_FORMAT_R16G16_SFLOAT : VK_FORMAT_UNDEFINED;
      case VK_FORMAT_R16G16B16A16_UINT:
      case VK_FORMAT_R16G16B16A16_SINT:
      case VK_FORMAT_R16G16B16A16_UNORM:
      case VK_FORMAT_R16G16B16A16_SNORM:
        return motion ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_UNDEFINED;
      default:
        return VK_FORMAT_UNDEFINED;
    }
  }


  template<typename ContextType>
  D3D11MetalFXContext<ContextType>::D3D11MetalFXContext(
          ContextType*          pContext)
  : m_ctx(pContext) {

  }


  template<typename ContextType>
  ULONG STDMETHODCALLTYPE D3D11MetalFXContext<ContextType>::AddRef() {
    return m_ctx->AddRef();
  }


  template<typename ContextType>
  ULONG STDMETHODCALLTYPE D3D11MetalFXContext<ContextType>::Release() {
    return m_ctx->Release();
  }


  template<typename ContextType>
  HRESULT STDMETHODCALLTYPE D3D11MetalFXContext<ContextType>::QueryInterface(
          REFIID                  riid,
          void**                  ppvObject) {
    return m_ctx->QueryInterface(riid, ppvObject);
  }


  template<typename ContextType>
  BOOL STDMETHODCALLTYPE D3D11MetalFXContext<ContextType>::SupportsTemporalUpscale() {
    return HasDriverInfoWord(*m_ctx->m_device, VK_MVK_METALFX_DRIVER_INFO);
  }


  template<typename ContextType>
  HRESULT STDMETHODCALLTYPE D3D11MetalFXContext<ContextType>::TemporalUpscale(
    const D3D11_VK_METALFX_TEMPORAL_DESC* pDesc,
          ID3D11VkMetalFXScaler**         ppScaler) {
    if (!pDesc || !ppScaler || !pDesc->Color || !pDesc->Depth || !pDesc->MotionVector || !pDesc->Output)
      return E_INVALIDARG;

    if (!SupportsTemporalUpscale())
      return E_NOTIMPL;

    std::array<Rc<DxvkImage>, VK_METALFX_IMAGE_COUNT_MVK> images = { };
    ID3D11Resource* resources[VK_METALFX_IMAGE_COUNT_MVK] = { };
    resources[VK_METALFX_IMAGE_COLOR_MVK] = pDesc->Color;
    resources[VK_METALFX_IMAGE_DEPTH_MVK] = pDesc->Depth;
    resources[VK_METALFX_IMAGE_MOTION_MVK] = pDesc->MotionVector;
    resources[VK_METALFX_IMAGE_OUTPUT_MVK] = pDesc->Output;
    resources[VK_METALFX_IMAGE_EXPOSURE_MVK] = pDesc->ExposureTexture;

    VkMetalFXScaleInfoMVK info = { };
    info.sType = VK_STRUCTURE_TYPE_METALFX_SCALE_INFO_MVK;

    for (uint32_t i = 0; i < VK_METALFX_IMAGE_COUNT_MVK; i++) {
      if (!resources[i])
        continue;

      D3D11CommonTexture* texture = GetCommonTexture(resources[i]);

      if (!texture)
        return E_INVALIDARG;

      images[i] = texture->GetImage();
      info.formats[i] = MetalFXReadFormat(images[i]->info().format, i == VK_METALFX_IMAGE_MOTION_MVK);
    }

    if (!*ppScaler)
      *ppScaler = new D3D11VkMetalFXScaler(D3D11VkMetalFXScaler::nextId());

    info.scalerId = static_cast<D3D11VkMetalFXScaler*>(*ppScaler)->id();
    info.scalerType = VK_METALFX_SCALER_TYPE_TEMPORAL_MVK;
    info.flags = (pDesc->AutoExposure ? VK_METALFX_SCALE_AUTO_EXPOSURE_BIT_MVK : 0u)
               | (pDesc->InReset ? VK_METALFX_SCALE_RESET_BIT_MVK : 0u)
               | (pDesc->DepthReversed ? VK_METALFX_SCALE_DEPTH_REVERSED_BIT_MVK : 0u)
               // MoltenVK brings them to the input resolution
               | (pDesc->MotionVectorInDisplayRes ? VK_METALFX_SCALE_MOTION_DISPLAY_RES_BIT_MVK : 0u);
    info.inputContentWidth = pDesc->InputContentWidth;
    info.inputContentHeight = pDesc->InputContentHeight;
    info.motionVectorScaleX = pDesc->MotionVectorScaleX;
    info.motionVectorScaleY = pDesc->MotionVectorScaleY;
    info.jitterOffsetX = pDesc->JitterOffsetX;
    info.jitterOffsetY = pDesc->JitterOffsetY;
    info.preExposure = pDesc->PreExposure;

    D3D10DeviceLock lock = m_ctx->LockContext();

    m_ctx->EmitCs([
      cImages   = std::move(images),
      cInfo     = info,
      cReleased = D3D11VkMetalFXScaler::takeReleased()
    ] (DxvkContext* ctx) {
      ctx->metalFXScale(cImages.data(), cInfo, cReleased.size(), cReleased.data());
    });

    return S_OK;
  }


  template class D3D11MetalFXContext<D3D11DeferredContext>;
  template class D3D11MetalFXContext<D3D11ImmediateContext>;

}
