// ReXNative renderer: temporal upscaling of the 3D scene with AMD FidelityFX Super
// Resolution, through the FidelityFX API (fidelityfx.h), which OptiScaler hooks to put other
// upscalers such as FSR 4 in its place.
//
// The scene is PGR4's 1280x720 2x MSAA surface: RB_SURFACE_INFO pitch 1280 with 2x MSAA, the
// only surface of that pitch and MSAA in the race and the menus (draw logs of td-265). With
// --rexnative_upscaler its host targets are single-sampled and of the render size
// (--rexnative_upscaler_width/height, RenderTargets::OverrideScene: the draws' viewports,
// scissors and clears scale with them; with --rexnative_upscaler_target_fps a size up to that
// chosen each frame, DynamicResolution and RenderTargets::SetSceneScale), every draw into it is
// offset by the frame's sub-pixel jitter and samples its mipmapped textures with AMD's LOD bias for
// the render size (DrawRenderer, the jitter through the system constants' NDC offset), the draws
// that write depth also write their motion into MotionTarget (vertex shaders that run again with
// the previous frame's constants, DxbcShaderTranslator's motion_vectors), and the first resolve out
// of it in a frame (EndTiling's depth resolve, before its color resolves clear the surface)
// upscales its color, depth and motion into the output, which the color resolves copy from instead
// of the target. FSR reads the color clamped to the range of its 7e3 EDRAM format, as the resolves
// do, and those copies take the alpha of the scene's target, which FSR does not upscale: the
// motion blur's mask (pixel shader 27A3898925DDCAF7), which keeps the player's car sharp. With
// frame generation (frame_generation.h) the same depth and motion are its inputs.
//
// Threading: everything on the thread that records the frames (NativeRenderer::Execute*) but the
// FidelityFX dispatch, which runs on the device's command stream (command_stream.h); the context is
// made again only with the stream idle (D3D12Device::WaitIdle).
#pragma once

#include "d3d12_device.h"
#include "dlss.h"
#include "dynamic_resolution.h"
#include "fidelityfx.h"
#include "gpu_timer.h"
#include "log.h"
#include "render_targets.h"
#include "scene.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace rexnative {

class NeuralPass;

class FrameGeneration;

class Upscaler {
 public:
  // Whether --rexnative_upscaler asks for the upscaler.
  static bool Requested();
  // What the upscaler runs, for the settings menu (any thread): "AMD FSR 4.1.2 Quality, 853x480
  // to 1280x720"; empty while it is off or failed. NoteOn: NativeRenderer::SwitchUpscaler's.
  static std::string Status();
  static void NoteOn(bool on);

  // The scene surface, from its RB_SURFACE_INFO (pitch and MSAA; the guest's value whatever the
  // host targets' sample count), and its size: game_layer.h's constants, or learned at run time
  // (scene.h).
  static bool IsScene(uint32_t surface_info) { return IsSceneSurface(surface_info); }
  static uint32_t SceneWidth() { return ::rexnative::SceneWidth(); }
  static uint32_t SceneHeight() { return ::rexnative::SceneHeight(); }
  // Whether a span of the scene surface (a resolve's, a viewport's) is the scene's width: the
  // scene resolves in tiles of its whole width, while the rear-view mirror renders into a corner
  // of the scene surface and resolves that corner on its own (neither upscaled nor jittered).
  static bool IsSceneWidth(int32_t x0, int32_t x1) {
    return x0 <= 0 && x1 >= int32_t(SceneWidth());
  }
  // Whether a resolve of the scene surface copies the scene: all its width (or bands of it), or,
  // for a scene wider than its surface's pitch (predicated tiles side by side: Tetris Splash's
  // 1280x720 in two tiles 640 wide), a tile of its whole height. Each tile then copies its part
  // of the upscaled output; with the width alone, the left tile was upscaled from a scene 640
  // wide and the right one copied nothing (the right half of the 3D logo was black).
  static bool IsSceneResolve(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    if (IsSceneWidth(x0, x1)) return true;
    return SceneWidth() > ScenePitch() && y0 <= 0 && y1 >= int32_t(SceneHeight()) && x0 >= 0 &&
           x1 > x0 && x1 <= int32_t(SceneWidth());
  }

