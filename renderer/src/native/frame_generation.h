// ReXNative renderer: frame generation with AMD FidelityFX Frame Generation, through
// the FidelityFX API (fidelityfx.h; amd_fidelityfx_framegeneration_dx12.dll beside the loader, or
// what OptiScaler puts behind it). --rexnative_frame_generation, with the upscaler and the frame
// pacer.
//
// Each frame with the scene's inputs is presented after a frame generated between it and the one
// before. The upscaler's Upscale records Prepare with the scene's depth and motion at the render
// size; the frame then draws into image() in place of the back buffer (D3D12Device::SetFrameImage),
// Generate records the generation of the frame before it from the last image and this one, and
// EndFrame presents that frame, then the image (D3D12Device::SetGeneratedFrame). Both presents are
// FIFO and back to back, so the display shows them on consecutive refreshes, and the pacer, which
// counts a frame shown with its second present (frame_pacer.h), begins the frames at half the
// refresh rate, in step with it.
//
// Not FidelityFX's frame generation swapchain: that presents from threads of its own on its own
// clock, signals its frame latency waitable before the display shows a frame, and waits for its
// fences by spinning, a core each, and on the Deck the CPU's power is the GPU's (td-308 to td-310:
// 28 to 38 frames per second with the GPU at 336 to 610 MHz, against 87 at 1275 MHz without frame
// generation, td-311). FidelityFX's context alone generates frames for any presenter
// (FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY).
//
// The HUD is not interpolated: drawn over the scene after its motion was taken, it would smear
// between two frames (td-314). FidelityFX takes it from the frame's image where the image and a
// HUD-less copy of it differ (HUDLessColor), which needs the two to match exactly elsewhere: the
// copy is the front buffer's resolve (the last resolve before the previous Swap) run early
// (Resolver::ResolveCopy), before the frame's first blended draw into its surface after an
// opaque one, the final composite (BeforeDraw; draw logs of td-265: pixel shader
// 6B06B8CDADD9CCBF, then 40 blended quads of HUD), and the Presenter draws it into hudless() as
// it draws the front buffer into the image (SwapCopy). Other games blend full-screen passes over
// the composite before their HUD (Sega Rally Revo's haze): a copy before them differs from the
// image almost everywhere, FidelityFX takes all of it for HUD and the generated frame is a copy
// of the real one (2026-11-19). So every 15th frame the copy is compared with the image
// (MeasureHudless, on the CPU a few frames later): differing in more than half of the pixels, the
// blended draws up to the copy point are full-screen passes, and their pixel shaders, in order,
// are learned (post_passes_); the copy then waits for them, the first blended draw that is not
// the next of them being the HUD's. The generation reads the
// scene's part of both (Generate's rect). A frame without the scene's inputs (the upscaler off, a
// loading screen) is presented alone, and the next one starts the generation over: a frame ID not
// one more than the last resets it.
//
// Live: --rexnative_frame_generation is read at each frame's start (NativeRenderer::BeginFrame),
// so the settings HUD turns it on and off in game. On, the next Prepare makes the context; off,
// Release drops it (its images and FidelityFX's, about 60 MB at 1280x800) and the frames are
// presented alone again, the pacer back at the display's rate.
//
// Threading: the thread that records the frames (NativeRenderer::Execute*) but the FidelityFX
// dispatches, which run on the device's command stream (command_stream.h); the context is made
// again and released only with the stream idle (Resize, Release).
#pragma once

#include "d3d12_device.h"
#include "fidelityfx.h"
#include "resolve.h"

#include <ffx_api_types.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <utility>
#include <vector>

namespace rexnative {

class FrameGeneration {
 public:
  // Whether --rexnative_frame_generation asks for it now (live).
  static bool Requested();

  FrameGeneration(D3D12Device& device, Resolver& resolver) : device_(device), resolver_(resolver) {}
  ~FrameGeneration();
  FrameGeneration(const FrameGeneration&) = delete;
  FrameGeneration& operator=(const FrameGeneration&) = delete;

  // Its descriptors; false (logged) on failure. The FidelityFX API is loaded at the first
  // Prepare, when frame generation is first turned on.
  bool Initialize();
  // Frame generation turned off: waits for the GPU and drops the context and its images, once.
  void Release();

  // Upscaler::Upscale, after the upscaling of frame `frame`: records the preparation of its depth
  // and motion (in COMPUTE_READ) into `list`. The context and images follow the back buffers'
  // size and format first, with the GPU idle.
  void Prepare(uint64_t frame, CommandStream* list, const FfxApiResource& depth,
               const FfxApiResource& motion, FfxApiDimensions2D render_size,
               FfxApiFloatCoords2D jitter, FfxApiFloatCoords2D motion_scale,
               const FfxCamera& camera, float frame_ms, bool reset, bool depth_inverted);
  // Frames prepared in reset mode (a camera cut, the upscaler's history started over) since the
  // last call: FidelityFX generates no frame from them, only a copy of the frame after.
  uint64_t TakeResets() { return std::exchange(resets_, 0); }
  // Whether Prepare recorded frame `frame`, which is then drawn into image() and generated.
  bool prepared(uint64_t frame) const { return context_ && prepared_frame_ == frame; }
  // The frame's image: the back buffers' size and format, in PRESENT outside the frame's draws.
  ID3D12Resource* image() const { return image_.Get(); }
  D3D12_CPU_DESCRIPTOR_HANDLE image_rtv() const { return Rtv(0); }
  // The image without its HUD, of the same kind.
  ID3D12Resource* hudless() const { return hudless_.Get(); }
  D3D12_CPU_DESCRIPTOR_HANDLE hudless_rtv() const { return Rtv(1); }

