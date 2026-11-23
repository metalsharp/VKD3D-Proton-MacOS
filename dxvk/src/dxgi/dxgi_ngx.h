#pragma once

#include "dxgi_include.h"

namespace dxvk {

  /**
   * \brief MetalSharp: DLSS's setup for VKD3D-Proton-MacOS's NGX core
   *
   * Where the program is shown an NVIDIA GPU (dxgi.customVendorId = 10de), sets
   * the NVIDIA driver's registry values NGX finds its core by (System32, where
   * Wine finds this lane's nvngx.dll) and trusts the certificate NGX checks the
   * core's file in System32 against, in the prefix's Root store.
   */
  void InitNvidiaExtension();

}