  Upscaler() = default;
  // This port: the output at this many host pixels per guest pixel (the resolution scale), the
  // render size from it; before Initialize.
  void set_output_scale(uint32_t scale) { out_scale_ = scale; }
  uint32_t OutputWidth() const { return SceneWidth() * out_scale_; }
  uint32_t OutputHeight() const { return SceneHeight() * out_scale_; }
  ~Upscaler();
  Upscaler(const Upscaler&) = delete;
  Upscaler& operator=(const Upscaler&) = delete;

  // Loads the FidelityFX API and creates the upscaling context; false (logged) when either fails.
  bool Initialize(ID3D12Device* device);

  // Once per frame before recording it: the frame's jitter, and with
  // --rexnative_upscaler_target_fps its render size from the GPU's time (D3D12Device::
  // gpu_busy_ns) and the time in ns of a frame at the pace the display shows them
  // (DynamicResolution::Update). True when the render size changed: the scene's targets take the
  // new scale() (RenderTargets::SetSceneScale) before the frame's draws.
  bool BeginFrame(uint64_t frame, uint64_t gpu_busy_ns, uint64_t paced_ns);
  // The history is not this scene's (the upscaler was off): the next upscaling starts over, and
  // the GPU times of the frames before are not counted toward the render size.
  void Reset() {
    reset_ = true;
    last_ns_ = 0;
    if (dynamic_) dynamic_->Restart();
  }
  // A camera cut this frame (NativeRenderer::FindCamera): the upscaling and the frame generation
  // start over rather than blend the frames before it, which show another view.
  void NoteCameraCut() { reset_ = true; }
  // The scene in passes: a pass ends at a full-width colour resolve of the scene's surface after
  // draws into it (its tiles, one resolve each with no draw between, end one pass), and a game
  // may draw the scene's surface again after it (Midnight Club: Los Angeles resolves a first
  // pass, cleared, then draws its 2000 draw main scene in the same surface). The upscaler takes
  // the frame's last pass that wrote depth (the scene's geometry, where the post-processing and
  // the HUD write none), its number from the last frame; the first pass when none wrote depth.
  // A pass with less than a quarter of the depth draws of the frame's largest one before it is
  // not the scene: Minecraft resolves its world, clears the depth and draws its HUD, whose
  // hotbar items are 3D blocks writing depth, and upscaling that pass took the HUD's text into
  // the upscaler's history (it trailed behind the camera's turns).
  // NoteSceneDraw for each draw or clear into the scene's surface, SceneResolveDue for each
  // full-width colour resolve of it (true: upscale here), the ones after the upscaled resolve
  // too: a pass of the scene drawn there at the output's size moves the next frame's upscaling to
  // it, where a count stopped at the upscaled resolve kept it early for good. A frame with fewer
  // passes than the last upscales none (its resolves copy the render-size scene); one with more
  // upscales an earlier one, and the next frame expects the new count.
  // gbuffer: the draw writes depth into several colour targets, a G-buffer (Overlord II). The
  // scene is lit in later passes that only test depth: from then on those count (deferred()),
  // and a G-buffer pass is never the one upscaled, nor weighed against the lit passes (its 220
  // draws left a gameplay light pass of 39 out, and FSR ran on the normals' target).
  bool deferred() const { return deferred_; }
  void NoteSceneDraw(bool writes_depth, bool gbuffer = false) {
    pass_drawn_ = true;
    pass_depth_draws_ += writes_depth;
    if (gbuffer) {
      pass_gbuffer_ = true;
      if (!deferred_)
        REXLOG_INFO("ReXNative upscaler: the scene fills a G-buffer (depth and several colour "
                    "targets): its lit passes, which only test depth, are the scene's");
      deferred_ = true;
    }
  }
  bool SceneResolveDue() {
    if (pass_drawn_ || !scene_passes_) {
      ++scene_passes_;
      resolved_depth_ = pass_depth_draws_ != 0 && !pass_gbuffer_;
      if (pass_depth_draws_ && !pass_gbuffer_) {
        most_depth_draws_ = std::max(most_depth_draws_, pass_depth_draws_);
        if (pass_depth_draws_ * 4 >= most_depth_draws_) {
          depth_pass_ = scene_passes_;
        } else {
          REXNATIVE_LOG_FIRST(1, INFO,
                              "ReXNative upscaler: pass {} of the scene left out, {} draws "
                              "writing depth where an earlier pass has {} (a HUD's 3D icons)",
                              scene_passes_, pass_depth_draws_, most_depth_draws_);
        }
      }
      resolved_gbuffer_ = pass_gbuffer_;
      pass_drawn_ = false;
      pass_gbuffer_ = false;
      pass_depth_draws_ = 0;
    }
    return scene_passes_ >= expected_pass_ && (resolved_depth_ || !expect_depth_);
  }
  // A scene draw's motion match (DrawRenderer::BindPreviousConstants): compared by its first
  // vertex, and refused for its distance (MotionHistory). Most of a frame's comparisons refused
  // is a camera cut where FindCamera sees no view turn (Jet Set Radio transforms its vertices
  // on the CPU: its constants hold the projection alone): Upscale starts over then too.
  void NoteMotionMatch(bool compared, bool refused) {
    if (match_frame_ != frame_) {
      match_frame_ = frame_;
      matches_compared_ = matches_refused_ = 0;
    }
    matches_compared_ += compared;
    matches_refused_ += refused;
  }
  // The upscaler running: NVIDIA DLSS / DLAA (dlss.h) where the GPU and nvngx_dlss.dll have it,
  // AMD FSR through the FidelityFX API otherwise (FSR 4 on the GPUs that run it, FSR 3.1
  // elsewhere, the Steam Deck among them), or as --rexnative_upscaler_backend asks.
  enum class Backend { kDlss, kFsr };
  Backend backend() const { return backend_; }
  const char* BackendName() const;
  // What the scene needs of the upscaler, from the game: an HDR scene (a float target, before
  // the game's exposure) and a reversed depth (cleared to 0). DLSS takes them per evaluation;
  // FSR's context is created with them, so an Upscale that finds them changed skips the frame
  // and ContextOutdated asks the renderer for UpdateContext, with the GPU idle.
  struct SceneKind {
    bool hdr = false;
    bool depth_inverted = false;
    bool operator==(const SceneKind&) const = default;
  };
  bool ContextOutdated() const { return context_outdated_; }
  // FSR 4 gave way to FSR 3.1, or FSR 3.1 to the scene's MSAA, for the rest of the run
  // (--rexnative_fsr_auto, WatchFsrCost): true once, when the antialiasing is to be made again.
  bool TakeFsrDowngrade() { return std::exchange(fsr_downgrade_due_, false); }
  bool UpdateContext() {
    kind_ = wanted_kind_;
    return CreateContext(debug_);
  }
  // FSR's debug view in place of the upscaled scene (FfxDebug): its inputs (color, depth, motion
  // vectors, reactiveness) and what it made of them, in a grid, and with FSR 4 AMD's watermark.
  // Either way the context is created again, debug visualization taking more memory and the
  // driver reading the watermark's variable at creation: between frames, with the GPU idle. False
  // (logged) when that failed. OptiScaler upscales in a context of its own, from the inputs alone:
  // through it the watermark shows, and the debug view is its menu's (Upscaler Debug View).
  bool debug() const { return debug_; }
  bool SetDebug(bool on) { return CreateContext(on); }
  // This frame's sub-pixel jitter in render pixels (x right, y down), for the scene's draws.
  float jitter_x() const { return jitter_[0]; }
  float jitter_y() const { return jitter_[1]; }
  // A scene draw's alpha to coverage dither offsets this frame, from the guest's (RB_COLORCONTROL
  // bits 24:31, a 2-bit threshold offset for each pixel of a 2x2 quad). Single-sampled, alpha to
  // coverage is that ordered dither alone, the same in every frame, and the upscaler's history
  // keeps it as a screen door in the color, depth and motion. Rotated one place a frame, each
  // pixel takes the four thresholds in turn, and the history averages them to the coverage.
  uint32_t AlphaToMaskOffsets(uint32_t offsets) const {
    const uint32_t shift = 2 * uint32_t(frame_ & 3);
    return ((offsets >> shift) | (offsets << (8 - shift))) & 0xFF;
  }
  // The scene's render size this frame on its guest size, and its largest
  // (--rexnative_upscaler_width/height: the size of the scene's targets).
  RenderScale scale() const {
    return {{render_width_, render_height_}, {SceneWidth(), SceneHeight()}};
  }
  RenderScale max_scale() const {
    return {{max_width_, max_height_}, {SceneWidth(), SceneHeight()}};
  }
  uint32_t render_height() const { return render_height_; }
  // The scene's texture LOD bias, in negative quarter levels (SamplerKey::lod_bias).
  uint32_t mip_bias_quarters() const { return mip_bias_quarters_; }

