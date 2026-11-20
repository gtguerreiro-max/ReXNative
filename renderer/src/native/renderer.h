// ReXNative renderer: host of the native D3D12 path.
//
// The guest D3D hooks in rexnative_d3d_trace.cpp forward every call to the guest D3D (without
// --rexnative the Xenos plugin keeps rendering, as the oracle) and, with
// --rexnative_beside_xenos, then call the NativeRenderer entry points below with the guest device and the
// call's arguments. Only the outermost
// draw/clear/resolve/Swap call is passed on (the D3D's internal clear, copy and Swap quads are
// not game draws); a top-level BeginVertices reaches OnDraw at the entry of the next hooked
// call, once the game has written its vertices. The renderer owns its own D3D12 queue, window
// and swapchain, separate from ReXGlue's presenter (d3d12_device.h).
//
// Threading: the guest D3D device is used by one guest thread at a time; all On* entry points
// are called on that thread, after the original guest function returned, and are serialized
// by the renderer. Modules may assume single-threaded access from the On* calls.
//
// With --rexnative_render_thread (default on; false keeps everything on the guest thread as
// above) the On* calls only capture, on the guest thread, what each call reads from guest
// memory: the register changes since the last draw (which the tracker records straight into the
// batch, GuestStateTracker::RecordInto), the DrawState, DrawRenderer::Prepare's shader lookup and
// vertex/index copies, and the decoded Clear/Resolve/Swap arguments. They queue it in batches to
// the render thread
// ("ReXNative render"), which replays it into the modules with its own copy of the register
// file. From then on the command list, the D3D12 objects and every module's state belong to
// that thread (DrawRenderer's vertex upload pool and shader lookup stay on the guest thread).
// The guest thread blocks at Swap while the render thread is more than one frame behind, and
// waits for the render thread to finish the Swaps around a capture (present.h).
#pragma once

#include "hook_args.h"
#include "fidelityfx.h"

#include <rex/cvar.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct ID3D12Device;
struct ID3D12CommandList;
struct ID3D12CommandQueue;

REXCVAR_DECLARE(bool, rexnative_render_thread);  // record on a render thread (see above)

namespace rexnative {

class ClassicFrames;
class CommandStream;
class D3D12Device;
class DrawRenderer;
class FrameGeneration;
class FramePacer;
class NeuralPass;
class GpuTimer;
class OcclusionQueries;
class Presenter;
class RenderTargets;
class Resolver;
class TextureCache;
class Upscaler;
struct ClearCall;
struct DrawPrep;
struct DrawState;
struct ResolveParams;
struct ResolveRegisters;
struct SwapCall;

// Per-frame counters of the native path (also the frame CSV's columns).
struct FrameStats {
  uint64_t draws = 0, clears = 0, resolves = 0;
  uint64_t state_fail = 0;  // draws ReadDrawState rejected
  // CPU time inside ExecuteDraw, ExecuteClear and ExecuteResolve, summed in ticks and converted
  // at the Swap: the render thread's time (the guest thread's without it).
  uint64_t draw_ns = 0, clear_ns = 0, resolve_ns = 0;
  uint64_t swap_ns = 0;  // ExecuteSwap: record, submit, present
  uint64_t generated = 0;  // 1: a generated frame was presented before it (frame_generation.h)
  // Guest thread: time in the On* calls (render thread mode: capturing and queueing) and the
  // part of it spent waiting for the render thread.
  uint64_t hook_ns = 0, hook_wait_ns = 0;
  uint64_t hook_max_ns = 0;  // render thread mode: the longest single On* call, without waits
  uint64_t reg_writes = 0;   // render thread mode: register values replayed
  // Parts of swap_ns: waiting for the texture untile workers, then D3D12Device::EndFrameTimes.
  uint64_t swap_untile_ns = 0, swap_submit_ns = 0, swap_present_ns = 0, swap_fence_ns = 0;
  uint64_t stream_ns = 0, stream_wait_ns = 0;  // D3D12Device::EndFrameTimes'
  uint64_t frames_held = 0;  // Swaps not presented (DrawRenderer::FrameIncomplete)
  uint64_t submits = 0;      // lists submitted before the Swap's (--rexnative_submit_draws)
  // The guest's pad reads since the Swap before (rex_xam_take_frame_pad, taken at the Swap).
  uint64_t pad = 0;
  // Pacing (NowNs clock): when the guest thread began the frame (its previous Swap returned) and
  // reached its Swap, how long FramePacer::WaitToBegin held it before it began, and the frame's
  // present number (D3D12Device::EndFrameTimes::presents; 0: not presented).
  uint64_t guest_begin_ns = 0, guest_swap_ns = 0, pace_wait_ns = 0, present = 0;
};

class NativeRenderer {
 public:
  // The renderer, created on first use when --rexnative_beside_xenos is on; nullptr when it is off, when
  // D3D12 initialization failed (logged once) and from a failed frame on (Fail).
  static NativeRenderer* Get();
  // The renderer while it draws the game: Get(), except after the renderer switch handed the
  // game to the Xenos plugin (primary mode, bind_renderer_switch) or a frame failed (in primary
  // mode from the guest thread's next Swap, which hands the game to the plugin as the switch
  // does). The hooks use this one, a single load while the renderer draws; while it is null the
  // guest D3D runs unchanged and nothing is tracked.
  // An access violation in the native work of a hook (guard.h: a layer whose offsets do not fit
  // the game's D3D read a bad guest address). From then on only Swap reaches the renderer, and it
  // hands the game to the Xenos plugin for the rest of the run, as the renderer switch does. Any
  // thread; logged once.
  static void NoteCrash(const char* where);
  static bool Crashed() { return crashed_.load(std::memory_order_relaxed); }
  // At a Swap after NoteCrash whose OnSwap faulted as well: the hand-over alone.
  void HandOverAfterCrash(uint8_t* base, uint32_t device) { HandOver(base, device); }
  static NativeRenderer* Live() {
    if (NativeRenderer* r = live_.load(std::memory_order_acquire)) [[likely]]
      return r;
    Get();  // creates the renderer on first use
    return live_.load(std::memory_order_acquire);
  }

