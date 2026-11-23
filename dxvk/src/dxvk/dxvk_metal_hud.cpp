#include <version.h>

#include "dxvk_metal_hud.h"

#include "../util/log/log.h"
#include "../util/util_string.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace dxvk {

  void reportMetalHud(const std::string& api) {
#ifdef _WIN32
    // On MetalSharp Wine, Mullion's winemetal.dll (ARM64X, so x86_64 programs load it too) adds labels
    // to the Metal HUD; VKD3D-Proton and Mullion head the HUD the same way. Elsewhere there is no such
    // DLL and nothing is shown.
    static dxvk::mutex s_mutex;
    static bool s_added = false;

    std::lock_guard<dxvk::mutex> lock(s_mutex);

    HMODULE winemetal = LoadLibraryA("winemetal.dll");

    if (!winemetal)
      return;

    auto stringAllocInit = reinterpret_cast<uint64_t (*)(const char*, unsigned int)>(
      reinterpret_cast<void*>(GetProcAddress(winemetal, "NSString_alloc_init")));
    auto hudUpdateLabel = reinterpret_cast<void (*)(uint64_t, uint64_t, uint64_t)>(
      reinterpret_cast<void*>(GetProcAddress(winemetal, "DeveloperHUDProperties_updateLabel")));
    auto hudAddLabel = reinterpret_cast<bool (*)(uint64_t, uint64_t, uint64_t)>(
      reinterpret_cast<void*>(GetProcAddress(winemetal, "DeveloperHUDProperties_addLabel")));
    auto objectRelease = reinterpret_cast<void (*)(uint64_t)>(
      reinterpret_cast<void*>(GetProcAddress(winemetal, "NSObject_release")));
    auto hudInstance = reinterpret_cast<uint64_t (*)()>(
      reinterpret_cast<void*>(GetProcAddress(winemetal, "DeveloperHUDProperties_instance")));

    uint64_t hud = 0;

    if (!stringAllocInit || !hudUpdateLabel || !hudAddLabel || !objectRelease || !hudInstance || !(hud = hudInstance()))
      return;

    // 4 is NSUTF8StringEncoding. The label is added once and its text updated by each swap chain, so the
    // HUD shows the API of the swap chain created last (D3D9 then D3D11 in one process, for example).
    static uint64_t s_label = stringAllocInit("com.github.metalsharp.dxvk-version", 4);
    std::string text = str::format("DXVK ", api, " ", DXVK_VERSION);

    if (!std::exchange(s_added, true)) {
      uint64_t after = stringAllocInit("com.apple.hud-graph.default", 4);
      hudAddLabel(hud, s_label, after);
      objectRelease(after);
    }

    uint64_t value = stringAllocInit(text.c_str(), 4);
    hudUpdateLabel(hud, s_label, value);
    objectRelease(value);
#endif
  }

}
