// ReXNative renderer: the Xenos plugin's frames shown through ReXNative's own swap chain.
//
// The renderer switch (F9, the HUD's Renderer setting, rexnative.classic) hands the game to the
// Xenos plugin (ReXGlue's renderer, Xenia's). The game window used to go to ReXGlue's presenter
// with it, and under vkd3d-proton (the Steam Deck's Proton) that never worked: once one swap
// chain on the window was released, the next one's Present waited for good, so the switch stayed
// off there. Now the window keeps ReXNative's swap chain while the plugin draws. At each guest
// Swap the renderer has the Xenos driver's presenter copy its latest frame (the guest output, the
// gamma ramp applied; SDK patch, rex/xenos/draw_hook.h) into one of kTextures textures here, on
// the driver's queue, which then signals fence_. The renderer's queue waits for that value before
// the frame that shows the texture, drawn as its own front buffer is (Presenter::OnSwap: scaled,
// FSR 1, the HUD over it). The HUD so stays up whichever renderer draws.
//
// Both queues must be on one ID3D12Device. D3D12 and vkd3d-proton give one device per adapter
// (D3D12CreateDevice returns the one made for it before); where the renderer and ReXGlue picked
// two GPUs apart, Initialize says so and the window goes to ReXGlue's presenter as before.
//
// Threading: the thread that records the renderer's frames (renderer.h).
#pragma once

#include "resolve.h"  // ResolvedImage

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>

namespace rexnative {

class D3D12Device;

class ClassicFrames {
 public:
  // Whether the Xenos driver copies its frames on `device` (logged either way).
  bool Initialize(ID3D12Device* device);
  bool available() const { return fence_ != nullptr; }

  // The plugin's latest frame for the frame being recorded: copied into the next texture, the
  // renderer's queue made to wait for the copy, the texture in the front buffer's state
  // (COPY_SOURCE | shader resource). The same frame again when the driver drew none since the
  // last call. Null before its first frame or on a failure (logged).
  const ResolvedImage* Take(D3D12Device& d);
  // After the frame drew the image Take gave: the texture back in COPY_DEST, the copies' state.
  void Release(D3D12Device& d);

 private:
  // Textures of the guest output's size; the frames still running may use the old ones.
  bool CreateTextures(D3D12Device& d, uint32_t width, uint32_t height);

  static constexpr uint32_t kTextures = 3;
  static constexpr D3D12_RESOURCE_STATES kReadState =
      D3D12_RESOURCE_STATE_COPY_SOURCE | D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
  Microsoft::WRL::ComPtr<ID3D12Fence> fence_;  // the driver's queue signals each copy
  uint64_t fence_value_ = 0;
  std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kTextures> textures_;
  std::array<ResolvedImage, kTextures> images_;
  std::array<uint64_t, kTextures> used_ = {};  // the renderer's frame fence that last read each
  uint32_t width_ = 0, height_ = 0;
  uint32_t next_ = 0, current_ = kTextures;  // current_: Take's, until Release
  bool logged_ = false;
};

}  // namespace rexnative