  // True while the renderer presents the game's frames and paces them on the display
  // (frame_pacer.h): the guest D3D then presents without waiting for a vblank
  // (RexNativePresentationInterval in rexnative_d3d_trace.cpp). Any thread.
  static bool PacesFrames() { return paces_frames_.load(std::memory_order_relaxed); }

  // Whether the native renderer draws the game now (false: the Xenos plugin, after the renderer
  // switch or the automatic hand-over). Any thread.
  static bool NativeDraws() { return live_.load(std::memory_order_relaxed) != nullptr; }
  // Whether the renderer shows the game's frames, its own or the Xenos plugin's through its swap
  // chain (classic_frames.h): the settings HUD is drawn over them. Any thread.
  static bool ShowsFrames() {
    NativeRenderer* r = Get();
    return r && (NativeDraws() || r->classic_);
  }

  // Hook entry points. `base` is the guest memory base the hook received (guest virtual
  // address a is at base + a, plus 0x1000 for a >= 0xE0000000 like the generated code).
  void OnDraw(const uint8_t* base, const HookArgs& args);
  void OnClear(const uint8_t* base, const HookArgs& args);
  void OnResolve(const uint8_t* base, const HookArgs& args);
  // A resolve the push buffer holds that no hooked call made (cmd_replay.h ReplayGap,
  // rexnative.inline_resolves), with the registers at its draw packet (DecodeRegisterResolve).
  void OnRegisterResolve(const ResolveRegisters& regs);
  // The D3D's Clear quad in a replayed command buffer (cmd_replay.h), with the registers at its
  // draw packet (DecodeRegisterClear).
  void OnRegisterClear(const ResolveRegisters& regs);
  void OnSwap(uint8_t* base, const HookArgs& args);
  // Occlusion queries (occlusion.h), primary mode: after QueryIssue returned, and at the entry
  // of GetData.
  void OnQueryIssue(const uint8_t* base, const HookArgs& args);
  void OnQueryGetData(uint8_t* base, const HookArgs& args);
  // A Swap while the Xenos plugin draws (Live() is null), after the guest's Swap ran: the
  // plugin's frame shown through the renderer's swap chain (classic_frames.h), then the renderer
  // switch takes the game back, or the automatic hand-over when the game's draw calls are back.
  void OnSwapSuspended();
  // A draw call while the Xenos plugin draws (the hooks, on the presenting device).
  static void NoteSuspendedDraw() { suspended_draws_.fetch_add(1, std::memory_order_relaxed); }

