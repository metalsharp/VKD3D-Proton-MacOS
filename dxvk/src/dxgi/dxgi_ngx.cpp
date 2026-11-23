#include <algorithm>
#include <cctype>
#include <mutex>

#include "dxgi_ngx.h"
#include "dxgi_ngx_certificate.h"

#include "../util/config/config.h"
#include "../util/log/log.h"
#include "../util/util_env.h"

namespace dxvk {

  static std::once_flag s_nvextInit;


  static bool NvidiaExtensionRequested() {
    std::string vendor = Config::getUserConfig().getOption<std::string>("dxgi.customVendorId", "");
    std::transform(vendor.begin(), vendor.end(), vendor.begin(), [] (unsigned char c) { return std::tolower(c); });
    return vendor == "10de" || vendor == "0x10de";
  }


  // NGX loads only a core whose signature chains to a trusted root
  static void TrustNgxCertificate() {
    typedef void* (WINAPI *PFN_CertOpenStore)(LPCSTR, DWORD, ULONG_PTR, DWORD, const void*);
    typedef BOOL (WINAPI *PFN_CertAddEncodedCertificateToStore)(void*, DWORD, const BYTE*, DWORD, DWORD, void*);
    typedef BOOL (WINAPI *PFN_CertCloseStore)(void*, DWORD);

    HMODULE crypt32 = LoadLibraryW(L"crypt32.dll");

    if (!crypt32)
      return;

    auto open = reinterpret_cast<PFN_CertOpenStore>(GetProcAddress(crypt32, "CertOpenStore"));
    auto add = reinterpret_cast<PFN_CertAddEncodedCertificateToStore>(GetProcAddress(crypt32, "CertAddEncodedCertificateToStore"));
    auto close = reinterpret_cast<PFN_CertCloseStore>(GetProcAddress(crypt32, "CertCloseStore"));

    // CERT_STORE_PROV_SYSTEM_W, CERT_SYSTEM_STORE_LOCAL_MACHINE; X509_ASN_ENCODING, CERT_STORE_ADD_USE_EXISTING
    if (open && add && close) {
      if (void* store = open(reinterpret_cast<LPCSTR>(10), 0, 0, 2 << 16, L"Root")) {
        if (!add(store, 1, mullion_ngx_certificate, sizeof(mullion_ngx_certificate), 2, nullptr))
          Logger::warn("DLSS: Failed to trust the NGX core's certificate");
        close(store, 0);
      }
    }

    FreeLibrary(crypt32);
  }


  // The NVIDIA driver's registry values NGX looks for its core with: NVIDIA Corporation\Global\NGXCore FullPath
  // and nvlddmkm\NGXCore NGXPath, System32, whose nvngx.dll Wine loads as this lane's. A process not shown an NVIDIA
  // GPU leaves them as they are: they are the prefix's.
  static void InitNvidiaExtensionOnce() {
#ifndef _WIN64
    return;
#else
    if (!NvidiaExtensionRequested())
      return;

    WCHAR folder[MAX_PATH] = L"C:\\windows\\system32";
    UINT length = GetSystemDirectoryW(folder, MAX_PATH);

    if (!length || length >= MAX_PATH)
      wcscpy(folder, L"C:\\windows\\system32");

    static const WCHAR* const keyNames[] = {
      L"SOFTWARE\\NVIDIA Corporation\\Global",
      L"SYSTEM\\ControlSet001\\Services\\nvlddmkm",
      L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore",
      L"SYSTEM\\ControlSet001\\Services\\nvlddmkm\\NGXCore",
    };

    HKEY keys[4] = { };
    bool created = true;

    for (uint32_t i = 0; i < 4 && created; i++)
      created = !RegCreateKeyExW(HKEY_LOCAL_MACHINE, keyNames[i], 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &keys[i], nullptr);

    if (created) {
      static const WCHAR* name = L"{41FCC608-8496-4DEF-B43E-7D9BD675A6FF}";
      DWORD one = 1, size = DWORD((wcslen(folder) + 1) * sizeof(WCHAR));

      RegSetValueExW(keys[0], name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
      RegSetValueExW(keys[1], name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
      RegSetValueExW(keys[2], L"FullPath", 0, REG_SZ, reinterpret_cast<const BYTE*>(folder), size);
      RegSetValueExW(keys[3], L"NGXPath", 0, REG_SZ, reinterpret_cast<const BYTE*>(folder), size);
      TrustNgxCertificate();

      Logger::info("DLSS: NGX core registered");
    }

    for (uint32_t i = 0; i < 4; i++) {
      if (keys[i])
        RegCloseKey(keys[i]);
    }
#endif
  }


  void InitNvidiaExtension() {
    std::call_once(s_nvextInit, InitNvidiaExtensionOnce);
  }


}
