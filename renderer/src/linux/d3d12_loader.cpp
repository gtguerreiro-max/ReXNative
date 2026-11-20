// The Linux build's D3D12 and DXGI entry points: vkd3d-proton's libvkd3d-proton-d3d12.so and DXVK
// Native's libdxvk_dxgi.so, loaded when first called (from LD_LIBRARY_PATH: the game's lib/).
#include "rexnative_win32.h"

#include <d3d12.h>
#include <dxgi1_6.h>

#include <dlfcn.h>

#include <cstdio>

namespace {

void* Symbol(const char* library, const char* name) {
  static void* d3d12 = nullptr;
  static void* dxgi = nullptr;
  void*& handle = library[0] == 'd' ? dxgi : d3d12;
  const char* file = library[0] == 'd' ? "libdxvk_dxgi.so.0" : "libvkd3d-proton-d3d12.so";
  if (!handle) handle = dlopen(file, RTLD_NOW | RTLD_LOCAL);
  if (!handle && library[0] == 'd') handle = dlopen("libdxvk_dxgi.so", RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    std::fprintf(stderr, "ReXNative: %s not loaded: %s\n", file, dlerror());
    return nullptr;
  }
  void* f = dlsym(handle, name);
  if (!f) std::fprintf(stderr, "ReXNative: %s has no %s\n", file, name);
  return f;
}

template <typename Fn>
Fn Get(const char* library, const char* name) {
  return reinterpret_cast<Fn>(Symbol(library, name));
}

}  // namespace

HRESULT WINAPI D3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL level, REFIID iid,
                                 void** device) {
  using Fn = HRESULT (*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
  static const Fn fn = Get<Fn>("vkd3d", "D3D12CreateDevice");
  return fn ? fn(adapter, level, iid, device) : E_NOTIMPL;
}

HRESULT WINAPI D3D12GetDebugInterface(REFIID iid, void** debug) {
  using Fn = HRESULT (*)(REFIID, void**);
  static const Fn fn = Get<Fn>("vkd3d", "D3D12GetDebugInterface");
  return fn ? fn(iid, debug) : E_NOINTERFACE;
}

HRESULT WINAPI D3D12SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC* desc,
                                           D3D_ROOT_SIGNATURE_VERSION version, ID3DBlob** blob,
                                           ID3DBlob** error) {
  using Fn = HRESULT (*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**,
                         ID3DBlob**);
  static const Fn fn = Get<Fn>("vkd3d", "D3D12SerializeRootSignature");
  return fn ? fn(desc, version, blob, error) : E_NOTIMPL;
}

HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID iid, void** factory) {
  using Fn = HRESULT (*)(UINT, REFIID, void**);
  static const Fn fn = Get<Fn>("dxgi", "CreateDXGIFactory2");
  return fn ? fn(flags, iid, factory) : E_NOTIMPL;
}