  // The HUD-less copy. OnResolve: every resolve. OnSwap, `front_base` the Swap's front buffer (its
  // guest physical address).
  void OnResolve(const ResolveParams& p) { last_resolve_ = p; }
  void OnSwap(uint32_t front_base);
  // Before each draw of frame `frame` (`regs` its register file): when the draw is the HUD's first,
  // records the copy into `list` and returns true (the list's compute state changed).
  // `pixel_shader`: the draw's pixel shader hash (0: unknown).
  bool BeforeDraw(CommandStream* list, uint64_t frame, const uint32_t* regs,
                  uint64_t pixel_shader) {
    if (!front_ || prepared_frame_ != frame) [[likely]]
      return false;
    return CopyAtHud(list, frame, regs, pixel_shader);
  }
  // Frame `frame`'s copy (ResolvedImage of the front buffer's format and size), nullptr without.
  const ResolvedImage* hudless_source(uint64_t frame) const {
    return copy_frame_ == frame ? copy_ : nullptr;
  }

  // After frame `frame` was drawn into image() and, with `hudless`, into hudless(), `rect` the
  // scene's part of it: records the generation of the frame before it into `list`. Returns that
  // frame (image()'s size, format and state); a failure, on the stream's thread, is logged once.
  ID3D12Resource* Generate(CommandStream* list, uint64_t frame, FfxApiRect2D rect, bool hudless);

  // A capture of frame `frame` (the presenter's, --rexnative_capture_dir), after Generate:
  // records copies of the frame as presented and of the frame generated before it, written to
  // `dir` as f<frame>-fg-real.png and f<frame>-fg-generated.png (and the HUD-less copy,
  // f<frame>-fg-hudless.png, when `hudless`) by FinishCaptures once the GPU
  // finished them (HDR: the PQ image's top 8 bits, washed out). With --rexnative_capture_burst,
  // consecutive frames: each generated
  // frame lies between the real ones before and after it.
  void Capture(CommandStream* list, uint64_t frame, uint64_t fence, std::filesystem::path dir,
               bool hudless);
  void FinishCaptures(uint64_t completed_fence);

  // After the Presenter drew frame `frame` into image() and hudless(): every 15th such frame,
  // records copies of both for the HUD-less copy's check, which FinishCaptures runs once the GPU
  // finished them.
  void MeasureHudless(CommandStream* list, uint64_t frame, uint64_t fence);

 private:
  // The context and images for back buffers of this size and format, the context with FfxDebug's
  // watermark or not (the driver reads its variable at creation); false (logged) on failure.
  // depth_inverted: the game's scene depth is reversed (Upscaler::SceneKind).
  bool Resize(uint32_t width, uint32_t height, bool hdr, bool depth_inverted, bool debug);
  void Destroy();
  D3D12_CPU_DESCRIPTOR_HANDLE Rtv(uint32_t i) const;

  struct PendingCapture {
    uint64_t frame = 0, fence = 0;
    bool hdr = false;  // R10G10B10A2 (PQ), else R8G8B8A8
    std::filesystem::path dir;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    ComPtr<ID3D12Resource> real, generated, hudless;  // readback buffers (hudless: null)
  };
  std::vector<PendingCapture> captures_;
  bool CopyAtHud(CommandStream* list, uint64_t frame, const uint32_t* regs, uint64_t pixel_shader);
  // The share of pixels where the two readbacks differ (sampled every 4th pixel each way).
  double HudlessDifference() const;

  D3D12Device& device_;
  Resolver& resolver_;
  const FfxApi* api_ = nullptr;
  void* context_ = nullptr;  // ffxContext
  ComPtr<ID3D12Resource> image_, hudless_;
  ComPtr<ID3D12Resource> output_;          // the generated frame
  ComPtr<ID3D12DescriptorHeap> rtv_heap_;  // image_'s RTV, hudless_'s
  uint32_t rtv_stride_ = 0;
  uint32_t width_ = 0, height_ = 0;
  bool hdr_ = false;
  bool depth_inverted_ = true;
  bool debug_ = false;                    // FidelityFX's debug view and the watermark
  uint64_t prepared_frame_ = UINT64_MAX;  // the last frame Prepare recorded
  uint64_t resets_ = 0;                   // TakeResets
  ResolveParams last_resolve_;
  std::optional<ResolveParams> front_;      // the front buffer's resolve
  uint64_t composited_frame_ = UINT64_MAX;  // the last frame drawn opaque into front_'s surface
  uint64_t copy_frame_ = UINT64_MAX;        // the last frame CopyAtHud ran in
  const ResolvedImage* copy_ = nullptr;     // its copy
  // The HUD-less copy's place: the pixel shaders of the full-screen passes blended over the
  // composite before the HUD, in order (learned by MeasureHudless).
  std::vector<uint64_t> post_passes_;
  uint32_t blended_ = 0;                // blended draws since the composite, this frame
  uint32_t copy_index_ = 0;             // the one the copy was made before
  std::vector<uint64_t> frame_blends_;  // their pixel shaders, this frame (at most 32)
  struct Measure {
    bool pending = false;
    uint64_t frame = 0, fence = 0;
    uint32_t copy_index = 0;
    std::vector<uint64_t> blends;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    ComPtr<ID3D12Resource> image, hudless;  // readback buffers
  } measure_;
  uint64_t measured_frames_ = 0;  // frames since the last measurement
  uint32_t hudless_logs_ = 0;
};

}  // namespace rexnative
