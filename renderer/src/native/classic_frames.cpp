// ReXNative renderer: the Xenos plugin's frames shown through ReXNative's own swap chain
// (classic_frames.h).
#include "classic_frames.h"

#include "d3d12_device.h"
#include "log.h"

#include <rex/logging.h>
#include <rex/xenos/draw_hook.h>

namespace rexnative {

using Microsoft::WRL::ComPtr;

namespace {

// The object's identity: two interface pointers of one object can differ, its IUnknown cannot.
IUnknown* Identity(IUnknown* object) {
  IUnknown* unknown = nullptr;
  if (!object || FAILED(object->QueryInterface(IID_PPV_ARGS(&unknown)))) return nullptr;
  unknown->Release();  // only compared, never used
  return unknown;
}

}  // namespace

bool ClassicFrames::Initialize(ID3D12Device* device) {
  auto* source = static_cast<ID3D12Device*>(rex::xenos::GetGuestOutputDevice());
  if (!source) {
    REXLOG_WARN(
        "ReXNative: the Xenos driver gives no frames to show (D3D12 presenter and sdk/patches "
        "needed): the renderer switch gives the window to ReXGlue's presenter");
    return false;
  }
  if (Identity(source) != Identity(device)) {
    REXLOG_WARN(
        "ReXNative: ReXGlue's D3D12 device is not the renderer's (two GPUs?): the renderer "
        "switch gives the window to ReXGlue's presenter");
    return false;
  }
  if (Failed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)),
             "classic frames fence"))
    return false;
  REXLOG_INFO("ReXNative: the Xenos plugin's frames go through ReXNative's swap chain");
  return true;
}

bool ClassicFrames::CreateTextures(D3D12Device& d, uint32_t width, uint32_t height) {
  d.WaitIdle();
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;  // the guest output's (D3D12Presenter)
  desc.SampleDesc.Count = 1;
  D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
  for (uint32_t i = 0; i < kTextures; ++i) {
    textures_[i].Reset();
    if (Failed(d.device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&textures_[i])),
               "classic frame texture")) {
      for (auto& t : textures_) t.Reset();
      width_ = height_ = 0;
      return false;
    }
    textures_[i]->SetName(L"ReXNative classic frame");
    used_[i] = 0;
  }
  width_ = width;
  height_ = height;
  return true;
}

const ResolvedImage* ClassicFrames::Take(D3D12Device& d) {
  if (!available()) return nullptr;
  const uint32_t i = next_;
  // The texture's last frame has finished reading it before the driver's queue writes it.
  if (used_[i] > d.completed_fence()) d.WaitFence(used_[i]);
  rex::xenos::GuestOutputInfo info;
  const auto copy = [&] {
    rex::xenos::GuestOutputCopy request;
    request.dest = textures_[i].Get();
    request.dest_width = width_;
    request.dest_height = height_;
    request.fence = fence_.Get();
    request.fence_value = fence_value_ + 1;
    return rex::xenos::CopyGuestOutput(request, &info);
  };
  // Without textures yet the call only gives the size.
  bool copied = copy();
  if (!copied && info.width && (info.width > width_ || info.height > height_)) {
    if (!CreateTextures(d, info.width, info.height)) return nullptr;
    copied = copy();
  }
  if (!copied) return nullptr;  // no frame from the driver yet
  ++fence_value_;
  // The queue waits for the copy before this frame's list, which the stream submits after.
  ID3D12CommandQueue* queue = d.command_queue();
  ID3D12Fence* fence = fence_.Get();
  const uint64_t value = fence_value_;
  CommandStream* list = d.command_list();
  list->Call([queue, fence, value](ID3D12GraphicsCommandList*) { queue->Wait(fence, value); });
  const D3D12_RESOURCE_BARRIER b =
      TransitionBarrier(textures_[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST, kReadState);
  list->ResourceBarrier(1, &b);
  ResolvedImage& image = images_[i];
  image = {};
  image.format = rex::graphics::xenos::TextureFormat::k_2_10_10_10;
  image.width = info.width;
  image.height = info.height;
  image.texture = textures_[i].Get();
  image.seq = info.serial;
  used_[i] = d.frame_fence();
  current_ = i;
  next_ = (i + 1) % kTextures;
  if (!logged_) {
    logged_ = true;
    REXLOG_INFO("ReXNative: showing the Xenos plugin's {}x{} frames", info.width, info.height);
  }
  return &image;
}

void ClassicFrames::Release(D3D12Device& d) {
  if (current_ >= kTextures) return;
  const D3D12_RESOURCE_BARRIER b =
      TransitionBarrier(textures_[current_].Get(), kReadState, D3D12_RESOURCE_STATE_COPY_DEST);
  d.command_list()->ResourceBarrier(1, &b);
  current_ = kTextures;
}

}  // namespace rexnative