  // The motion vector target of the scene's draws at the largest render size
  // (DxbcShaderTranslator's kMotionVectorRenderTarget, kMotionFormat), cleared to no motion at the
  // first call of a frame.
  static constexpr DXGI_FORMAT kMotionFormat = DXGI_FORMAT_R16G16_FLOAT;
  D3D12_CPU_DESCRIPTOR_HANDLE MotionTarget(CommandStream* list);

  // The upscaled scene color of this frame: at the first call of a frame (a resolve out of the
  // scene surface `regs`), records the upscaling of the surface's color and depth into `list`.
  // nullptr (logged once) when that failed; the resolves then copy the target as it is.
  RenderTarget* Upscale(CommandStream* list, RenderTargets& targets, const SurfaceRegs& regs);
  // Frame generation's inputs, recorded by each Upscale; nullptr: none.
  void set_frame_generation(FrameGeneration* fg) { frame_generation_ = fg; }
  // The scene's camera, the renderer's, read at each Upscale.
  void set_camera(const FfxCamera* camera) { camera_ = camera; }
  // With GPU timing: Upscale marks its passes (GpuTimer::UpscalePhase).
  void set_gpu_timer(GpuTimer* timer) { timer_ = timer; }
  // This port: DLSS 5 on the scene after DLAA (neural.h's scene stage); nullptr: none.
  void set_neural(NeuralPass* neural) { neural_ = neural; }

