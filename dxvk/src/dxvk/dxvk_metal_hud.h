#pragma once

#include <string>

#include "../util/thread.h"

namespace dxvk {

  /**
   * \brief Shows the API in the Metal HUD
   *
   * Sets a "DXVK <api> <version>" line in the Metal performance HUD through
   * MetalSharp Wine's winemetal.dll, next to the lines VKD3D-Proton and Mullion
   * show. Does nothing where that DLL is missing.
   * \param [in] api The API and feature level, e.g. "D3D11 FL_11_1"
   */
  void reportMetalHud(const std::string& api);

}
