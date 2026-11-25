#include "dxgi_interfaces.h"
#include "dxgi_ngx_certificate.h"
#include <wincrypt.h>
#include "log/log.hpp"
#include "util_env.hpp"
#include "config/config.hpp"
#include <algorithm>
#include "winemetal.h"
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>

namespace mullion {
Logger Logger::s_instance("dxgi.log");

VendorExtension g_extension_enabled = VendorExtension::None;

#ifdef _WIN32
std::once_flag nvext_init;

// trusts the certificate Mullion's nvngx.dll is signed with, in the prefix's Root store (NGX loads only a core whose
// signature chains to a trusted root). not in DllMain: opening Wine's Root store syncs it with the host's roots, once a
// process, behind a semaphore other processes share, and crypt32 is loaded only for the processes that ask for NVEXT
static void TrustNGXCertificate() {
  HMODULE crypt32 = LoadLibraryW(L"crypt32.dll");
  if (!crypt32)
    return;
  auto open = (decltype(&CertOpenStore))GetProcAddress(crypt32, "CertOpenStore");
  auto add = (decltype(&CertAddEncodedCertificateToStore))GetProcAddress(crypt32, "CertAddEncodedCertificateToStore");
  auto close = (decltype(&CertCloseStore))GetProcAddress(crypt32, "CertCloseStore");
  if (open && add && close) {
    if (HCERTSTORE store = open(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_LOCAL_MACHINE, L"Root")) {
      if (!add(store, X509_ASN_ENCODING, mullion_ngx_certificate, sizeof(mullion_ngx_certificate),
               CERT_STORE_ADD_USE_EXISTING, nullptr))
        Logger::warn("NVEXT: failed to trust the NGX core's certificate");
      close(store, 0);
    }
  }
  FreeLibrary(crypt32);
}

// NVEXT is on with MULLION_ENABLE_NVEXT=1, or where the configuration shows the program an NVIDIA GPU
// (dxgi.customVendorId = 10de), unless MULLION_ENABLE_NVEXT=0: a launcher that rewrites the environment it starts a
// game with leaves the configuration's per-program section alone
static bool NvidiaExtensionRequested() {
  auto env = env::getEnvVar("MULLION_ENABLE_NVEXT");
  if (env == "1" || env == "0")
    return env == "1";
  auto vendor = Config::getInstance().getOption<std::string>("dxgi.customVendorId");
  std::ranges::transform(vendor, vendor.begin(), [](unsigned char c) { return std::tolower(c); });
  return vendor == "10de" || vendor == "0x10de";
}

// NVEXT: an NVIDIA GPU's driver registry values, where NGX looks for its core: NVIDIA
// Corporation\Global\NGXCore FullPath and nvlddmkm\NGXCore NGXPath, both the folder this DXGI was loaded from, which
// has Mullion's nvngx.dll. A process without it leaves them as they are: they are the prefix's, and another process's
// (Steam's, which loads this DXGI too) removal raced the game's
static void InitializeVendorExtensionNV() {
#ifdef __i386__
  return;
#endif
  if (!NvidiaExtensionRequested())
    return;
  auto name1 = L"{41FCC608-8496-4DEF-B43E-7D9BD675A6FF}";
  WCHAR folder[MAX_PATH] = L"C:\\Windows\\System32";
  HMODULE self = nullptr;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         (LPCWSTR)&InitializeVendorExtensionNV, &self)) {
    WCHAR path[MAX_PATH];
    DWORD length = GetModuleFileNameW(self, path, MAX_PATH);
    WCHAR *slash = length && length < MAX_PATH ? wcsrchr(path, L'\\') : nullptr;
    if (slash) {
      *slash = 0;
      wcscpy(folder, path);
    }
  }
  struct CloseRegKey {
    void operator()(HKEY k) const { RegCloseKey(k); }
  };
  using UniqueRegKey = std::unique_ptr<std::remove_pointer_t<HKEY>, CloseRegKey>;
  auto open_key = [](const WCHAR *subkey) {
    HKEY raw = nullptr;
    RegCreateKeyExW(HKEY_LOCAL_MACHINE, subkey, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &raw, nullptr);
    return UniqueRegKey(raw);
  };
  auto k1 = open_key(L"SOFTWARE\\NVIDIA Corporation\\Global");
  auto k2 = open_key(L"SYSTEM\\ControlSet001\\Services\\nvlddmkm");
  auto k3 = open_key(L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore");
  auto k4 = open_key(L"SYSTEM\\ControlSet001\\Services\\nvlddmkm\\NGXCore");
  if (!k1 || !k2 || !k3 || !k4)
    return;
  DWORD _1 = 1, size = DWORD((wcslen(folder) + 1) * sizeof(WCHAR));
  RegSetValueExW(k1.get(), name1, 0, REG_DWORD, (const BYTE *)&_1, sizeof(_1));
  RegSetValueExW(k2.get(), name1, 0, REG_DWORD, (const BYTE *)&_1, sizeof(_1));
  RegSetValueExW(k3.get(), L"FullPath", 0, REG_SZ, (const BYTE *)folder, size);
  RegSetValueExW(k4.get(), L"NGXPath", 0, REG_SZ, (const BYTE *)folder, size);
  Logger::info("Vendor extension enabled: NVEXT");
  g_extension_enabled = VendorExtension::Nvidia;
};

static void InitializeMetalCachePath() {
  if (env::getEnvVar("MULLION_USE_DEFAULT_METAL_CACHE") == "1")
    return;
  // This is the framework cache (mainly for PSOs), not managed by Mullion
  auto metal_cache_path = str::format("mullion/", env::getExeName(), "/com.apple.metal");
  if (!WMTSetMetalShaderCachePath(metal_cache_path.c_str())) {
    Logger::info("Failed to set Metal cache path, fallback to system default");
  }
}

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason,
                               LPVOID reserved) {
  if (reason != DLL_PROCESS_ATTACH)
    return TRUE;

  DisableThreadLibraryCalls(instance);

  InitializeMetalCachePath();
  return TRUE;
}

#endif

void InitializeVendorExtension() {
#ifdef _WIN32
  static std::once_flag once;
  std::call_once(nvext_init, InitializeVendorExtensionNV);
  if (g_extension_enabled == VendorExtension::Nvidia)
    std::call_once(once, TrustNGXCertificate);
#endif
}

extern "C" HRESULT __stdcall DXGIGetDebugInterface1(UINT Flags, REFIID riid,
                                                    void **ppDebug) {
#ifdef _WIN32
  // it's a Mullion implementation detail
  if (riid == MULLION_NVEXT_GUID) {
    std::call_once(nvext_init, InitializeVendorExtensionNV);
    return g_extension_enabled == VendorExtension::Nvidia ? S_OK : E_NOINTERFACE;
  }
#endif

  return E_NOINTERFACE;
}

extern "C" HRESULT __stdcall DXGIDeclareAdapterRemovalSupport() {
  static bool enabled = false;

  if (std::exchange(enabled, true))
    return FAIL(DXGI_ERROR_ALREADY_EXISTS);

  return S_OK;
}

} // namespace mullion