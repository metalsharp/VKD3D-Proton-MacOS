// DXVK's ID3D11VkExtContext (DXVK's src/d3d11/d3d11_interfaces.h, zlib license), which NVAPI's D3D11 extensions
// use on this lane, as DXVK-NVAPI does
#pragma once

#include <d3d11.h>

enum D3D11_VK_BARRIER_CONTROL : uint32_t {
  D3D11_VK_BARRIER_CONTROL_IGNORE_WRITE_AFTER_WRITE = 1 << 0,
};

MIDL_INTERFACE("fd0bca13-5cb6-4c3a-987e-4750de2ca791")
ID3D11VkExtContext : public IUnknown {
  virtual void STDMETHODCALLTYPE MultiDrawIndirect(UINT DrawCount, ID3D11Buffer *pBufferForArgs, UINT ByteOffsetForArgs,
                                                   UINT ByteStrideForArgs) = 0;
  virtual void STDMETHODCALLTYPE MultiDrawIndexedIndirect(UINT DrawCount, ID3D11Buffer *pBufferForArgs,
                                                          UINT ByteOffsetForArgs, UINT ByteStrideForArgs) = 0;
  virtual void STDMETHODCALLTYPE MultiDrawIndirectCount(UINT MaxDrawCount, ID3D11Buffer *pBufferForCount,
                                                        UINT ByteOffsetForCount, ID3D11Buffer *pBufferForArgs,
                                                        UINT ByteOffsetForArgs, UINT ByteStrideForArgs) = 0;
  virtual void STDMETHODCALLTYPE MultiDrawIndexedIndirectCount(UINT MaxDrawCount, ID3D11Buffer *pBufferForCount,
                                                               UINT ByteOffsetForCount, ID3D11Buffer *pBufferForArgs,
                                                               UINT ByteOffsetForArgs, UINT ByteStrideForArgs) = 0;
  virtual void STDMETHODCALLTYPE SetDepthBoundsTest(BOOL Enable, FLOAT MinDepthBounds, FLOAT MaxDepthBounds) = 0;
  virtual void STDMETHODCALLTYPE SetBarrierControl(UINT ControlFlags) = 0;
};

#ifndef _MSC_VER
__CRT_UUID_DECL(ID3D11VkExtContext, 0xfd0bca13,0x5cb6,0x4c3a,0x98,0x7e,0x47,0x50,0xde,0x2c,0xa7,0x91);
#endif
