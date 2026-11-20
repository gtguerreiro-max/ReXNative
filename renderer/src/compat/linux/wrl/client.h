// Microsoft::WRL::ComPtr for the Linux build (D3D12 from vkd3d-proton, DXGI from DXVK Native):
// the part of WRL's <wrl/client.h> the renderer uses. &ptr gives a ComPtrRef, as WRL's does: it
// releases what the pointer held and converts to T**, void** or ComPtr<T>* for an out argument.
#pragma once

#include <windows.h>
#include <unknwn.h>

#include <cstddef>
#include <type_traits>
#include <utility>

namespace Microsoft::WRL {

template <typename T>
class ComPtr;

namespace Details {

template <typename P>
class ComPtrRef {
 public:
  using InterfaceType = typename P::InterfaceType;
  explicit ComPtrRef(P* ptr) noexcept : ptr_(ptr) {}
  operator P*() const noexcept { return ptr_; }
  operator InterfaceType**() const noexcept { return ptr_->ReleaseAndGetAddressOf(); }
  operator void**() const noexcept { return reinterpret_cast<void**>(ptr_->ReleaseAndGetAddressOf()); }
  operator IUnknown**() const noexcept
    requires(!std::is_same_v<InterfaceType, IUnknown>)
  {
    return reinterpret_cast<IUnknown**>(ptr_->ReleaseAndGetAddressOf());
  }
  InterfaceType* operator*() const noexcept { return ptr_->Get(); }
  InterfaceType* const* GetAddressOf() const noexcept { return ptr_->GetAddressOf(); }
  InterfaceType** ReleaseAndGetAddressOf() const noexcept { return ptr_->ReleaseAndGetAddressOf(); }
  P* Ref() const noexcept { return ptr_; }

 private:
  P* ptr_;
};

}  // namespace Details

template <typename T>
class ComPtr {
 public:
  using InterfaceType = T;

  ComPtr() noexcept = default;
  ComPtr(std::nullptr_t) noexcept {}
  template <typename U>
  ComPtr(U* p) noexcept : ptr_(p) {
    InternalAddRef();
  }
  ComPtr(const ComPtr& o) noexcept : ptr_(o.ptr_) { InternalAddRef(); }
  template <typename U>
  ComPtr(const ComPtr<U>& o) noexcept : ptr_(o.Get()) {
    InternalAddRef();
  }
  ComPtr(ComPtr&& o) noexcept : ptr_(o.ptr_) { o.ptr_ = nullptr; }
  template <typename U>
  ComPtr(ComPtr<U>&& o) noexcept : ptr_(o.Detach()) {}
  ~ComPtr() { InternalRelease(); }

  ComPtr& operator=(std::nullptr_t) noexcept {
    InternalRelease();
    return *this;
  }
  template <typename U>
  ComPtr& operator=(U* p) noexcept {
    if (ptr_ != p) ComPtr(p).Swap(*this);
    return *this;
  }
  ComPtr& operator=(const ComPtr& o) noexcept {
    if (ptr_ != o.ptr_) ComPtr(o).Swap(*this);
    return *this;
  }
  template <typename U>
  ComPtr& operator=(const ComPtr<U>& o) noexcept {
    ComPtr(o).Swap(*this);
    return *this;
  }
  ComPtr& operator=(ComPtr&& o) noexcept {
    ComPtr(std::move(o)).Swap(*this);
    return *this;
  }
  template <typename U>
  ComPtr& operator=(ComPtr<U>&& o) noexcept {
    ComPtr(std::move(o)).Swap(*this);
    return *this;
  }

  void Swap(ComPtr& o) noexcept { std::swap(ptr_, o.ptr_); }
  void Swap(ComPtr&& o) noexcept { std::swap(ptr_, o.ptr_); }

  T* Get() const noexcept { return ptr_; }
  T* operator->() const noexcept { return ptr_; }
  explicit operator bool() const noexcept { return ptr_ != nullptr; }
  Details::ComPtrRef<ComPtr> operator&() noexcept { return Details::ComPtrRef<ComPtr>(this); }
  const ComPtr* operator&() const noexcept { return this; }

  T* const* GetAddressOf() const noexcept { return &ptr_; }
  T** GetAddressOf() noexcept { return &ptr_; }
  T** ReleaseAndGetAddressOf() noexcept {
    InternalRelease();
    return &ptr_;
  }
  T* Detach() noexcept {
    T* p = ptr_;
    ptr_ = nullptr;
    return p;
  }
  void Attach(T* p) noexcept {
    InternalRelease();
    ptr_ = p;
  }
  unsigned long Reset() noexcept { return InternalRelease(); }

  template <typename U>
  HRESULT As(ComPtr<U>* out) const noexcept {
    return ptr_->QueryInterface(__uuidof(U), reinterpret_cast<void**>(out->ReleaseAndGetAddressOf()));
  }
  template <typename U>
  HRESULT As(Details::ComPtrRef<ComPtr<U>> out) const noexcept {
    return As(out.Ref());
  }
  HRESULT AsIID(REFIID iid, ComPtr<IUnknown>* out) const noexcept {
    return ptr_->QueryInterface(iid, reinterpret_cast<void**>(out->ReleaseAndGetAddressOf()));
  }
  HRESULT CopyTo(T** out) const noexcept {
    InternalAddRef();
    *out = ptr_;
    return S_OK;
  }
  template <typename U>
  HRESULT CopyTo(U** out) const noexcept {
    return ptr_->QueryInterface(__uuidof(U), reinterpret_cast<void**>(out));
  }

 private:
  void InternalAddRef() const noexcept {
    if (ptr_) ptr_->AddRef();
  }
  unsigned long InternalRelease() noexcept {
    unsigned long r = 0;
    if (T* p = ptr_) {
      ptr_ = nullptr;
      r = p->Release();
    }
    return r;
  }

  T* ptr_ = nullptr;
};

template <typename T, typename U>
bool operator==(const ComPtr<T>& a, const ComPtr<U>& b) noexcept {
  return a.Get() == b.Get();
}
template <typename T>
bool operator==(const ComPtr<T>& a, std::nullptr_t) noexcept {
  return a.Get() == nullptr;
}

}  // namespace Microsoft::WRL

// IID_PPV_ARGS for a T** and for &comptr (a ComPtrRef), as on Windows.
namespace rexnative_compat {
template <typename T>
inline GUID Iid(T**) {
  return __uuidof(T);
}
template <typename T>
inline void** Ppv(T** p) {
  return reinterpret_cast<void**>(p);
}
template <typename P>
inline GUID Iid(const Microsoft::WRL::Details::ComPtrRef<P>&) {
  return __uuidof(typename P::InterfaceType);
}
template <typename P>
inline void** Ppv(const Microsoft::WRL::Details::ComPtrRef<P>& r) {
  return reinterpret_cast<void**>(r.ReleaseAndGetAddressOf());
}
template <typename T>
inline GUID Iid(Microsoft::WRL::ComPtr<T>*) {
  return __uuidof(T);
}
template <typename T>
inline void** Ppv(Microsoft::WRL::ComPtr<T>* p) {
  return reinterpret_cast<void**>(p->ReleaseAndGetAddressOf());
}
}  // namespace rexnative_compat
#undef IID_PPV_ARGS
#define IID_PPV_ARGS(pp) ::rexnative_compat::Iid(pp), ::rexnative_compat::Ppv(pp)