  // This port, DLSS 5 (neural.h): the inputs of this frame's upscaling, read after it.
  struct SceneInputs {
    ID3D12Resource* depth = nullptr;   // NON_PIXEL_SHADER_RESOURCE, reversed
    ID3D12Resource* motion = nullptr;  // NON_PIXEL_SHADER_RESOURCE, UV-space, current -> previous
    uint32_t width = 0, height = 0;    // the render size
    float jitter_x = 0, jitter_y = 0;  // render pixels
    bool reset = false;                // the upscaling started over
  };
  bool upscaled_this_frame() const { return upscaled_frame_ == frame_ && upscaled_; }

  // After the upscaling resolve, the game may draw into the scene's surface again in the same
  // frame: post-processing passes, its HUD, a first-person hand (Minecraft resolves it four times
  // a frame, the HUD over the last). Resolves before any such write copy the upscaled output
  // (PGR4's tiles, resolved one after another); the first draw or clear after them calls
  // EnterPostScene: the surface's own full-size targets from then on (RenderTargets::
  // SuspendSceneOverride), its color starting from the upscaled output, or from the clear the
  // last resolve made, its depth from that clear; no jitter, no motion; the later resolves copy
  // that target. Returns whether it recorded calls (the caller sets its own state again).
  bool post_scene() const { return post_scene_frame_ == frame_; }
  bool EnterPostScene(CommandStream* list, RenderTargets& targets, const SurfaceRegs& regs);