  // For modules.
  D3D12Device& device() { return *device_; }
  ID3D12Device* d3d12() const;
  ID3D12CommandQueue* command_queue() const;
  // The frame's direct command list, through the device's command stream: open from the previous
  // Swap until OnSwap closes and submits it. Record into it from the On* calls only (the render
  // thread, in that mode).
  CommandStream* command_list() const;
  // Swaps recorded so far (= index of the frame being recorded).
  uint64_t frame_index() const { return frame_index_; }
  // Fence value OnSwap will signal for the frame being recorded; a resource last used in this
  // frame may be freed once device().completed_fence() >= this.
  uint64_t frame_fence() const;
  // Modules owned by the renderer.
  TextureCache& textures() { return *textures_; }
  RenderTargets& render_targets() { return *rt_; }
  Resolver& resolver() { return *resolver_; }
  // The game's occlusion queries, counted by the draws; nullptr when they are not.
  OcclusionQueries* occlusion() { return occlusion_.get(); }
  // Submits the frame so far (as --rexnative_submit_draws does, once the untile workers are
  // done) and waits until the GPU has finished it: the memexport readback. Recording goes on in
  // the next list (DrawRenderer::InvalidateListState). False when the submission failed (Fail).
  bool SubmitAndWait(uint64_t* wait_ns);
  // The scene's upscaler while it replaces the scene's MSAA (--rexnative_upscaler, switched
  // with bind_upscaler); nullptr otherwise.
  Upscaler* upscaler() { return upscaler_on_ ? upscaler_.get() : nullptr; }
  // --rexnative_frame_generation as this frame began (BeginFrame).
  bool frame_generation_on() const { return frame_generation_on_; }

  // The in-game settings (games/pgr4/src/pgr4_settings.cpp), from any thread.
  // The scene's antialiasing settings changed (--rexnative_msaa, --rexnative_upscaler and
  // its sizes): taken at the start of the next frame, the upscaler made again with them.
  void RequestAntialiasing() { antialiasing_requested_.store(true, std::memory_order_relaxed); }
  // --rexnative_present_interval changed: taken at the start of the next frame.
  void RequestFrameRate() { frame_rate_requested_.store(true, std::memory_order_relaxed); }
  // What the upscaler runs (Upscaler::Status): "AMD FSR 4.1.2 Quality, 853x480 to 1280x720";
  // empty while it is off or unavailable.
  static std::string UpscalerStatus();
  // The GPU's PCI vendor (D3D12Device::GpuVendor; 0 before the device exists).
  static uint32_t GpuVendor();
  // Primary mode: the native renderer (true) or the Xenos plugin draws from the next Swap on,
  // as the renderer switch does. A choice: the automatic hand-over stops deciding (TakeChoice),
  // also when that renderer draws already.
  // The renderer chosen, not a switch: two choices before the next Swap (the HUD's Classic then
  // Native) end on the last one, where a switch flag set twice switched once.
  void RequestRenderer(bool native) {
    choice_made_.store(true, std::memory_order_relaxed);
    wanted_.store(native ? kWantNative : kWantClassic, std::memory_order_relaxed);
  }

  ~NativeRenderer();

