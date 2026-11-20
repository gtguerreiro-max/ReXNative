// The Linux build's D3DCompile (d3d12_device.h D3DCompiler()): WineHQ vkd3d's, from
// libvkd3d-utils.so.1 in the game's lib/, which compiles HLSL to DXBC as d3dcompiler_47 does.
// That library keeps the Windows calling convention on x86-64 Linux (ms_abi) for D3DCompile and
// for its blobs' methods, unlike vkd3d-proton's native build and this program: it is called
// through ms_abi declarations, and its blobs are copied into blobs of this program's own.
#include "rexnative_win32.h"

#include <d3dcommon.h>
#include <d3dcompiler.h>

#include <dlfcn.h>

#include <atomic>
#include <cstring>
#include <vector>

#include <rex/logging.h>

namespace rexnative {

namespace {

#define MSABI __attribute__((ms_abi))

struct MsBlob;
struct MsBlobVtbl {
  HRESULT(MSABI* QueryInterface)(MsBlob*, REFIID, void**);
  ULONG(MSABI* AddRef)(MsBlob*);
  ULONG(MSABI* Release)(MsBlob*);
  void*(MSABI* GetBufferPointer)(MsBlob*);
  SIZE_T(MSABI* GetBufferSize)(MsBlob*);
};
struct MsBlob {
  const MsBlobVtbl* vtbl;
};

using MsD3DCompile = HRESULT(MSABI*)(const void* data, SIZE_T size, const char* name,
                                     const D3D_SHADER_MACRO* defines, void* include,
                                     const char* entry, const char* target, UINT flags1,
                                     UINT flags2, MsBlob** code, MsBlob** errors);

class Blob final : public ID3DBlob {
 public:
  explicit Blob(std::vector<uint8_t> data) : data_(std::move(data)) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (iid == __uuidof(IUnknown) || iid == __uuidof(ID3D10Blob)) {
      AddRef();
      *out = this;
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG r = --refs_;
    if (!r) delete this;
    return r;
  }
  LPVOID STDMETHODCALLTYPE GetBufferPointer() override { return data_.data(); }
  SIZE_T STDMETHODCALLTYPE GetBufferSize() override { return data_.size(); }

 private:
  std::vector<uint8_t> data_;
  std::atomic<ULONG> refs_{1};
};

// A copy of a vkd3d blob, which is released.
ID3DBlob* Copy(MsBlob* blob) {
  if (!blob) return nullptr;
  const auto* p = static_cast<const uint8_t*>(blob->vtbl->GetBufferPointer(blob));
  const SIZE_T n = blob->vtbl->GetBufferSize(blob);
  std::vector<uint8_t> data(p, p + n);
  blob->vtbl->Release(blob);
  return new Blob(std::move(data));
}

MsD3DCompile Vkd3dCompile() {
  static const MsD3DCompile fn = [] {
    void* lib = dlopen("libvkd3d-utils.so.1", RTLD_NOW | RTLD_LOCAL);
    auto f = lib ? reinterpret_cast<MsD3DCompile>(dlsym(lib, "D3DCompile")) : nullptr;
    if (!f) REXLOG_ERROR("ReXNative: libvkd3d-utils.so.1 / D3DCompile not found: {}", dlerror());
    return f;
  }();
  return fn;
}

}  // namespace

HRESULT WINAPI D3DCompileVkd3d(const void* data, SIZE_T size, const char* name,
                               const D3D_SHADER_MACRO* defines, ID3DInclude* include,
                               const char* entry, const char* target, UINT flags1, UINT flags2,
                               ID3DBlob** code, ID3DBlob** errors) {
  if (code) *code = nullptr;
  if (errors) *errors = nullptr;
  const MsD3DCompile compile = Vkd3dCompile();
  if (!compile) return E_FAIL;
  if (include) REXLOG_WARN("ReXNative: D3DCompile include handler ignored on Linux ({})", name);
  MsBlob* ms_code = nullptr;
  MsBlob* ms_errors = nullptr;
  const HRESULT hr = compile(data, size, name, defines, nullptr, entry, target, flags1, flags2,
                             code ? &ms_code : nullptr, errors ? &ms_errors : nullptr);
  if (code) *code = Copy(ms_code);
  if (errors) *errors = Copy(ms_errors);
  return hr;
}

}  // namespace rexnative