  // FSR's reactive mask (--rexnative_fsr_reactive). FSR 3 keeps the history of a pixel whose
  // motion it trusts, so particles and other blended draws without motion trail behind. The scene
  // as it is before the first blended draw that writes no depth in the pass to be upscaled (in a
  // deferred scene, after its lights: draws that read the G-buffer) is copied (SnapshotOpaque);
  // at the upscale FidelityFX's GenerateReactiveMask compares it with the finished scene.
  bool WantsOpaqueSnapshot() const;
  void SnapshotOpaque(CommandStream* list, RenderTargets& targets, const SurfaceRegs& regs);
  // A colour resolve of the scene's surface (after SceneResolveDue): a G-buffer pass's
  // destinations are remembered for ReadsGBuffer for the rest of the frame.
  void NoteSceneColorResolve(uint32_t base, uint32_t extent_bytes);
  bool ReadsGBuffer(uint32_t address) const;
  // Each resolve that copied the upscaled output: its clears, for EnterPostScene.
  void NoteSceneResolve(const ResolveParams& p);
  // This frame's depth and motion, made readable in `list`; empty when the frame was not upscaled.
  SceneInputs ReadSceneInputs(CommandStream* list);
  Dlss& dlss() { return dlss_; }

 private:
  // The upscaling context, with debug visualization and the watermark or not; false (logged) on
  // failure.
  bool CreateContext(bool debug);
  // The FidelityFX loader's id of the newest FSR 4.x or 3.x for this GPU when
  // --rexnative_fsr_version asks for one; 0 for the newest of all.
  uint64_t FsrVersionId() const;
  bool CreateTargets();
  // The commands recorded next belong to the upscaling pass `phase` (with GPU timing).
  void Time(CommandStream* list, GpuTimer::UpscalePhase phase);
  // The render size, LOD bias and jitter phases of a DynamicResolution level; false (logged)
  // without a phase count.
  bool SetLevel(uint32_t level);
  // The clamp of FSR's input (kInputHlsl); false (logged) on failure.
  bool CreateInputPass();
  // Records the clamp of the scene's `color` (read as `format`) into input_ at the render size.
  // `target` (input_ or opaque_) and `view_slot` (its views' first slot in the frame's views).
  void ClampInput(CommandStream* list, ID3D12Resource* color, DXGI_FORMAT format,
                  ID3D12Resource* target, uint32_t view_slot);
  // GenerateReactiveMask into reactive_ when this frame's opaque snapshot is of the upscaled pass.
  bool GenerateReactive(CommandStream* list);