 private:
  NativeRenderer() = default;
  bool Initialize();
  // At the end of each frame's ExecuteSwap: its row of the frame CSV, its share of the stats
  // log (every --rexnative_d3d_stats_frames frames), then the per-frame counters start over.
  void WriteCsvRow(const FrameStats& s, uint64_t frame_ns);
  // The frame CSV's columns, their names (header) or this frame's values: those of every frame,
  // with --rexnative_profile the stage times and texture counters, with
  // --rexnative_luma_probe the probe's.
  void AppendCsvColumns(const FrameStats& s, uint64_t frame_ns, bool header, std::string* line);
  void AppendCsvProfileColumns(bool header, std::string* line);
  void AppendCsvLumaColumns(bool header, std::string* line);
  void AddToStatsLog(const FrameStats& s, uint64_t frame_ns);
  void ResetFrameCounters();
  // Starts a frame's module state (RenderTargets/Resolver BeginFrame) at its first On* call.
  void BeginFrame() {
    if (!frame_begun_) BeginFrameModules();
  }
  void BeginFrameModules();
  // The recording half of the On* calls (on the guest thread, or the render thread).
  void ExecuteDraw(const DrawState& state, const DrawPrep* prep);
  // The frame's camera (camera_): from the clip transform of its first depth-writing scene
  // draws, while camera_scan_ counts how many more of them to look at.
  void FindCamera(const DrawState& state);
  void ExecuteClear(const ClearCall& clear);
  void ExecuteResolve(const ResolveParams& p);
  void ExecuteSwap(const SwapCall& swap);
  struct QueryCall {
    uint32_t query;
    bool end;
  };
  void ExecuteQuery(const QueryCall& q);
  // After a draw: submits the frame so far once --rexnative_submit_draws draws were recorded
  // since the last submission.
  void SubmitEarly();
  // Before each submission, once the untiles finished: the texture upload lists to submit first
  // (TextureCache::CloseUploadLists), into upload_lists_. False on failure (logged).
  bool CloseUploadLists();
  // A failed submission or present: logs and turns the native path off for the rest of the run.
  // In primary mode it also releases the game window to ReXGlue's presenter (on the thread that
  // records, which owns the swapchain), and the guest thread hands the game to the Xenos plugin
  // at its next Swap (HandOver).
  void Fail();
  // Primary mode, at a Swap before anything is recorded into the back buffer: who presents into
  // the game window. The renderer while it draws the game and no XAM dialog is up; ReXGlue's
  // presenter otherwise (the dialogs are its ImGui overlay, and after the renderer switch it
  // shows the Xenos plugin's frames). A window has one swapchain at a time: the presenter's
  // surface is disconnected before ours is created, and ours released before it is connected
  // again. Returns false when the swapchain cannot be created (logged).
  bool OwnGameWindow(bool own);
  // OwnGameWindow(false) from any state, including a half-done OwnGameWindow(true).
  void ReleaseGameWindow();
  // Render thread mode.
  struct Queue;
  void QueueDraw(const uint8_t* base, const HookArgs& args);
  void QueueClear(const uint8_t* base, const HookArgs& args);
  void QueueResolve(const uint8_t* base, const HookArgs& args);
  void QueueSwap(uint8_t* base, const HookArgs& args);
  // While a frame trace is recorded (trace.h): the command's record; its number + 1, else 0.
  uint32_t TraceClear(const ClearCall& c);
  uint32_t TraceResolve(const ResolveParams& p);
  // --rexnative_trace_play, at the game's first Swap: draws the trace's frames on this thread
  // from then on; never returns.
  [[noreturn]] void PlayTrace(uint8_t* base);
  void RenderThreadMain();
  // Renderer switch. The key bind_renderer_switch (F9; a Steam Input binding can send it from a
  // back grip) requests it on the UI thread; the guest thread takes the request at its next
  // Swap. A key rather than a pad combination: the game reads every button of the pad.
  // HandOver: the Xenos plugin draws from the next frame on (also after a failed frame, Fail);
  // TakeBack: native again.
  // F9, once a press, or the renderer RequestRenderer chose when it is not the one drawing
  // (native_now); never under Wine without classic_ (SwitchRefusedUnderWine), where it is logged
  // and dropped. A choice decides over F9 presses taken with it.
  bool SwitchRequested(bool native_now) {
    const int wanted = wanted_.exchange(kWantNone, std::memory_order_relaxed);
    const bool flip = switch_requested_.load(std::memory_order_relaxed) &&
                      switch_requested_.exchange(false, std::memory_order_relaxed);
    const bool requested = wanted != kWantNone ? (wanted == kWantNative) != native_now : flip;
    return requested && !SwitchRefusedUnderWine();
  }
  static constexpr int kWantNone = 0, kWantNative = 1, kWantClassic = 2;
  std::atomic<int> wanted_{kWantNone};
  // Under Wine (the Steam Deck's Proton) the window does not change hands: once one renderer's
  // swap chain was released, the other's Present waited in vkd3d-proton for good. The switch
  // keeps the window instead (classic_), and is refused there only without it.
  static bool UnderWine();
  bool SwitchRefusedUnderWine() const;
  // The Xenos plugin's frames through the renderer's swap chain (classic_frames.h), primary mode:
  // null when the Xenos driver gives none on this device, and the window then goes to ReXGlue's
  // presenter while the plugin draws. ClassicSwap on the guest thread at each of the plugin's
  // Swaps queues ExecuteClassicSwap (the thread that records), which shows the frame.
  std::unique_ptr<ClassicFrames> classic_;
  void ClassicSwap();
  void ExecuteClassicSwap(const SwapCall& swap);
  // SwitchRequested, while the native renderer draws: false (logged) when the frame is one the
  // plugin does not draw (frame_height.h).
  bool HandOverRequested();
  void HandOver(uint8_t* base, uint32_t device);
  void TakeBack();
  // rexnative.auto_handover = N: N Swaps in a row whose frames had at most one draw call (the
  // game drew the rest with code inlined past the hooks) hand the game over; two Swaps in a row
  // with more than kAutoTakeBackDraws take it back (OnSwapSuspended). F9 turns it off for the
  // rest of the run.
  bool AutoHandOverDue();
  // Guest thread: after RequestRenderer, the automatic hand-over is off for the rest of the run.
  void TakeChoice() {
    if (!choice_made_.exchange(false, std::memory_order_relaxed)) return;
    auto_off_ = true;
    auto_handed_over_ = false;
  }
  std::atomic<bool> choice_made_{false};
  static constexpr uint32_t kAutoTakeBackDraws = 16;
  uint32_t frame_draw_calls_ = 0;  // OnDraw calls since the last Swap (guest thread)
  uint32_t idle_swaps_ = 0, busy_swaps_ = 0;
  bool auto_handed_over_ = false, auto_off_ = false;
  static inline std::atomic<uint32_t> suspended_draws_{0};
  static inline std::atomic<bool> crashed_{false};  // NoteCrash
  // ExecuteClassicSwap's own stats line: the plugin's frames since the last one, their time.
  uint64_t classic_frames_ = 0, classic_ns_ = 0, classic_last_ns_ = 0;
  // Upscaler switch. bind_upscaler (F7) switches the scene between MSAA and the upscaler,
  // requested on the UI thread and taken at the start of the next frame. The upscaler is created
  // at its first use and kept after.
  void SwitchUpscaler(bool on);
  std::atomic<bool> upscaler_switch_requested_{false};
  std::atomic<bool> antialiasing_requested_{false}, frame_rate_requested_{false};
  bool upscaler_bound_ = false;  // primary mode
  bool upscaler_on_ = false;
  uint32_t scene_msaa_ = 2;  // --rexnative_msaa, as far as the GPU has it
  void ReadSceneMsaa();      // scene_msaa_ from --rexnative_msaa

