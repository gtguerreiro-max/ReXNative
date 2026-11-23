// ReXNative renderer: guest Swap -> the debug window, and captures.
//
// OnSwap reads the front buffer texture of the guest Swap (r4, a D3DBaseTexture; fetch
// constant at +28, the same object the frame's last Resolve wrote), finds its host image in the
// Resolver (RTResolve) and draws it into the debug window's back buffer with the fetch
// constant's swizzle and the display controller's gamma ramp (present.cpp). Without a
// resolved image the back buffer is cleared to black.
//
// Captures (the image check against Xenos): --rexnative_capture_dir (empty by default: no
// requests polled) and either the file <dir>\capture.request (deleted when taken, read every 16
// frames), a mark (F12), or --rexnative_capture_frame=N. A request at Swap F-1 switches the
// Xenos plugin to full readback (HoldFullReadback in resolve.h; with "fast" guest memory holds
// the previous frame's resolves) and captures frame F (or the next frame shown, when F is held
// back):
//   f<F>-native.png  the native front buffer (read back after the frame's fence)
//   f<F>-xenos.png   the same front buffer as the Xenos plugin wrote it into guest memory
//                    (read after the guest thread was held in the Swap hook for
//                    --rexnative_capture_hold_ms so the plugin's GPU thread finishes frame
//                    F), untiled with TextureMod's UntileTexture
// and logs the PSNR (RGB, 8 bit) of the pair and appends it to <dir>\captures.csv.
// Both images are the front buffer before the presenter's gamma ramp, or with it applied as the
// window shows it with --rexnative_capture_shown (the classic renderer's frames have it already).
//
// The debug tools (PNG writer, glitch catcher, luma probe) are in present_debug.h.
//
// Threading: the renderer's ExecuteSwap only. With the render thread (renderer.h) ReadSwap,
// CaptureRequested and capture_holding_guest run on the guest thread and OnSwap/AfterEndFrame
// on the render thread; the renderer runs the Swaps of a capture synchronously (the guest
// thread waits in its Swap hook from the request until the capture was taken), so the hold and
// the guest memory read keep their meaning.
#pragma once

#include "hook_args.h"

#include <d3d12.h>

#include <cstdint>
#include <memory>

namespace rexnative {

class NativeRenderer;
struct ResolvedImage;

// --rexnative_luma_probe: the mean brightness (0-255, before the gamma ramp) of each quarter
// of three rows of a front buffer, at 10, 40 and 80 % of its height (left to right), read
// kFramesInFlight frames after it was drawn. Quarters, as a whole-row mean hides a flash
// confined to one object.
struct LumaProbe {
  static constexpr uint32_t kRows = 3, kSegments = 4;
  uint64_t frame = 0;  // the frame they belong to (0: none yet)
  float luma[kRows][kSegments] = {};
};

// Where a frame shown put the front buffer's image in the back buffer: scaled to fit, centred.
struct ImageRect {
  int32_t left = 0, top = 0, width = 0, height = 0;
};

// What OnSwap needs from the guest Swap call (read at the call).
struct SwapCall {
  uint32_t texture = 0;    // r4: the front buffer D3DBaseTexture (0 = none)
  uint32_t fetch[6] = {};  // its fetch constant (+28), as stored (virtual address)
  // -1: OnSwap checks for a capture request itself; 0/1: the caller did (CaptureRequested).
  int8_t capture_request = -1;
  // The renderer switch hands the game to the Xenos plugin after this frame: hide the window.
  bool hand_over = false;
};

// An image OnSwap draws as it draws the front buffer, with the same transform: frame generation's
// HUD-less copy of the front buffer (frame_generation.h), into a target of the back buffers' size
// and format (PRESENT state in and out).
struct SwapCopy {
  const ResolvedImage* image = nullptr;
  ID3D12Resource* target = nullptr;
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = {};
};

class Presenter {
 public:
  explicit Presenter(NativeRenderer& renderer);
  ~Presenter();
  Presenter(const Presenter&) = delete;
  Presenter& operator=(const Presenter&) = delete;

  bool Initialize();

  // Reads the Swap's front buffer texture object from guest memory.
  static void ReadSwap(const uint8_t* base, const HookArgs& args, SwapCall* out);
  // A capture was requested (file, F12 or --rexnative_capture_frame == frame).
  bool CaptureRequested(uint64_t frame);
  // A capture runs its Swaps with the guest thread held (to read the Xenos plugin's copy of the
  // frame, --rexnative_capture_xenos).
  bool capture_holds_guest() const;
  // Such a capture is armed and not taken yet: from the OnSwap that armed it to the
  // AfterEndFrame that took it (the next frame shown). Any thread.
  bool capture_holding_guest() const;

  // In NativeRenderer::ExecuteSwap before D3D12Device::EndFrame: records the blit into the back
  // buffer (PRESENT state on entry and exit) and, for a capture, the readback copy. Sets its own
  // root signature, pipeline and descriptor heap on the list. `shown`: the frame is presented
  // (false: held, no blit). `copy`: also drawn, when not null. `classic`: the Xenos plugin's frame
  // (classic_frames.h) drawn in place of the Swap's front buffer, its gamma ramp already applied;
  // captured only without the Xenos copy (--rexnative_capture_xenos=false), as it is that copy.
  void OnSwap(const SwapCall& swap, bool shown, const SwapCopy* copy = nullptr,
              const ResolvedImage* classic = nullptr);

  // In NativeRenderer::ExecuteSwap after EndFrame: finishes a pending capture (with the Xenos
  // copy: waits for the frame's fence, holds the guest thread, reads guest memory, writes the
  // PNGs; without it: hands a completed frame's image to a thread that writes its PNG).
  void AfterEndFrame();
  // Once the GPU is idle (D3D12Device::WaitIdle): writes every capture still waiting and returns
  // when all their PNGs are on disk. The trace player calls it before it ends the process.
  void FinishShots();

  bool front_buffer_found() const;
  // Whether this Swap took a capture of frame `frame` (OnSwap copied its image).
  bool capturing(uint64_t frame) const;
  // The last Swap's front buffer, its guest physical address (0: none).
  uint32_t front_buffer_base() const;
  // The last frame shown's (empty before one).
  const ImageRect& image_rect() const;
  const LumaProbe& luma_probe() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rexnative