  ID3D12Device* device_ = nullptr;
  Backend backend_ = Backend::kDlss;
  Dlss dlss_;  // DLSS / DLAA (dlss.h)
  Dlss::Mode mode_ = Dlss::Mode::kDlaa;  // the render size's ratio, for either backend
  SceneKind kind_{};         // what FSR's context was created for
  SceneKind wanted_kind_{};  // what the last Upscale found
  bool context_outdated_ = false;
  uint64_t post_scene_frame_ = UINT64_MAX;  // the frame EnterPostScene ran in
  // The last upscaled resolve's clears (ResolveParams::clear_*).
  uint32_t resolve_clear_flags_ = 0;
  float resolve_clear_color_[4] = {};
  float resolve_clear_depth_ = 1.0f;
  uint8_t resolve_clear_stencil_ = 0;
  // EnterPostScene's copy of the output into the full-size target: per RTV format (bits 4 up)
  // and sample count (bits 0-3).
  bool CreateCopyPass();
  ComPtr<ID3D12RootSignature> copy_signature_;
  ComPtr<ID3DBlob> copy_vs_, copy_ps_;
  std::unordered_map<uint32_t, ComPtr<ID3D12PipelineState>> copy_pso_;
  const FfxApi* api_ = nullptr;
  FrameGeneration* frame_generation_ = nullptr;
  const FfxCamera* camera_ = nullptr;  // the renderer's
  GpuTimer* timer_ = nullptr;
  void* context_ = nullptr;  // ffxContext
  // The upscaled color (an R16G16B16A16_FLOAT UAV: the scene's host format), in the fields the
  // resolve reads; and the motion vectors.
  RenderTarget output_;
  // A scene the game reads through another format's EDRAM bits (RenderTargets::ConvertedFrom:
  // Asura's Wrath draws it in 2_10_10_10_FLOAT and resolves it as 2_10_10_10): the upscaler takes
  // the real colors, and the resolve the upscaled output converted to those bits, here (the
  // format the game resolved, at the output's size). `encoded_`: this frame's upscale did so.
  RenderTarget encoded_output_;
  ComPtr<ID3D12DescriptorHeap> encoded_rtv_;
  bool encoded_ = false;
  // The resolve's source: output_, or encoded_output_ after an encoded scene.
  RenderTarget* Result() { return encoded_ ? &encoded_output_ : &output_; }
  bool PrepareEncodedOutput(const RenderTarget& like);
  ComPtr<ID3D12Resource> motion_;
  ComPtr<ID3D12DescriptorHeap> motion_rtv_;
  D3D12_RESOURCE_STATES motion_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
  uint64_t motion_frame_ = UINT64_MAX;  // the frame motion_ was cleared for
  // FSR's color input: the scene's color clamped, at the largest render size.
  ComPtr<ID3D12Resource> input_;
  D3D12_RESOURCE_STATES input_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  // The reactive mask: the opaque scene (as input_) and the mask (R8), at the largest render size.
  ComPtr<ID3D12Resource> opaque_, reactive_;
  D3D12_RESOURCE_STATES opaque_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  uint64_t opaque_frame_ = UINT64_MAX;  // the frame of the snapshot in opaque_
  uint32_t opaque_pass_ = 0;            // its pass (scene_passes_ numbering)
  struct Range {
    uint32_t lo, hi;
  };
  std::array<Range, 8> gbuffer_dests_ = {};
  uint32_t gbuffer_dest_count_ = 0;
  uint64_t gbuffer_dest_frame_ = UINT64_MAX;
  bool resolved_gbuffer_ = false;  // the last pass closed by SceneResolveDue was a G-buffer's
  ComPtr<ID3D12RootSignature> input_signature_;
  ComPtr<ID3D12PipelineState> input_pso_;
  ComPtr<ID3D12DescriptorHeap> input_heap_;
  uint32_t input_stride_ = 0;
  uint32_t max_width_ = 0, max_height_ = 0;  // Initialize's, from the scene's size
  uint32_t render_width_ = 0, render_height_ = 0;
  std::optional<DynamicResolution> dynamic_;  // with --rexnative_upscaler_target_fps
  uint32_t mip_bias_quarters_ = 0;
  uint64_t frame_ = 0;
  bool deferred_ = false;  // a G-buffer draw seen (NoteSceneDraw): depth-tested draws count
  bool pass_gbuffer_ = false;  // this pass has a G-buffer draw: not the scene's colour
  // SceneResolveDue's: this frame's passes and its last that wrote depth, the last frame's one.
  uint32_t scene_passes_ = 0, depth_pass_ = 0, expected_pass_ = 1;
  uint32_t most_depth_draws_ = 0;  // the frame's largest count of a pass's draws writing depth
  bool expect_depth_ = false, resolved_depth_ = false;
  bool pass_drawn_ = false;        // since the last scene resolve
  uint32_t pass_depth_draws_ = 0;  // the same, its draws writing depth
  uint64_t match_frame_ = UINT64_MAX;  // NoteMotionMatch's frame and counts
  uint32_t matches_compared_ = 0, matches_refused_ = 0, motion_cuts_ = 0;
  uint64_t upscaled_frame_ = UINT64_MAX;
  bool upscaled_ = false;  // the frame's Upscale succeeded
  RenderTarget* scene_depth_ = nullptr;  // that Upscale's depth (SceneInputs)
  uint32_t out_scale_ = 1;
  NeuralPass* neural_ = nullptr;
  bool scene_reset_ = false;
  uint64_t last_ns_ = 0;
  float frame_ms_ = 0.0f;
  // WatchFsrCost's: FSR 4 runs, the GPU's busy and paced time over the window so far.
  static constexpr uint32_t kFsrWindowFrames = 120, kFsrWindows = 2;
  static constexpr uint64_t kFsrSettleFrames = 600;
  void WatchFsrCost(uint64_t frame, uint64_t gpu_busy_ns, uint64_t paced_ns);
  bool fsr4_ = false, fsr_downgrade_ = false, fsr_downgrade_due_ = false;
  uint64_t busy_sum_ns_ = 0, paced_sum_ns_ = 0;
  uint32_t busy_frames_ = 0, busy_windows_ = 0;
  int32_t phase_count_ = 0;
  float jitter_[2] = {};
  bool reset_ = true;
  bool debug_ = false;  // context_ has debug visualization and the watermark
};

}  // namespace rexnative