  static inline std::atomic<bool> paces_frames_{false};       // pacer_->pacing(), for any thread
  static inline std::atomic<NativeRenderer*> live_{nullptr};  // Live()
  std::atomic<bool> switch_requested_{false};
  bool switch_bound_ = false;  // primary mode with --rexnative_renderer_switch

  std::unique_ptr<Queue> queue_;  // render thread mode only

  std::unique_ptr<D3D12Device> device_;
  bool primary_ = false;       // --rexnative: presenting into the game window
  bool window_taken_ = false;  // OwnGameWindow disconnected ReXGlue's presenter from the window
  std::unique_ptr<RenderTargets> rt_;
  std::unique_ptr<Resolver> resolver_;
  std::unique_ptr<TextureCache> textures_;
  std::unique_ptr<DrawRenderer> draw_;
  std::unique_ptr<Presenter> present_;
  std::unique_ptr<OcclusionQueries> occlusion_;  // primary mode with --rexnative_occlusion
  std::unique_ptr<Upscaler> upscaler_;
  std::unique_ptr<FrameGeneration> frame_generation_;  // with the pacer
  bool frame_generation_on_ = false;  // --rexnative_frame_generation at this frame's start
  uint32_t fg_captures_left_ = 0;     // frames still to capture (FrameGeneration::Capture)
  std::unique_ptr<NeuralPass> neural_;                 // this port: --rexnative_dlss5
  // The scene's camera for the FidelityFX effects, found each frame by FindCamera (the defaults
  // until one matches); the upscaler reads it through the pointer set_camera gave it.
  FfxCamera camera_;
  uint32_t camera_scan_ = 0;  // this frame's scene depth draws still to scan for it
  bool camera_logged_ = false;
  // Camera cuts: the view direction (the clip transform's w row, normalized) of the last frame
  // the camera was found in, and that frame; a turn of more than kCutCos between two frames in a
  // row is a cut (FindCamera).
  float camera_dir_[3] = {};
  uint64_t camera_frame_ = 0;
  uint32_t camera_cuts_ = 0;
  // Primary mode with --rexnative_frame_latency > 0, or observing only with --rexnative_csv.
  std::unique_ptr<FramePacer> pacer_;
  GpuTimer* gpu_timer_ = nullptr;  // device_'s, with --rexnative_gpu_timing
  bool frame_begun_ = false;
  uint64_t frame_index_ = 0;
  uint64_t frame_start_ns_ = 0;
  FrameStats stats_;
  // Ticks inside ExecuteDraw, ExecuteClear and ExecuteResolve, into stats_ at the Swap.
  uint64_t draw_ticks_ = 0, clear_ticks_ = 0, resolve_ticks_ = 0;
  uint64_t draws_submitted_ = 0;                  // stats_.draws at the frame's last SubmitEarly
  std::vector<ID3D12CommandList*> upload_lists_;  // CloseUploadLists'
  struct Csv;
  std::unique_ptr<Csv> csv_;
  // This port: the front end's frame limit (--rexnative_menu_fps), on the guest thread at
  // each Swap; returns the time it waited.
  uint64_t FrontEndLimit();
  uint64_t limit_ns_ = 0;
  void* limit_timer_ = nullptr;  // HANDLE
};

}  // namespace rexnative
