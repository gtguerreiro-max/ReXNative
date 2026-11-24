// ReXNative renderer: NativeRenderer host.
#include "renderer.h"
#include "debug_tools.h"

#include "legacy_names.h"

#include "async_file.h"
#include "classic_frames.h"
#include "clock.h"
#include "d3d12_device.h"
#include "d3d_state.h"
#include "draw.h"
#include "frame_generation.h"
#include "neural.h"
#include "frame_height.h"
#include "frame_pacer.h"
#include "hud/hud.h"
#include "gpu_timer.h"
#include "guard.h"
#include "guest_memory.h"
#include "occlusion.h"
#include "present.h"
#include "profile.h"
#include "render_targets.h"
#include "resolve.h"
#include "textures.h"
#include "trace.h"
#include "upscaler.h"

#include "host.h"

#include <rex/logging.h>
#include <rex/xenos/register.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

// patches/0025 (xam_input.cpp): what user 0's XamInputGetState calls returned
// since the last call, merged (bit layout there).
extern "C" uint64_t rex_xam_take_frame_pad();

namespace reg = rex::graphics::reg;

REXCVAR_DEFINE_BOOL(rexnative_beside_xenos, false, "ReXNative",
                    "Run the native D3D12 renderer beside the Xenos plugin (debug window)");
// On by default where the game's layer says so (game_layer.h kDefaultEnabled): --rexnative=false
// gives the game back to the Xenos plugin.
REXCVAR_DEFINE_BOOL(rexnative, rexnative::game::kDefaultEnabled, "ReXNative",
                    "ReXNative draws and presents the game: it presents into the game window and "
                    "the Xenos plugin skips all draws, resolves and swaps (gpu_skip_draws, "
                    "patches/0031); implies --rexnative_beside_xenos. On by default where the "
                    "game's layer says so");
REXCVAR_DEFINE_INT32(rexnative_msaa, 2, "ReXNative",
                     "Samples of the 3D scene's MSAA without the upscaler: 1, 2 (the game's), 4 "
                     "or 8 (fewer if the GPU lacks them)");
REXCVAR_DEFINE_INT32(rexnative_render_height, rexnative::game::kDefaultRenderHeight, "ReXNative",
                     "The render height the 16:10 patch gives every hooked 720 while the native "
                     "renderer draws from the start (720 disables it; the Xenos plugin always "
                     "draws the game's 720, frame_height.h)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(rexnative_resolution_scale, 1, "ReXNative",
                     "This port: the game's screen-sized surfaces (the scene, its post-processing "
                     "and the front buffer, 1280 wide) render at this many host pixels per guest "
                     "pixel, 1-3 (2: 2560x1440 for the game's 1280x720), with their resolves, "
                     "DLSS and DLSS 5 at that size")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(rexnative_menu_fps, 60, "ReXNative",
                     "The game's frame rate limit (30: the console's for a game that ran at 30, "
                     "such as Dark Souls II; 0: none). Menus whose cursor repeats once per frame "
                     "go too fast above 60. PGR4 lifts it while the player drives");
REXCVAR_DEFINE_INT32(rexnative_auto_handover, 0, "ReXNative",
                     "After this many frames in a row with no draw call the native renderer "
                     "sees (screens drawn by code the game inlined past the hooked D3D "
                     "functions: Orcs Must Die!'s title and menus), ReXGlue's renderer draws, "
                     "as after F9, until the game's draw calls come back; 0: off");
REXCVAR_DEFINE_STRING(rexnative_csv, "", "ReXNative",
                      "Per-frame CSV of the native path: counts and CPU time per hook class");
REXCVAR_DEFINE_INT32(rexnative_width, 1280, "ReXNative", "Debug window width");
REXCVAR_DEFINE_INT32(rexnative_height, 720, "ReXNative", "Debug window height");
REXCVAR_DEFINE_BOOL(rexnative_profile, false, "ReXNative",
                    "Per-stage CPU time of the draw path (RDTSC), appended to the native CSV as "
                    "st_<stage>_us columns and to the stats log line");
REXCVAR_DEFINE_BOOL(rexnative_gpu_timing, false, "ReXNative",
                    "GPU timestamps around every pass of the native frame (draws per render "
                    "target set, clears, resolves, present); the costliest passes are logged "
                    "every --rexnative_d3d_stats_frames frames");
REXCVAR_DEFINE_BOOL(rexnative_gpu_timing_shaders, false, "ReXNative",
                    "With --rexnative_gpu_timing: time draws per vertex/pixel shader pair "
                    "instead of per render target set, and log the ucode of the costliest "
                    "shaders once (render thread mode)");
REXCVAR_DEFINE_BOOL(rexnative_texture_capture_order, true, "ReXNative",
                    "Bind textures as they were when the game queued the draw: a texture the game "
                    "rewrites before the render thread reaches the draw keeps its older data for "
                    "it (TextureCache::MarkCapture)");
REXCVAR_DEFINE_BOOL(rexnative_render_thread, true, "ReXNative",
                    "Record the native frames on their own thread (\"ReXNative render\"): the "
                    "game thread only copies what each D3D call reads from guest memory and "
                    "queues it, at most one frame ahead of the render thread");
REXCVAR_DEFINE_BOOL(rexnative_render_thread_lockstep, false, "ReXNative",
                    "Debug: with the render thread, each Swap waits until the render thread has "
                    "recorded that frame, so the game never writes the next frame's memory while "
                    "the render thread still reads this one's (a race on guest memory read late)");
REXCVAR_DEFINE_BOOL(rexnative_texture_reload, false, "ReXNative",
                    "Debug (live): at the next frame every cached texture uploads again from guest "
                    "memory, then the switch turns itself off (a texture whose memory was written "
                    "without the cache noticing)");
REXCVAR_DEFINE_INT32(rexnative_submit_draws, 256, "ReXNative",
                     "Submit the frame's commands every this many draws, so the GPU starts on them "
                     "while the rest of the frame is recorded (0: one submission per frame, at the "
                     "Swap)");
// Off for From Dust: it hands its pool of 89 queries, in order, to whichever objects it tests
// in a frame and reads each result in the same frame, which the host can only answer with an
// older frame's count for that query, another object's. A count of 0 hid a visible object for a
// frame (the background showed through: the "blue" flashes on the totem and the huts, 9 frames
// in 735 against none with this off). Off costs nothing measurable (1460 draws a frame against
// 1464).
REXCVAR_DEFINE_BOOL(rexnative_occlusion, rexnative::game::kDefaultOcclusion, "ReXNative",
                    "With --rexnative: count the samples of the game's occlusion "
                    "queries on the host; off (From Dust's default), every query reports its "
                    "whole draw visible")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(rexnative_renderer_switch, true, "ReXNative",
                    "With --rexnative, the key bind_renderer_switch (F9) switches "
                    "between the native renderer and the Xenos plugin at the next Swap");
REXCVAR_DEFINE_BOOL(rexnative_classic, false, "ReXNative",
                    "With --rexnative: the Xenos plugin draws the game from the start, "
                    "as after the renderer switch, until the switch (F9 or the Display settings' "
                    "Renderer) hands it to the native renderer");
REXCVAR_DEFINE_BOOL(rexnative_debug_keys, true, "ReXNative",
                    "The renderer's keys: bind_renderer_switch (F9), bind_upscaler (F7) and "
                    "bind_upscaler_debug (F8, this port: DLSS 5's settings); false: they do "
                    "nothing");
REXCVAR_DEFINE_INT32(rexnative_frame_latency, 3, "ReXNative",
                     "With --rexnative: pace the game's frames on the display, with at "
                     "most this many frames begun but not yet shown (1-3, frame_pacer.h; the game "
                     "thread, render thread and GPU each hold one at 90 fps); 0 leaves "
                     "the pacing to the game's vblank wait (with --rexnative_csv the shown "
                     "presents are still logged)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(rexnative_frame_latency_auto, true, "ReXNative",
                    "With --rexnative_frame_latency=1: frames that miss the display raise it to "
                    "2 for as long as they would at 1, and it comes back to 1 once they fit "
                    "(frame_pacer.h, automatic latency); false keeps 1 whatever the frames do");
REXCVAR_DEFINE_INT32(rexnative_debug_frame_cost_us, 0, "ReXNative",
                     "Testing: the render thread spends this long on each frame before it is "
                     "submitted, as a heavy game's frame (the automatic frame latency on a fast "
                     "PC); live through the control file");
REXCVAR_DEFINE_INT32(rexnative_debug_game_cost_us, 0, "ReXNative",
                     "Testing: the game thread spends this long at the start of each frame, as "
                     "a heavy game's logic (with --rexnative_debug_frame_cost_us, a pipeline that "
                     "fits at frame latency 2 and not at 1); live through the control file");
REXCVAR_DEFINE_INT32(rexnative_present_interval, 1, "ReXNative",
                     "With the frames presented with vsync (the frame pacer's FIFO): each frame "
                     "is shown for this many display refreshes (1-3), so the game runs at the "
                     "refresh rate over it: 90, 45 or 30 fps at 90 Hz, evenly. Steam's frame "
                     "limiter does the same through gamescope; a frame with a generated one "
                     "before it is shown once");
REXCVAR_DEFINE_BOOL(rexnative_low_latency, true, "ReXNative",
                    "With the frames paced on the display: begin each frame as late as the "
                    "time the frames before it waited for the display allows (frame_pacer.h), "
                    "so a frame limit lowers the latency instead of raising it")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(rexnative_latency_margin_us, 4000, "ReXNative",
                     "With --rexnative_low_latency: how long before the display shows a frame "
                     "the GPU should have finished it, in microseconds")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(
    rexnative_mark_audit, 0, "ReXNative",
    "Debug, render thread mode: a mark (F12 or <capture dir>/capture.request) also "
    "logs the draws of the next this many frames to <capture dir>/draws-f<frame>.txt "
    "and audits them: each shader against the guest's ucode, the render thread's "
    "registers against the game thread's, and the shared memory each draw reads "
    "against guest memory as it was when the draw was queued (0: off)");
REXCVAR_DECLARE(std::string, rexnative_capture_dir);
REXCVAR_DECLARE(int32_t, rexnative_trace_record);
REXCVAR_DECLARE(int32_t, rexnative_texture_audit);
REXCVAR_DECLARE(int32_t, rexnative_d3d_stats_frames);
REXCVAR_DECLARE(bool, rexnative_dlss5_paused);
REXCVAR_DECLARE(int32_t, rexnative_dlss5_passes);
REXCVAR_DECLARE(bool, rexnative_luma_probe);

namespace rexnative {

struct NativeRenderer::Csv {
  std::unique_ptr<AsyncFile> file;
  // Window for the log summary.
  uint64_t frames = 0, draws = 0, draw_ns = 0, swap_ns = 0, frame_ns = 0, hook_ns = 0,
           hook_wait_ns = 0, generated = 0;
  // The window's hitches: what can stop a frame mid-game (logged when any happened).
  uint64_t pso_created = 0, held = 0, partial = 0, pso_wait_us = 0, translations = 0,
           slow_frames = 0, worst_frame_ns = 0;
  double translate_us = 0, translate_max_us = 0;
  // Profile: the window's stage ticks.
  uint64_t stage_ticks[profile::kStageCount] = {}, track_calls = 0;
  TextureCache::Stats tex_prev;                  // texture counters at the previous CSV row
  uint64_t evicted_prev = 0, evict_ns_prev = 0;  // the same for every row (tex_prev: profile)
  uint64_t occ_unknown_prev = 0;                 // OcclusionQueries::unknown_pairs() likewise
  bool luma = false;                             // --rexnative_luma_probe columns
  std::string line;                              // the row being written
};

namespace {
// Set once the device or swapchain failed; the native path stops.
std::atomic<bool> g_failed{false};

// The automatic frame latency settled on 2 (frame_pacer.h): the game's setting from now on, as
// if the player had picked 2 in the HUD, saved so the next start begins there.
void KeepAutoLatency() {
  rex::cvar::SetFlagByName("rexnative_frame_latency", "2");
  if (!hud::SaveSetting("rexnative_frame_latency", "2"))
    REXLOG_WARN("ReXNative: frame latency 2 kept for this run; the settings file was not written");
}

// --rexnative_debug_game_cost_us: a heavy game's logic on the game thread, for tests.
void DebugGameCost() {
  if (const int32_t cost = REXCVAR_GET(rexnative_debug_game_cost_us); cost > 0)
    for (const uint64_t until = NowNs() + uint64_t(cost) * 1000; NowNs() < until;) {}
}

// The game window (the SDK's display window). ReXGlue's Xenos driver presents into it unless
// the renderer does (OwnGameWindow).
HWND GameWindow() { return static_cast<HWND>(host::Window()); }

// `path`, or the first of <stem>.1<ext>, <stem>.2<ext>... that does not exist: a title launched
// from this one gets this process's arguments (games/common/title_launch.cpp), and PGR4 started
// again after Geometry Wars must not overwrite the first run's files.
std::string UnusedPath(const std::string& path) {
  std::filesystem::path p(path);
  std::error_code ec;
  for (int i = 1; std::filesystem::exists(p, ec); ++i) {
    p = path;
    p.replace_filename(p.stem().string() + "." + std::to_string(i) + p.extension().string());
  }
  return p.string();
}

bool FlagOn(const char* name) {
  const std::string v = rex::cvar::GetFlagByName(name);
  return v == "true" || v == "1";
}

// One column of the frame CSV: its name in the header and its value in this frame's row, so
// that the header and the rows come from one list.
struct CsvCell {
  enum Format : uint8_t { kUnsigned, kSigned, kHex4, kFixed1, kFixed3, kFixed4 };
  const char* name;
  Format format;
  uint64_t u;  // kUnsigned, kSigned (two's complement), kHex4
  double d;    // kFixed*: digits after the point
};
CsvCell Uint(const char* name, uint64_t v) { return {name, CsvCell::kUnsigned, v, 0.0}; }
CsvCell Int(const char* name, int64_t v) { return {name, CsvCell::kSigned, uint64_t(v), 0.0}; }
CsvCell Hex4(const char* name, uint64_t v) { return {name, CsvCell::kHex4, v, 0.0}; }
CsvCell Fix1(const char* name, double v) { return {name, CsvCell::kFixed1, 0, v}; }
CsvCell Fix3(const char* name, double v) { return {name, CsvCell::kFixed3, 0, v}; }
CsvCell Fix4(const char* name, double v) { return {name, CsvCell::kFixed4, 0, v}; }

// The cell's name (header) or value, after a comma unless it is the line's first.
void AppendCsvCell(const CsvCell& c, bool header, bool comma, std::string* line) {
  if (comma) line->push_back(',');
  if (header) {
    line->append(c.name);
    return;
  }
  char text[64];
  int n = 0;
  switch (c.format) {
    case CsvCell::kUnsigned:
      n = std::snprintf(text, sizeof(text), "%llu", (unsigned long long)c.u);
      break;
    case CsvCell::kSigned:
      n = std::snprintf(text, sizeof(text), "%lld", (long long)int64_t(c.u));
      break;
    case CsvCell::kHex4:
      n = std::snprintf(text, sizeof(text), "%04llX", (unsigned long long)c.u);
      break;
    case CsvCell::kFixed1:
      n = std::snprintf(text, sizeof(text), "%.1f", c.d);
      break;
    case CsvCell::kFixed3:
      n = std::snprintf(text, sizeof(text), "%.3f", c.d);
      break;
    case CsvCell::kFixed4:
      n = std::snprintf(text, sizeof(text), "%.4f", c.d);
      break;
  }
  line->append(text, size_t(std::clamp(n, 0, int(sizeof(text)) - 1)));
}

void AppendCsvCells(std::span<const CsvCell> cells, bool header, bool leading_comma,
                    std::string* line) {
  for (size_t i = 0; i < cells.size(); ++i)
    AppendCsvCell(cells[i], header, i || leading_comma, line);
}

// The Xenos driver's presenter on (connect) or off the game window: with
// gpu.xenos.present_external (sdk/patches) its present thread lets the window go, releasing its
// swap chain, at its next present, within about 100 ms.
void ConnectSdkPresenter(bool connect) {
  if (!rex::cvar::SetFlagByName("present_external", connect ? "false" : "true"))
    REXLOG_ERROR("ReXNative: could not set gpu.xenos.present_external (sdk/patches missing?)");
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Render thread mode (renderer.h).

struct NativeRenderer::Queue {
  // Commands per batch before the guest thread hands it over (and at every Swap). Each
  // hand-over can wake the render thread (a syscall), so batches are large.
  static constexpr size_t kBatchCommands = 256;

  enum Kind : uint8_t { kDraw, kClear, kResolve, kSwap, kQuery };
  struct Cmd {
    Kind kind;
    uint32_t index;    // into the batch's vector of that kind
    uint32_t reg_end;  // Batch::regs up to here are applied before the command
    uint32_t trace;    // its number + 1 in the trace being recorded (trace.h), else 0
  };
  struct DrawCmd {
    DrawState state;  // regs and immediate ucode pointers are filled in on the render thread
    DrawPrep prep;
    uint32_t copy_begin, copy_end;    // Batch::copies
    uint32_t audit_begin, audit_end;  // Batch::audits
    uint32_t memexport_begin, memexport_end;  // Batch::memexport_guest
  };
  struct SwapCmd {
    SwapCall call;
    // Guest-thread numbers of the frame, added to the render thread's before its CSV row.
    uint64_t hook_ns, hook_wait_ns, hook_max_ns, state_fail, track_calls, pad;
    uint64_t guest_begin_ns, guest_swap_ns, pace_wait_ns;
    DrawRenderer::PrepareCounters prep;
    uint64_t ticks[profile::kStageCount];
    uint32_t log_frames;  // a marked Swap with --rexnative_mark_audit: the frames to log
    bool classic;         // the Xenos plugin's frame (ExecuteClassicSwap), not a native one
  };
  struct Batch {
    std::vector<Cmd> cmds;
    RegLog regs;
    std::vector<VertexCopy> copies;
    std::vector<uint32_t> segments;  // indices into copies where a copy segment starts
    std::vector<AuditRange> audits;
    std::vector<uint8_t> memexport_guest;  // DrawPrep::memexport_guest
    std::vector<DrawCmd> draws;
    std::vector<ClearCall> clears;
    std::vector<ResolveParams> resolves;
    std::vector<SwapCmd> swaps;
    std::vector<QueryCall> queries;
    void Clear() {
      cmds.clear();
      regs.clear();
      copies.clear();
      segments.clear();
      audits.clear();
      memexport_guest.clear();
      draws.clear();
      clears.clear();
      resolves.clear();
      swaps.clear();
      queries.clear();
    }
  };

  std::mutex mutex;
  std::condition_variable work_cv, done_cv;
  std::deque<std::unique_ptr<Batch>> pending;  // guest -> render thread
  std::vector<std::unique_ptr<Batch>> spare;   // render -> guest thread
  uint64_t frames_done = 0;                    // Swaps the render thread finished (mutex)
  // Frames whose fence the GPU reached (render thread writes; DrawRenderer::Prepare's tags).
  std::atomic<uint64_t> gpu_frames_done{0};
  std::thread thread;

  // Guest thread. The tracker records the register changes straight into the current batch.
  std::unique_ptr<Batch> current = std::make_unique<Batch>();
  Queue() { GuestState().RecordInto(&current->regs); }
  uint64_t frame = 0;  // Swaps queued
  bool capture_armed = false;
  // Time in the On* calls this frame: the Swap's in ns, the others' in ticks, summed per call
  // and converted at the Swap.
  uint64_t hook_ns = 0, hook_wait_ns = 0, hook_max_ns = 0, state_fail = 0;
  uint64_t hook_ticks = 0, hook_max_ticks = 0;
  uint64_t begin_ns = 0, pace_wait_ns = 0;  // the frame being captured (FrameStats)
  void AddHook(uint64_t ticks) {
    hook_ticks += ticks;
    hook_max_ticks = std::max(hook_max_ticks, ticks);
  }

  // Render thread.
  uint32_t regs[kGuestRegisterCount] = {};
  std::deque<std::pair<uint64_t, uint64_t>> frame_fences;  // (frames submitted, their fence)

  void Publish() {
    if (trace::Recording()) [[unlikely]] {
      size_t count;
      if (const RegWrite* regs = TraceRegs(&count)) trace::Regs(regs, count);
    }
    trace_from = 0;
    std::unique_ptr<Batch> next;
    {
      std::lock_guard<std::mutex> lock(mutex);
      pending.push_back(std::move(current));
      if (!spare.empty()) {
        next = std::move(spare.back());
        spare.pop_back();
      }
    }
    work_cv.notify_one();
    current = next ? std::move(next) : std::make_unique<Batch>();
    GuestState().RecordInto(&current->regs);
  }
  // While a trace is recorded: the register writes since the batch's last command, for the
  // command about to be pushed.
  // The writes of a command the trace does not hold (an occlusion query) wait for the next one.
  size_t trace_from = 0;  // current->regs from here are not in the trace yet
  const RegWrite* TraceRegs(size_t* count) {
    const size_t from = std::min(trace_from, current->regs.size());
    trace_from = current->regs.size();
    *count = trace_from - from;
    return *count ? &current->regs[from] : nullptr;
  }
  void Push(Kind kind, uint32_t index, uint32_t traced = 0) {
    current->cmds.emplace_back(kind, index, uint32_t(current->regs.size()), traced);
    if (kind != kSwap && current->cmds.size() >= kBatchCommands) Publish();
  }
  // Blocks the guest thread until the render thread finished `frames` Swaps; returns the ns.
  uint64_t WaitFrames(uint64_t frames) {
    std::unique_lock<std::mutex> lock(mutex);
    if (frames_done >= frames) return 0;
    const uint64_t t0 = NowNs();
    done_cv.wait(lock, [&] { return frames_done >= frames; });
    return NowNs() - t0;
  }
};

uint32_t FrameHeight() {
  static const uint32_t height =
      REXCVAR_GET(rexnative) && !REXCVAR_GET(rexnative_classic)
          ? uint32_t(std::clamp<int32_t>(REXCVAR_GET(rexnative_render_height), 360, 2160))
          : kGameFrameHeight;
  return height;
}

std::string NativeRenderer::UpscalerStatus() { return Upscaler::Status(); }
uint32_t NativeRenderer::GpuVendor() { return D3D12Device::GpuVendor(); }

NativeRenderer* NativeRenderer::Get() {
  if (!(REXCVAR_GET(rexnative_beside_xenos) || REXCVAR_GET(rexnative)) || g_failed) return nullptr;
  // Never destroyed: the process exits with the game, and the guest thread may still be in a
  // hook during static destruction.
  static NativeRenderer* r = [] {
    auto* n = new NativeRenderer();
    if (!n->Initialize()) {
      REXLOG_ERROR("ReXNative: initialization failed; native path off for this run");
      g_failed = true;
      // host::Attach took the window from ReXGlue's presenter, which draws the game now.
      if (REXCVAR_GET(rexnative)) ConnectSdkPresenter(true);
      delete n;
      return static_cast<NativeRenderer*>(nullptr);
    }
    live_.store(n, std::memory_order_release);
    return n;
  }();
  return g_failed ? nullptr : r;
}

NativeRenderer::~NativeRenderer() {
  // The lists in flight and the calls the command stream holds use the modules' objects.
  if (device_) device_->WaitIdle();
  if (limit_timer_) CloseHandle(limit_timer_);
  if (switch_bound_) host::UnregisterBind("bind_renderer_switch");
  if (upscaler_bound_) {
    host::UnregisterBind("bind_upscaler");
    host::UnregisterBind("bind_upscaler_debug");
  }
}

bool NativeRenderer::Initialize() {
  LogOldNames();
  device_ = std::make_unique<D3D12Device>();
  uint32_t w = uint32_t(std::max(64, REXCVAR_GET(rexnative_width)));
  uint32_t h = uint32_t(std::max(64, REXCVAR_GET(rexnative_height)));
  const bool primary = primary_ = REXCVAR_GET(rexnative);
  // host::Attach took the window from ReXGlue's presenter already (present_external): a failure
  // or an XAM dialog before ReXNative's first frame on it gives it back (ReleaseGameWindow).
  window_taken_ = primary;
  const bool paced = primary && REXCVAR_GET(rexnative_frame_latency) > 0;
  // The pacer, or with the CSV only its log of the presents shown.
  const bool pacer = paced || (primary && !REXCVAR_GET(rexnative_csv).empty());
  HWND game_window = nullptr;
  if (primary) {
    // The first D3D call comes after the SDK opened its window; wait briefly in case not.
    for (int i = 0; i < 100 && !game_window; ++i) {
      game_window = GameWindow();
      if (!game_window) Sleep(50);
    }
    if (!game_window) {
      REXLOG_ERROR("ReXNative: --rexnative: no game window found");
      return false;
    }
    // The pacer needs FIFO: vkd3d-proton releases the frame latency waitable on the display's
    // present feedback only for capped present modes, and on the blit for the others. Without
    // the pacer, the same switch as the SDK presenter's.
    device_->set_vsync(paced || FlagOn("d3d12_present_vsync"));
    device_->set_frame_latency_waitable(pacer);
  }
  if (!device_->Initialize(w, h, primary ? L"ReXNative" : L"ReXNative (debug)", game_window))
    return false;
  frame_rate_requested_.store(true, std::memory_order_relaxed);
  if (REXCVAR_GET(rexnative_gpu_timing)) {
    if (device_->EnableGpuTimer()) {
      gpu_timer_ = device_->gpu_timer();
      gpu_timer_->set_per_shader(REXCVAR_GET(rexnative_gpu_timing_shaders));
    } else
      REXLOG_ERROR("ReXNative: GPU timing unavailable");
  }
  rt_ = std::make_unique<RenderTargets>();
  if (!rt_->Initialize(device_->device())) return false;
  rt_->SetResolutionScale(uint32_t(std::clamp(REXCVAR_GET(rexnative_resolution_scale), 1, 3)));
  if (rt_->resolution_scale() > 1)
    REXLOG_INFO("ReXNative: the screen-sized surfaces at {}x", rt_->resolution_scale());
  ReadSceneMsaa();
  resolver_ = std::make_unique<Resolver>();
  if (!resolver_->Initialize(device_->device(), rt_.get(), device_->command_queue())) return false;
  resolver_->set_gpu_timer(gpu_timer_);
  // The generated frames are paced by FIFO and the pacer's display clock (frame_generation.h):
  // with the pacer, frame generation is there to turn on and off in game (BeginFrame).
  if (paced) {
    frame_generation_ = std::make_unique<FrameGeneration>(*device_, *resolver_);
    if (!frame_generation_->Initialize()) {
      REXLOG_ERROR("ReXNative: frame generation unavailable");
      frame_generation_.reset();
    }
  } else if (FrameGeneration::Requested()) {
    REXLOG_ERROR("ReXNative: frame generation needs the frame pacer (--rexnative)");
  }
  // This port: DLSS 5 (neural.h), on the scene after DLAA or before the HUD; the upscaler takes
  // it as it is made.
  if (NeuralPass::Requested()) neural_ = std::make_unique<NeuralPass>(device_->device());
  // A scene learned at run time and not yet known: the upscaler is made when it is learned
  // (BeginFrameModules), as until then auto mode matches no surface; made here, it was made a
  // second time there, for the scene's size, with the GPU idle.
  if (!SceneAuto() || Scene().mask) SwitchUpscaler(Upscaler::Requested());
  textures_ = std::make_unique<TextureCache>(device_->device(), host::Memory());
  textures_->SetResolvedLookup([this](uint32_t a) { return resolver_->Find(a); });
  textures_->SetResolvedContaining(
      [this](uint32_t a, uint32_t* row) { return resolver_->FindContaining(a, row); });
  textures_->SetCaptureOrder(REXCVAR_GET(rexnative_texture_capture_order));
  resolver_->SetChangedCallback([this](uint32_t a) { textures_->OnResolved(a); });
  // The textures over a dropped image's memory are the guest's again: uploaded from it.
  resolver_->SetOverwrittenCallback(
      [this](uint32_t base, uint32_t bytes) { textures_->InvalidateWritten(base, bytes); });
  draw_ = std::make_unique<DrawRenderer>(*this);
  if (!draw_->Initialize()) return false;
  present_ = std::make_unique<Presenter>(*this);
  if (!present_->Initialize()) return false;
  if (primary) {
    classic_ = std::make_unique<ClassicFrames>();
    if (!classic_->Initialize(device_->device())) classic_.reset();
  }
  // Beside the Xenos plugin (debug window) the plugin's fake counts are the game's.
  if (primary && REXCVAR_GET(rexnative_occlusion)) {
    occlusion_ = std::make_unique<OcclusionQueries>();
    if (!occlusion_->Initialize(device_->device()) ||
        !rex::cvar::SetFlagByName("occlusion_query_external", "true")) {
      REXLOG_ERROR(
          "ReXNative: occlusion queries unavailable (patches/0049 missing?); every query "
          "reports its draw visible");
      occlusion_.reset();
    }
  }
  // Only once native is up: from here the Xenos plugin does no draw, copy or swap work, and its
  // command processor only runs fences, write-backs and interrupts.
  if (primary && !rex::cvar::SetFlagByName("gpu_skip_draws", "true"))
    REXLOG_ERROR(
        "ReXNative: could not set gpu_skip_draws (patches/0031 missing?); "
        "Xenos keeps drawing");
  // A handheld has no keyboard for the replay name prompt; the title's default name is what
  // confirming the dialog unchanged returns (PGR4Native's SDK patch; ReXGlue v0.11 has no such
  // setting, and its keyboard dialog takes the pad).
  if (primary) rex::cvar::SetFlagByName("xam_keyboard_accept_default", "true");
  // The keys are read when pressed: the Developer settings turn them off and on.
  const auto key = [](std::atomic<bool>& request) {
    return [&request] {
      if (REXCVAR_GET(rexnative_debug_keys)) request.store(true, std::memory_order_relaxed);
    };
  };
  if (primary && REXCVAR_GET(rexnative_renderer_switch)) {
    host::RegisterBind("bind_renderer_switch", "F9",
                          "ReXNative: switch between the native renderer and the Xenos plugin",
                          key(switch_requested_));
    switch_bound_ = true;
  }
  // The first Swap hands the game to the plugin.
  if (primary && REXCVAR_GET(rexnative_classic))
    switch_requested_.store(true, std::memory_order_relaxed);
  if (primary) {
    host::RegisterBind(
        "bind_upscaler", "F7",
        "ReXNative: switch the 3D scene between MSAA and FidelityFX Super Resolution",
        key(upscaler_switch_requested_));
    // This port: F8 cycles DLSS 5 (with --rexnative_dlss5) between the balanced settings
    // (Default, 1 pass), the strong ones (Natural, 2 passes) and off, live.
    host::RegisterBind(
        "bind_upscaler_debug", "F8",
        "ReXNative: DLSS 5 balanced (Default, 1 pass), strong (Natural, 2 passes) or off", [] {
          if (!REXCVAR_GET(rexnative_debug_keys)) return;
          const char* next;
          if (REXCVAR_GET(rexnative_dlss5_paused)) {
            rex::cvar::SetFlagByName("rexnative_dlss5_paused", "false");
            rex::cvar::SetFlagByName("rexnative_dlss5_style", "0");
            rex::cvar::SetFlagByName("rexnative_dlss5_passes", "1");
            rex::cvar::SetFlagByName("rexnative_dlss5_scale", "1");
            next = "balanced (Default, 1 pass, the full size)";
          } else if (REXCVAR_GET(rexnative_dlss5_passes) < 2) {
            rex::cvar::SetFlagByName("rexnative_dlss5_style", "1");
            rex::cvar::SetFlagByName("rexnative_dlss5_passes", "2");
            // Two full-size passes would run under 60 fps on an RTX 5060 Ti.
            rex::cvar::SetFlagByName("rexnative_dlss5_scale", "0.8");
            next = "strong (Natural, 2 passes, 0.8 of the size)";
          } else {
            rex::cvar::SetFlagByName("rexnative_dlss5_paused", "true");
            next = "off";
          }
          REXLOG_INFO("ReXNative DLSS 5: {} (F8)", next);
        });
    upscaler_bound_ = true;
  }
  csv_ = std::make_unique<Csv>();
  profile::enabled.store(REXCVAR_GET(rexnative_profile), std::memory_order_relaxed);
  const std::string path = REXCVAR_GET(rexnative_csv).empty()
                               ? std::string()
                               : UnusedPath(REXCVAR_GET(rexnative_csv));
  if (!path.empty()) {
    csv_->file = AsyncFile::Create(path);
    if (csv_->file) {
      csv_->luma = REXCVAR_GET(rexnative_luma_probe);
      std::string header;
      AppendCsvColumns(FrameStats{}, 0, true, &header);
      if (profile::On()) AppendCsvProfileColumns(true, &header);
      if (csv_->luma) AppendCsvLumaColumns(true, &header);
      header += '\n';
      csv_->file->Append(header);
    } else {
      REXLOG_ERROR("ReXNative: cannot open {}", path);
    }
  }
  if (pacer) {
    std::unique_ptr<AsyncFile> log;
    if (!path.empty()) {
      const std::string log_path =
          path.substr(0, path.size() - (path.ends_with(".csv") ? 4 : 0)) + "-presents.csv";
      if (!(log = AsyncFile::Create(log_path)))
        REXLOG_ERROR("ReXNative: cannot open {}", log_path);
    }
    pacer_ = std::make_unique<FramePacer>(FramePacer::Options{
        .frame_latency = uint32_t(std::max(0, REXCVAR_GET(rexnative_frame_latency))),
        .low_latency = REXCVAR_GET(rexnative_low_latency),
        .latency_margin_ns =
            uint64_t(std::max(0, REXCVAR_GET(rexnative_latency_margin_us))) * 1000,
        .log = std::move(log)});
    if (pacer_->frame_latency() > 0)
      REXLOG_INFO("ReXNative: frames paced on the display, at most {} begun but not shown",
                  pacer_->frame_latency());
  }
  frame_start_ns_ = NowNs();
  // A played trace is drawn by the thread that plays it (trace.h).
  if (REXCVAR_GET(rexnative_render_thread) && trace::PlayPath().empty()) {
    queue_ = std::make_unique<Queue>();
    queue_->thread = std::thread([this] { RenderThreadMain(); });
    REXLOG_INFO("ReXNative: recording on the render thread");
  }
  return true;
}

ID3D12Device* NativeRenderer::d3d12() const { return device_->device(); }
ID3D12CommandQueue* NativeRenderer::command_queue() const { return device_->command_queue(); }
CommandStream* NativeRenderer::command_list() const { return device_->command_list(); }
uint64_t NativeRenderer::frame_fence() const { return device_->frame_fence(); }

void NativeRenderer::SwitchUpscaler(bool on) {
  if (on && !upscaler_) {
    upscaler_ = std::make_unique<Upscaler>();
    upscaler_->set_output_scale(rt_->resolution_scale());
    if (!upscaler_->Initialize(device_->device())) {
      REXLOG_ERROR("ReXNative: upscaler unavailable; the scene keeps its MSAA");
      upscaler_.reset();
      Upscaler::NoteOn(false);
      return;
    }
    upscaler_->set_frame_generation(frame_generation_on_ ? frame_generation_.get() : nullptr);
    upscaler_->set_camera(&camera_);
    upscaler_->set_gpu_timer(gpu_timer_);
    upscaler_->set_neural(neural_.get());
  }
  upscaler_on_ = on;
  Upscaler::NoteOn(on);
  // The scene's targets are keyed by their sample count: the next draws make the other ones,
  // and the unused ones age out (RenderTargets::BeginFrame).
  // The scene (scene.h); none yet when it is learned at run time and was not seen yet: the
  // override then waits for it (SceneEndFrame asks for this again).
  const SceneSurface& scene = Scene();
  if (on) {
    rt_->OverrideScene(scene.mask, scene.value, 1, upscaler_->max_scale());
    rt_->SetSceneScale(upscaler_->scale());
    upscaler_->Reset();
  } else if (scene_msaa_ != 2) {
    const uint32_t s = rt_->resolution_scale();
    rt_->OverrideScene(scene.mask, scene.value, scene_msaa_, RenderScale{{s, s}, {1, 1}});
  } else {
    rt_->OverrideScene(0, 0, 1, {});
  }
  resolver_->set_upscaler(upscaler());
  if (on)
    REXLOG_INFO("ReXNative: the scene antialiased by {}", upscaler_->BackendName());
  else
    REXLOG_INFO("ReXNative: the scene with {}x MSAA", scene_msaa_);
}

void NativeRenderer::ReadSceneMsaa() {
  if (const int32_t msaa = REXCVAR_GET(rexnative_msaa);
      msaa == 1 || msaa == 2 || msaa == 4 || msaa == 8) {
    scene_msaa_ = rt_->SupportedSamples(uint32_t(msaa));
    if (scene_msaa_ != uint32_t(msaa))
      REXLOG_WARN("ReXNative: {}x MSAA unsupported; the scene takes {}x", msaa, scene_msaa_);
  } else {
    REXLOG_ERROR("ReXNative: --rexnative_msaa={} is not 1, 2, 4 or 8", msaa);
  }
}

void NativeRenderer::BeginFrameModules() {
  if (frame_rate_requested_.exchange(false, std::memory_order_relaxed))
    device_->set_present_interval(uint32_t(std::max(1, REXCVAR_GET(rexnative_present_interval))));
  // A scene learned at run time (scene.h): the upscaler and the scene's targets are made again
  // for it, as when the antialiasing changes.
  if (SceneEndFrame(frame_index_) && (upscaler_ || Upscaler::Requested() || scene_msaa_ != 2))
    antialiasing_requested_.store(true, std::memory_order_relaxed);
  // DLSS 5 turned on live (the settings HUD): its pass is made now and handed to the upscaler,
  // which DLSS 5 needs (made too when it was off). Off, the pass stays and stops running
  // (NeuralPass::Requested in SceneWanted and BeforeDraw).
  if (!neural_ && NeuralPass::Requested()) {
    neural_ = std::make_unique<NeuralPass>(device_->device());
    if (upscaler_) upscaler_->set_neural(neural_.get());
    if (!upscaler_on_) antialiasing_requested_.store(true, std::memory_order_relaxed);
    REXLOG_INFO("ReXNative DLSS 5: turned on at frame {}", frame_index_);
  }
  if (antialiasing_requested_.exchange(false, std::memory_order_relaxed)) {
    ReadSceneMsaa();
    // The upscaler takes its sizes when it is made (Upscaler::Initialize); its resources may be
    // in the lists still running.
    if (upscaler_) {
      device_->WaitIdle();
      upscaler_on_ = false;
      upscaler_.reset();
    }
    SwitchUpscaler(Upscaler::Requested());
  }
  if (upscaler_switch_requested_.exchange(false, std::memory_order_relaxed))
    SwitchUpscaler(!upscaler_on_);
  if (Upscaler* u = upscaler(); u && u->ContextOutdated()) {
    // FSR's context for the game's scene (HDR or not, its depth reversed or not), which the last
    // Upscale found: its old resources may be in the lists still running.
    device_->WaitIdle();
    if (!u->UpdateContext()) {
      REXLOG_ERROR("ReXNative: upscaler unavailable; the scene keeps its MSAA");
      upscaler_.reset();
      SwitchUpscaler(false);
    }
  }
  if (Upscaler* u = upscaler(); u && u->debug() != FfxDebug()) {
    // The context made again: its old resources may be in the lists still running.
    device_->WaitIdle();
    if (!u->SetDebug(FfxDebug())) {
      REXLOG_ERROR("ReXNative: upscaler unavailable; the scene keeps its MSAA");
      upscaler_.reset();
      SwitchUpscaler(false);
    }
  }
  // Frame generation, live (the settings HUD): the frame's choice, for the upscaler's Prepare
  // and its frame time; off, its context is dropped.
  const bool generate = frame_generation_ && FrameGeneration::Requested();
  if (generate != frame_generation_on_) {
    frame_generation_on_ = generate;
    if (frame_generation_ && !generate) frame_generation_->Release();
    if (upscaler_) upscaler_->set_frame_generation(generate ? frame_generation_.get() : nullptr);
    if (generate) REXLOG_INFO("ReXNative frame generation: on");
  }
  const uint64_t fence = device_->frame_fence(), done = device_->completed_fence();
  rt_->BeginFrame(frame_index_, fence, done);
  rt_->SuspendSceneOverride(false);  // the scene at the upscaler's size again (EnterPostScene)
  resolver_->set_linear_output(device_->hdr());
  resolver_->BeginFrame(frame_index_, fence, done);
  if (Upscaler* u = upscaler()) {
    // A frame's time on the display: refreshes at the rate the guest's vblanks run at (as
    // ReXGlue's xboxkrnl_video clamps it), two with a generated frame before it.
    const double hz =
        std::clamp(std::atof(rex::cvar::GetFlagByName("ui.video_mode_refresh_rate").c_str()),
                   24.0, 240.0);
    const uint32_t presents = frame_generation_on_ ? 2 : device_->present_interval();
    if (u->BeginFrame(frame_index_, device_->gpu_busy_ns(), uint64_t(1e9 * presents / hz)))
      rt_->SetSceneScale(u->scale());
  }
  frame_begun_ = true;
}

void NativeRenderer::RenderThreadMain() {
  SetThreadDescription(GetCurrentThread(), L"ReXNative render");
  Queue& q = *queue_;
  for (;;) {
    std::unique_ptr<Queue::Batch> b;
    {
      std::unique_lock<std::mutex> lock(q.mutex);
      q.work_cv.wait(lock, [&] { return !q.pending.empty(); });
      b = std::move(q.pending.front());
      q.pending.pop_front();
    }
    size_t reg = 0;
    uint32_t copies_issued = 0;
    size_t segment = 0;
    for (const Queue::Cmd& c : b->cmds) {
      stats_.reg_writes += c.reg_end - reg;
      for (; reg < c.reg_end; ++reg) q.regs[b->regs[reg].reg] = b->regs[reg].value;
      if (g_failed && c.kind != Queue::kSwap) continue;
      if (c.trace) [[unlikely]]
        trace::SetExecuting(c.trace);
      switch (c.kind) {
        case Queue::kDraw: {
          Queue::DrawCmd& d = b->draws[c.index];
          d.state.regs = q.regs;
          // A draw whose copies are not recorded yet is the first of its copy segment to
          // need any: it records the whole segment's (DrawRenderer::Prepare).
          uint32_t to = copies_issued;
          if (d.copy_end > copies_issued) {
            while (segment < b->segments.size() && b->segments[segment] <= copies_issued) ++segment;
            to = segment < b->segments.size() ? b->segments[segment] : uint32_t(b->copies.size());
            to = std::max(to, d.copy_end);
          }
          d.prep.copies = b->copies.data() + copies_issued;
          d.prep.copy_count = to - copies_issued;
          d.prep.audit = b->audits.data() + d.audit_begin;
          d.prep.audit_count = d.audit_end - d.audit_begin;
          d.prep.memexport_guest = b->memexport_guest.data() + d.memexport_begin;
          d.prep.memexport_guest_size = d.memexport_end - d.memexport_begin;
          copies_issued = to;
          ExecuteDraw(d.state, &d.prep);
          break;
        }
        case Queue::kClear:
          ExecuteClear(b->clears[c.index]);
          break;
        case Queue::kResolve:
          ExecuteResolve(b->resolves[c.index]);
          break;
        case Queue::kQuery:
          ExecuteQuery(b->queries[c.index]);
          break;
        case Queue::kSwap: {
          const Queue::SwapCmd& s = b->swaps[c.index];
          if (s.classic) {
            if (!g_failed) ExecuteClassicSwap(s.call);
          } else if (!g_failed) {
            stats_.hook_ns = s.hook_ns;
            stats_.hook_wait_ns = s.hook_wait_ns;
            stats_.hook_max_ns = s.hook_max_ns;
            stats_.pad = s.pad;
            stats_.guest_begin_ns = s.guest_begin_ns;
            stats_.guest_swap_ns = s.guest_swap_ns;
            stats_.pace_wait_ns = s.pace_wait_ns;
            stats_.state_fail += s.state_fail;
            draw_->AddPrepareCounters(s.prep);
            if (s.log_frames) draw_->LogFrames(s.log_frames);
            if (profile::On()) {
              for (uint32_t i = 0; i < profile::kStageCount; ++i) profile::g.ticks[i] += s.ticks[i];
              profile::g.track_calls += s.track_calls;
            }
            ExecuteSwap(s.call);
          }
          {
            std::lock_guard<std::mutex> lock(q.mutex);
            ++q.frames_done;
          }
          q.done_cv.notify_all();
          break;
        }
      }
    }
    b->Clear();
    std::lock_guard<std::mutex> lock(q.mutex);
    q.spare.push_back(std::move(b));
  }
}

void NativeRenderer::QueueDraw(const uint8_t* base, const HookArgs& args) {
  const uint64_t t0 = Ticks();
  profile::Begin();
  Queue& q = *queue_;
  Queue::Batch& b = *q.current;
  Queue::DrawCmd& d = b.draws.emplace_back();
  // The render thread wrote the next slot last (batches come back through `spare`): its cache
  // lines are fetched for writing while this draw is captured.
  for (size_t offset = 0; offset < sizeof(Queue::DrawCmd); offset += 64)
    __builtin_prefetch(reinterpret_cast<const char*>(&d + 1) + offset, 1);
  const bool ok = GuestState().ReadDrawState(base, args, &d.state);
  profile::Mark(profile::kReadState);
  if (!ok) {
    b.draws.pop_back();
    ++q.state_fail;
  } else {
    // The render thread records a batch's copies while it runs the batch: each batch starts
    // a copy segment.
    if (b.draws.size() == 1) draw_->BeginCopySegment();
    d.copy_begin = uint32_t(b.copies.size());
    d.audit_begin = uint32_t(b.audits.size());
    d.memexport_begin = uint32_t(b.memexport_guest.size());
    // Upload memory of frame F is tagged F + 1: free once F's fence completed.
    draw_->Prepare(d.state, q.frame + 1, &d.prep, &b.copies, &b.audits, &b.memexport_guest);
    d.copy_end = uint32_t(b.copies.size());
    d.audit_end = uint32_t(b.audits.size());
    d.memexport_end = uint32_t(b.memexport_guest.size());
    if (d.prep.copy_segment) b.segments.push_back(d.copy_begin);
    uint32_t traced = 0;
    if (trace::Recording()) [[unlikely]] {
      size_t count;
      const RegWrite* regs = q.TraceRegs(&count);
      traced = trace::Draw(d.state, d.state.regs, regs, count);
    }
    d.state.regs = nullptr;
    d.state.vs.immediate = d.state.ps.immediate = nullptr;  // Prepare loaded the shaders
    q.Push(Queue::kDraw, uint32_t(b.draws.size() - 1), traced);
    profile::Mark(profile::kCapRecord);
  }
  q.AddHook(Ticks() - t0);
}

uint32_t NativeRenderer::TraceClear(const ClearCall& c) {
  if (!trace::Recording()) [[likely]]
    return 0;
  size_t count;
  const RegWrite* regs = queue_->TraceRegs(&count);
  return trace::Clear(c, GuestState().regs(), regs, count);
}

uint32_t NativeRenderer::TraceResolve(const ResolveParams& p) {
  if (!trace::Recording()) [[likely]]
    return 0;
  size_t count;
  const RegWrite* regs = queue_->TraceRegs(&count);
  return trace::Resolve(p, GuestState().regs(), regs, count);
}

void NativeRenderer::QueueClear(const uint8_t* base, const HookArgs& args) {
  const uint64_t t0 = Ticks();
  profile::Scope scope(profile::kCapRecord);
  Queue& q = *queue_;
  Queue::Batch& b = *q.current;
  ClearCall& c = b.clears.emplace_back();
  if (RenderTargets::DecodeClear(base, args, &c)) {
    q.Push(Queue::kClear, uint32_t(b.clears.size() - 1), TraceClear(c));
    if (c.rest_from) KeepTileClearRest(c);
  } else {
    b.clears.pop_back();
  }
  q.AddHook(Ticks() - t0);
}

void NativeRenderer::KeepTileClearRest(const ClearCall& c) {
  tile_clear_rest_ = std::make_unique<ClearCall>(c);
  ClearCall& rest = *tile_clear_rest_;
  const LONG right = c.p.rects.empty() ? 0 : c.p.rects[0].right;
  rest.p.rects = {{0, LONG(c.rest_from), right, LONG(c.height)}};
  rest.rest_from = 0;
}

void NativeRenderer::OnTilingEnd() { tile_clear_rest_.reset(); }

void NativeRenderer::IssueTileClearRest() {
  if (!tile_clear_rest_) [[likely]]
    return;
  ClearCall rest = std::move(*tile_clear_rest_);
  tile_clear_rest_.reset();
  if (queue_) {
    Queue& q = *queue_;
    Queue::Batch& b = *q.current;
    b.clears.push_back(std::move(rest));
    q.Push(Queue::kClear, uint32_t(b.clears.size() - 1), TraceClear(b.clears.back()));
  } else if (!g_failed) {
    ExecuteClear(rest);
  }
}

void NativeRenderer::QueueResolve(const uint8_t* base, const HookArgs& args) {
  const uint64_t t0 = Ticks();
  profile::Scope scope(profile::kCapRecord);
  Queue& q = *queue_;
  Queue::Batch& b = *q.current;
  ResolveParams& p = b.resolves.emplace_back();
  if (DecodeResolve(base, args, &p))
    q.Push(Queue::kResolve, uint32_t(b.resolves.size() - 1), TraceResolve(p));
  else
    b.resolves.pop_back();  // logged once per reason by DecodeResolve
  q.AddHook(Ticks() - t0);
}

void NativeRenderer::QueueSwap(uint8_t* base, const HookArgs& args) {
  // The render thread failed a frame (Fail): what was queued since is dropped.
  if (g_failed) [[unlikely]]
    return HandOver(base, args.device);
  const uint64_t t0 = NowNs();
  Queue& q = *queue_;
  Queue::Batch& b = *q.current;
  Queue::SwapCmd& s = b.swaps.emplace_back();
  s.guest_begin_ns = q.begin_ns;
  s.guest_swap_ns = t0;
  s.pace_wait_ns = q.pace_wait_ns;
  Presenter::ReadSwap(base, args, &s.call);
  // Captures with the Xenos copy: from the Swap that arms one (HoldFullReadback) to the one
  // captured, the next shown (DrawRenderer::FrameIncomplete holds frames back), every Swap runs
  // before the guest continues, as without the render thread.
  bool sync = q.capture_armed, requested = false;
  if (sync) {
    s.call.capture_request = 0;
  } else {
    requested = present_->CaptureRequested(q.frame);
    s.call.capture_request = requested ? 1 : 0;
    sync = requested && present_->capture_holds_guest();
  }
  s.prep = draw_->EndFramePrepare(q.gpu_frames_done.load(std::memory_order_acquire));
  s.log_frames = requested ? uint32_t(std::max(0, REXCVAR_GET(rexnative_mark_audit))) : 0;
  if (s.log_frames) draw_->StartAudit(s.log_frames);
  // At most one frame ahead: frame F is queued once the render thread finished F - 1.
  const uint64_t wait = q.WaitFrames(q.frame);
  q.hook_wait_ns += wait;
  s.state_fail = q.state_fail;
  s.hook_wait_ns = q.hook_wait_ns;
  const uint64_t t1 = NowNs();
  s.hook_ns = q.hook_ns + TicksToNs(q.hook_ticks) + (t1 - t0);
  s.hook_max_ns = std::max({q.hook_max_ns, TicksToNs(q.hook_max_ticks), t1 - t0 - wait});
  s.track_calls = profile::g.track_calls;
  s.pad = rex_xam_take_frame_pad();
  const bool hand_over = s.call.hand_over = HandOverRequested() || AutoHandOverDue();
  std::memcpy(s.ticks, profile::g.ticks, sizeof(s.ticks));
  for (uint64_t& t : profile::g.ticks) t = 0;
  profile::g.track_calls = 0;
  q.state_fail = q.hook_ns = q.hook_wait_ns = q.hook_ticks = q.hook_max_ticks = 0;
  uint32_t traced = 0;
  if (trace::Recording()) [[unlikely]] {
    size_t count;
    const RegWrite* regs = q.TraceRegs(&count);
    traced = trace::Swap(s.call, GuestState().regs(), regs, count);
  }
  q.Push(Queue::kSwap, uint32_t(b.swaps.size() - 1), traced);
  q.Publish();
  ++q.frame;
  // A trace starts and ends between two frames, once the render thread ran what came before.
  if (trace::Recording() || REXCVAR_GET(rexnative_trace_record) > 0) [[unlikely]] {
    q.hook_wait_ns += q.WaitFrames(q.frame);
    trace::AtSwap(q.frame);
  }
  // Handing over and a capture's wait count towards the next frame.
  const uint64_t capture_wait =
      sync || REXCVAR_GET(rexnative_render_thread_lockstep) ? q.WaitFrames(q.frame) : 0;
  // The render thread has run this Swap: the capture is taken, or waits for a shown frame.
  if (sync) q.capture_armed = present_->capture_holding_guest();
  // The settings HUD's Frame latency, live.
  if (pacer_ && REXCVAR_GET(rexnative_frame_latency) > 0)
    pacer_->SetFrameLatency(uint32_t(REXCVAR_GET(rexnative_frame_latency)),
                            REXCVAR_GET(rexnative_frame_latency_auto));
  if (pacer_ && pacer_->TakeAutoDecision()) KeepAutoLatency();
  const uint64_t pace_wait = (pacer_ ? pacer_->WaitToBegin() : 0) + FrontEndLimit();
  DebugGameCost();
  const uint64_t t2 = NowNs();
  // The pacer's wait is the gap between two frames, not time in the hook.
  const uint64_t tail = t2 - t1 - pace_wait;
  q.hook_wait_ns = capture_wait;
  q.hook_ns = tail;
  q.hook_max_ns = tail - capture_wait;
  q.begin_ns = t2;
  q.pace_wait_ns = pace_wait;
  if (hand_over) HandOver(base, args.device);
}

// ---------------------------------------------------------------------------------------------
// Hook entry points.

void NativeRenderer::OnDraw(const uint8_t* base, const HookArgs& args) {
  ++frame_draw_calls_;
  IssueTileClearRest();
  if (queue_) return QueueDraw(base, args);
  if (g_failed) [[unlikely]]
    return;
  profile::Begin();
  DrawState state;
  const bool ok = GuestState().ReadDrawState(base, args, &state);
  profile::Mark(profile::kReadState);
  if (ok)
    ExecuteDraw(state, nullptr);
  else
    ++stats_.state_fail;
}

void NativeRenderer::OnClear(const uint8_t* base, const HookArgs& args) {
  IssueTileClearRest();
  if (queue_) return QueueClear(base, args);
  if (g_failed) [[unlikely]]
    return;
  if (ClearCall c; RenderTargets::DecodeClear(base, args, &c)) {
    ExecuteClear(c);
    if (c.rest_from) KeepTileClearRest(c);
  }
}

void NativeRenderer::OnResolve(const uint8_t* base, const HookArgs& args) {
  IssueTileClearRest();
  if (queue_) return QueueResolve(base, args);
  if (g_failed) [[unlikely]]
    return;
  if (ResolveParams p; DecodeResolve(base, args, &p)) ExecuteResolve(p);
}

void NativeRenderer::OnRegisterResolve(const ResolveRegisters& regs) {
  IssueTileClearRest();
  const uint8_t* physical = host::Memory()->GetPhysicalMembase();
  if (queue_) {
    const uint64_t t0 = Ticks();
    profile::Scope scope(profile::kCapRecord);
    Queue& q = *queue_;
    Queue::Batch& b = *q.current;
    ResolveParams& p = b.resolves.emplace_back();
    if (DecodeRegisterResolve(physical, regs, &p))
      q.Push(Queue::kResolve, uint32_t(b.resolves.size() - 1), TraceResolve(p));
    else
      b.resolves.pop_back();  // logged once per reason
    q.AddHook(Ticks() - t0);
    return;
  }
  if (g_failed) [[unlikely]]
    return;
  if (ResolveParams p; DecodeRegisterResolve(physical, regs, &p)) ExecuteResolve(p);
}

void NativeRenderer::OnRegisterClear(const ResolveRegisters& regs) {
  IssueTileClearRest();
  const uint8_t* physical = host::Memory()->GetPhysicalMembase();
  if (queue_) {
    const uint64_t t0 = Ticks();
    profile::Scope scope(profile::kCapRecord);
    Queue& q = *queue_;
    Queue::Batch& b = *q.current;
    ClearCall& c = b.clears.emplace_back();
    if (DecodeRegisterClear(physical, regs, &c))
      q.Push(Queue::kClear, uint32_t(b.clears.size() - 1), TraceClear(c));
    else
      b.clears.pop_back();  // logged once
    q.AddHook(Ticks() - t0);
    return;
  }
  if (g_failed) [[unlikely]]
    return;
  if (ClearCall c; DecodeRegisterClear(physical, regs, &c)) ExecuteClear(c);
}

uint64_t NativeRenderer::FrontEndLimit() {
  const int32_t fps = REXCVAR_GET(rexnative_menu_fps);
  const uint64_t now = NowNs();
  if (fps <= 0 || NeuralPass::CarsSimulated()) {
    limit_ns_ = now;
    return 0;
  }
  const uint64_t deadline = limit_ns_ + uint64_t(1'000'000'000 / std::clamp(fps, 10, 1000));
  if (now >= deadline) {
    // Behind (a slow frame, the first one): start over from now.
    limit_ns_ = now;
    return 0;
  }
  if (!limit_timer_)
    limit_timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
  LARGE_INTEGER due;
  due.QuadPart = -int64_t((deadline - now + 99) / 100);
  if (limit_timer_ && SetWaitableTimer(limit_timer_, &due, 0, nullptr, nullptr, FALSE))
    WaitForSingleObject(limit_timer_, INFINITE);
  else
    Sleep(DWORD((deadline - now) / 1'000'000));
  limit_ns_ = deadline;
  return NowNs() - now;
}

void NativeRenderer::OnSwap(uint8_t* base, const HookArgs& args) {
  tile_clear_rest_.reset();
  if (!trace::PlayPath().empty()) [[unlikely]]
    PlayTrace(base);  // never returns
  if (queue_) return QueueSwap(base, args);
  if (g_failed) [[unlikely]]
    return HandOver(base, args.device);
  if (REXCVAR_GET(rexnative_trace_record) > 0) [[unlikely]] {
    REXLOG_WARN("ReXNative trace: a frame trace is recorded with the render thread on "
                "(--rexnative_render_thread=true): none recorded");
    rex::cvar::SetFlagByName("rexnative_trace_record", "0");
  }
  stats_.guest_swap_ns = NowNs();
  SwapCall swap;
  Presenter::ReadSwap(base, args, &swap);
  stats_.pad = rex_xam_take_frame_pad();
  swap.hand_over = HandOverRequested() || AutoHandOverDue();
  ExecuteSwap(swap);
  // stats_ is the next frame's from here.
  if (pacer_ && REXCVAR_GET(rexnative_frame_latency) > 0)
    pacer_->SetFrameLatency(uint32_t(REXCVAR_GET(rexnative_frame_latency)),
                            REXCVAR_GET(rexnative_frame_latency_auto));
  if (pacer_ && pacer_->TakeAutoDecision()) KeepAutoLatency();
  stats_.pace_wait_ns = (pacer_ ? pacer_->WaitToBegin() : 0) + FrontEndLimit();
  DebugGameCost();
  stats_.guest_begin_ns = NowNs();
  if (swap.hand_over || g_failed) HandOver(base, args.device);
}

void NativeRenderer::OnQueryIssue(const uint8_t* base, const HookArgs& args) {
  // D3DQUERYTYPE_OCCLUSION at query+4, and D3DISSUE_END / D3DISSUE_BEGIN; QueryIssue takes BEGIN
  // when both are set.
  constexpr uint32_t kTypeOcclusion = 9, kIssueEnd = 1, kIssueBegin = 2;
  if (!occlusion_ || g_failed) return;
  const uint32_t query = args.r[0], flags = args.r[1];
  if (!query || Load32(base, query + 4) != kTypeOcclusion || !(flags & (kIssueBegin | kIssueEnd)))
    return;
  const QueryCall c = {query, !(flags & kIssueBegin)};
  if (!queue_) return ExecuteQuery(c);
  Queue::Batch& b = *queue_->current;
  b.queries.push_back(c);
  queue_->Push(Queue::kQuery, uint32_t(b.queries.size() - 1));
}

void NativeRenderer::OnQueryGetData(uint8_t* base, const HookArgs& args) {
  if (occlusion_) occlusion_->WriteResult(base, args.r[0], frame_index_);
}

void NativeRenderer::OnSwapSuspended() {
  const uint32_t draws = suspended_draws_.exchange(0, std::memory_order_relaxed);
  TakeChoice();
  if (crashed_.load(std::memory_order_relaxed)) {
    // NoteCrash: the native path does not come back (F9, the HUD, the automatic switch).
    wanted_.store(kWantNone, std::memory_order_relaxed);
    switch_requested_.store(false, std::memory_order_relaxed);
    if (classic_ && !g_failed) ClassicSwap();
    return;
  }
  if (SwitchRequested(/*native_now=*/false)) {
    auto_handed_over_ = false;
    auto_off_ = true;  // F9 decides from here on (HandOverRequested)
    // The choice as the settings HUD shows and saves it (its Renderer, rexnative.classic).
    rex::cvar::SetFlagByName("rexnative_classic", "false");
    return TakeBack();
  }
  if (classic_ && !g_failed) ClassicSwap();
  if (!auto_handed_over_) return;
  // The game's draw calls are back (its level after the menus): two frames of them in a row.
  // More than a few: the screens handed over can reach a hooked draw call or two that the
  // native renderer did not count as one (Orcs Must Die!'s title, 2 a frame, went back and
  // forth every second with `> 1`).
  busy_swaps_ = draws > kAutoTakeBackDraws ? busy_swaps_ + 1 : 0;
  if (busy_swaps_ < 2) return;
  busy_swaps_ = 0;
  auto_handed_over_ = false;
  REXLOG_INFO("ReXNative: {} draw calls in a frame again: native draws (rexnative.auto_handover)",
              draws);
  TakeBack();
}

// ---------------------------------------------------------------------------------------------
// Renderer switch (primary mode)

bool NativeRenderer::UnderWine() {
  static const bool wine = [] {
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version");
  }();
  return wine;
}

bool NativeRenderer::SwitchRefusedUnderWine() const {
  if (!UnderWine() || classic_) return false;
  REXLOG_WARN("ReXNative: renderer switch ignored under Wine: the window cannot change hands "
              "under vkd3d-proton, and the Xenos driver gives no frames to show through "
              "ReXNative's swap chain (rexnative.enabled = false starts the game on ReXGlue's "
              "renderer)");
  return true;
}

bool NativeRenderer::AutoHandOverDue() {
  const uint32_t draws = std::exchange(frame_draw_calls_, 0);
  const int32_t frames = REXCVAR_GET(rexnative_auto_handover);
  TakeChoice();
  if (frames <= 0 || !primary_ || auto_off_) return false;
  // Under Wine (the Steam Deck's Proton) the window does not change hands: once ReXNative's
  // swap chain was released, the Xenos driver's Present waited in vkd3d-proton for good and its
  // GPU thread with it (Orcs Must Die! black at its title). The window keeps ReXNative's swap
  // chain while the plugin draws (classic_); without that, the game's setting picks one renderer
  // for the whole run (<work>/deck.toml).
  if (UnderWine() && !classic_) {
    auto_off_ = true;
    REXLOG_WARN("ReXNative: rexnative.auto_handover off under Wine (the window cannot change "
                "hands under vkd3d-proton)");
    return false;
  }
  idle_swaps_ = draws <= 1 ? idle_swaps_ + 1 : 0;
  if (idle_swaps_ < uint32_t(frames)) return false;
  idle_swaps_ = 0;
  if (FrameHeight() != kGameFrameHeight) {
    auto_off_ = true;
    REXLOG_WARN("ReXNative: rexnative.auto_handover off: the Xenos plugin draws only the game's "
                "{}-row frame, not this {}-row one (frame_height.h)",
                kGameFrameHeight, FrameHeight());
    return false;
  }
  auto_handed_over_ = true;
  REXLOG_INFO("ReXNative: {} frames without a draw call the native renderer sees: the Xenos "
              "plugin draws until they come back (rexnative.auto_handover)",
              frames);
  return true;
}

void NativeRenderer::NoteCrash(const char* where) {
  if (crashed_.exchange(true, std::memory_order_relaxed)) return;
  const Fault fault = LastFault();
  REXLOG_ERROR("ReXNative: the native path read a bad guest address in {} (the fault was "
               "caught: {} of {:016X} by the code at +{:X} of the executable): this game's "
               "layer does not fit its D3D. The Xenos plugin draws from the next Swap, for the "
               "rest of the run",
               where, fault.write ? "a write" : "a read", fault.address, fault.code_offset);
}

bool NativeRenderer::HandOverRequested() {
  if (crashed_.load(std::memory_order_relaxed)) {
    // NoteCrash: whatever the frame height, a crash is worse.
    auto_off_ = true;
    rex::cvar::SetFlagByName("rexnative_classic", "true");
    return true;
  }
  if (!SwitchRequested(/*native_now=*/true)) return false;
  // F9 decides from here on: the automatic hand-over stays off for the rest of the run.
  auto_off_ = true;
  if (FrameHeight() == kGameFrameHeight) {
    // The choice as the settings HUD shows and saves it (its Renderer, rexnative.classic).
    rex::cvar::SetFlagByName("rexnative_classic", "true");
    return true;
  }
  REXLOG_WARN(
      "ReXNative: renderer switch: the Xenos plugin draws only the game's {}-row frame, not "
      "this {}-row one (frame_height.h); it draws from the start with the Classic renderer",
      kGameFrameHeight, FrameHeight());
  return false;
}

void NativeRenderer::HandOver(uint8_t* base, uint32_t device) {
  // Beside the Xenos plugin (debug window) it never stopped drawing; Fail cleared live_ already.
  if (!primary_) return;
  // The replaced draws (guest_d3d.h) cleared dirty bits without writing the state to the
  // stream, and the plugin skipped the shader loads: it has neither. The next draw writes all
  // of it. (Not with a device layout the layer does not know: game::kStreamOnly.)
  // After NoteCrash the layer's offsets are not this D3D's: writing the dirty masks there would
  // corrupt the device. The plugin then waits for the game's next state changes.
  if constexpr (!game::kStreamOnly)
    if (!crashed_.load(std::memory_order_relaxed)) MarkAllDirty(base, device);
  rex::cvar::SetFlagByName("gpu_skip_draws", "false");
  if (occlusion_) rex::cvar::SetFlagByName("occlusion_query_external", "false");
  // The game waits for the plugin's vblanks again: the plugin's frames are shown as they come
  // (through the renderer's swap chain with classic_, by ReXGlue's presenter without).
  paces_frames_.store(false, std::memory_order_relaxed);
  live_.store(nullptr, std::memory_order_relaxed);
  if (g_failed || crashed_.load(std::memory_order_relaxed))
    REXLOG_INFO("ReXNative: the Xenos plugin draws for the rest of the run");
  else
    REXLOG_INFO("ReXNative: renderer switch: the Xenos plugin draws");
}

void NativeRenderer::TakeBack() {
  // The tracker saw none of the calls the plugin drew. The next native frame's ExecuteSwap
  // takes the game window back (OwnGameWindow), and the pacing with it.
  GuestState().Resync();
  rex::cvar::SetFlagByName("gpu_skip_draws", "true");
  if (occlusion_) rex::cvar::SetFlagByName("occlusion_query_external", "true");
  // The window kept its swap chain (classic_): OwnGameWindow has nothing to take back, so the
  // pacing comes back here.
  if (classic_ && pacer_)
    paces_frames_.store(pacer_->pacing(), std::memory_order_relaxed);
  live_.store(this, std::memory_order_release);
  REXLOG_INFO("ReXNative: renderer switch: native draws");
}

void NativeRenderer::ClassicSwap() {
  SwapCall swap;
  // A capture of the plugin's frame, without the Xenos copy only (Presenter::OnSwap).
  swap.capture_request =
      !present_->capture_holds_guest() &&
      present_->CaptureRequested(queue_ ? queue_->frame : frame_index_);
  if (queue_) {
    Queue& q = *queue_;
    Queue::SwapCmd& s = q.current->swaps.emplace_back();
    s.call = swap;
    s.classic = true;
    // At most one frame ahead, as the native frames.
    q.WaitFrames(q.frame);
    q.Push(Queue::kSwap, uint32_t(q.current->swaps.size() - 1));
    q.Publish();
    ++q.frame;
  } else {
    ExecuteClassicSwap(swap);
  }
  // The frames shown stay within the frame latency, as the native ones (frame_pacer.h).
  if (pacer_ && REXCVAR_GET(rexnative_frame_latency) > 0)
    pacer_->SetFrameLatency(uint32_t(REXCVAR_GET(rexnative_frame_latency)),
                            REXCVAR_GET(rexnative_frame_latency_auto));
  if (pacer_ && pacer_->TakeAutoDecision()) KeepAutoLatency();
  if (pacer_) pacer_->WaitToBegin();
}

void NativeRenderer::ExecuteClassicSwap(const SwapCall& swap) {
  D3D12Device& d = *device_;
  // An XAM dialog is ReXGlue's presenter's overlay: the window is its own while one is up, as
  // while the native renderer draws.
  if (!OwnGameWindow(!host::XamUIActive())) return Fail();
  const ResolvedImage* image = d.has_swapchain() ? classic_->Take(d) : nullptr;
  const bool shown = image != nullptr;
  if (shown) {
    present_->OnSwap(swap, true, nullptr, image);
    classic_->Release(d);
  }
  const uint64_t fence = d.frame_fence();
  if (!d.EndFrame(shown)) return Fail();
  present_->AfterEndFrame();  // a capture's PNG
  const D3D12Device::EndFrameTimes& et = d.end_frame_times();
  if (pacer_ && et.presented)
    pacer_->Presented(fence, et.frame_presents);
  else if (pacer_)
    pacer_->RetireUnshown();
  ++frame_index_;
  d.SetTitleFrame(frame_index_);
  // The plugin's frames are not in the native stats; they get a line of their own, in the same
  // form, so a game on the classic renderer (from its start, or handed over after NoteCrash)
  // still says that it runs (the Gestor's boot test counts these lines). A gap of more than a
  // second (native frames between) is not counted.
  const uint64_t now = NowNs();
  if (classic_last_ns_ && now - classic_last_ns_ < 1000000000ull) {
    ++classic_frames_;
    classic_ns_ += now - classic_last_ns_;
  }
  classic_last_ns_ = now;
  if (const int32_t every = REXCVAR_GET(rexnative_d3d_stats_frames);
      every > 0 && classic_frames_ >= uint64_t(every)) {
    REXLOG_INFO("ReXNative: frames {} | per frame: draws 0.0 (the Xenos plugin draws the game), "
                "frame {:.2f} ms",
                frame_index_, double(classic_ns_) / 1e6 / double(classic_frames_));
    classic_frames_ = classic_ns_ = 0;
  }
  // The next native frame's time starts here.
  frame_start_ns_ = now;
  if (queue_) {
    // The native frames' upload memory is tagged with the frames queued (QueueDraw): these count.
    Queue& q = *queue_;
    const uint64_t completed = d.completed_fence();
    q.frame_fences.emplace_back(frame_index_, fence);
    while (!q.frame_fences.empty() && q.frame_fences.front().second <= completed) {
      q.gpu_frames_done.store(q.frame_fences.front().first, std::memory_order_release);
      q.frame_fences.pop_front();
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Recording (render thread, or the guest thread without it).

void NativeRenderer::ExecuteDraw(const DrawState& state, const DrawPrep* prep) {
  const uint64_t t0 = Ticks();
  profile::Begin();
  BeginFrame();
  ++stats_.draws;
  if (gpu_timer_) {
    uint64_t key;
    if (gpu_timer_->per_shader() && prep && prep->vs) {
      const auto [vs, ps] = DrawRenderer::ShaderHashes(*prep);
      key = gpu_timer_->KeyDrawShaders(state.regs, vs, ps);
    } else {
      key = GpuTimer::KeyDraw(state.regs);
    }
    gpu_timer_->Pass(device_->command_list(), key);
  }
  if (camera_scan_ && (upscaler_ || frame_generation_)) FindCamera(state);
  if (neural_ &&
      neural_->BeforeDraw(device_->command_list(), frame_index_, state.regs, *rt_, upscaler(),
                          prep ? DrawRenderer::ShaderHashes(*prep).second : 0))
    draw_->InvalidateListState();
  if (frame_generation_ && frame_generation_->Watching(frame_index_) &&
      frame_generation_->BeforeDraw(
          device_->command_list(), frame_index_, state.regs,
          prep ? DrawRenderer::ShaderHashes(*prep).second : 0,
          prep ? DrawRenderer::FirstTextureSize(state, *prep) : 0, [&] {
            const Upscaler* up = upscaler();
            using Stage = FrameGeneration::SceneStage;
            return !up || !up->upscaled_this_frame() ? Stage::kScene
                   : up->post_scene()                ? Stage::kPostScene
                                                     : Stage::kUpscaled;
          }()))
    draw_->InvalidateListState();
  if (dbg::Markers()) [[unlikely]]
    dbg::Pass(device_->command_list(), SurfaceRegsFromRegisters(state.regs));
  draw_->OnDraw(state, prep);
  SubmitEarly();
  draw_ticks_ += Ticks() - t0;
}

void NativeRenderer::FindCamera(const DrawState& state) {
  const uint32_t* regs = state.regs;
  if (!Upscaler::IsScene(regs[XE_GPU_REG_RB_SURFACE_INFO])) return;
  const reg::RB_DEPTHCONTROL depth{.value = regs[XE_GPU_REG_RB_DEPTHCONTROL]};
  if (!depth.z_enable || !depth.z_write_enable) return;
  --camera_scan_;
  // A perspective clip transform: four consecutive constant registers whose z row is a multiple
  // of their w row (fidelityfx.cpp). PGR4's vertex shaders keep it in c4-c7 (td-323); not every
  // game starts it at a multiple of four, so every start is tried.
  const uint32_t* c = regs + XE_GPU_REG_SHADER_CONSTANT_000_X;
  float m[16];
  for (uint32_t v = 0; v + 4 <= 256; ++v) {
    for (uint32_t i = 0; i < 16; ++i) m[i] = std::bit_cast<float>(c[4 * v + i]);
    if (!CameraFromClipRows(m, camera_)) {
      // Or kept as columns (Jet Set Radio's vertex shaders): the transpose's rows.
      float t[16];
      for (uint32_t i = 0; i < 16; ++i) t[i] = m[(i & 3) * 4 + (i >> 2)];
      if (!CameraFromClipRows(t, camera_)) continue;
      std::copy(t, t + 16, m);
    }
    camera_scan_ = 0;
    // A camera cut: the view turned further than a camera turns in one frame (25 degrees: a
    // fast turn at 30 FPS is a few). The upscaler and the frame generation start over, as
    // FidelityFX asks for a cut, instead of blending the frames of the view before it.
    const float wl = std::sqrt(m[12] * m[12] + m[13] * m[13] + m[14] * m[14]);
    if (wl > 0.0f) {
      const float d[3] = {m[12] / wl, m[13] / wl, m[14] / wl};
      constexpr float kCutCos = 0.9063f;  // cos(25 degrees)
      if (camera_frame_ + 1 == frame_index_ &&
          d[0] * camera_dir_[0] + d[1] * camera_dir_[1] + d[2] * camera_dir_[2] < kCutCos) {
        if (Upscaler* u = upscaler()) u->NoteCameraCut();
        if (++camera_cuts_ <= 8)
          REXLOG_INFO("ReXNative: camera cut at frame {} (the view turned by {:.0f} degrees)",
                      frame_index_,
                      std::acos(std::clamp(d[0] * camera_dir_[0] + d[1] * camera_dir_[1] +
                                               d[2] * camera_dir_[2],
                                           -1.0f, 1.0f)) * 57.2958f);
      }
      std::copy(d, d + 3, camera_dir_);
      camera_frame_ = frame_index_;
    }
    if (!camera_logged_) {
      camera_logged_ = true;
      REXLOG_INFO(
          "ReXNative: the scene's camera, from c{} of its vertex shaders: near {}, "
          "far {}, fov {:.1f} deg",
          v, camera_.near_plane, camera_.far_plane, camera_.fov_y * 57.2958f);
    }
    return;
  }
}

void NativeRenderer::ExecuteClear(const ClearCall& c) {
  const uint64_t t0 = Ticks();
  BeginFrame();
  ++stats_.clears;
  if (gpu_timer_) gpu_timer_->Pass(device_->command_list(), GpuTimer::KeyClear(c));
  draw_->LogClear(c);
  // A clear of the scene's surface after its upscaled resolve (Upscaler::EnterPostScene).
  if (Upscaler* u = upscaler(); u && Upscaler::IsScene(c.regs.surface_info)) {
    u->NoteSceneDraw(false);  // a new pass of the scene (Upscaler::SceneResolveDue)
    if (u->upscaled_this_frame() && !u->post_scene())
      u->EnterPostScene(device_->command_list(), *rt_, c.regs);
  }
  dbg::Event event(device_->command_list(), "clear flags {:X} colors {:X}{}", c.p.flags,
                   c.bound_colors, c.depth_bound ? " depth" : "");
  rt_->Clear(device_->command_list(), c.regs, c.bound_colors, c.depth_bound, c.height, c.p);
  clear_ticks_ += Ticks() - t0;
}

void NativeRenderer::ExecuteResolve(const ResolveParams& p) {
  const uint64_t t0 = Ticks();
  BeginFrame();
  ++stats_.resolves;
  if (gpu_timer_) gpu_timer_->Pass(device_->command_list(), GpuTimer::KeyResolve(p));
  draw_->LogResolve(p);
  if (frame_generation_) frame_generation_->OnResolve(p);
  if (neural_) neural_->OnResolve(p);
  if (dbg::Markers()) [[unlikely]] {
    dbg::EndPass(device_->command_list());
    dbg::BeginEvent(device_->command_list(), dbg::DescribeResolve(p));
  }
  // Sets its own compute root signature, pipeline and descriptor heap on the list.
  resolver_->Resolve(device_->command_list(), p);
  if (dbg::Markers()) [[unlikely]]
    dbg::EndEvent(device_->command_list());
  draw_->InvalidateListState();
  resolve_ticks_ += Ticks() - t0;
}

void NativeRenderer::ExecuteQuery(const QueryCall& q) {
  if (q.end)
    occlusion_->End(q.query, frame_index_, device_->frame_fence());
  else
    occlusion_->Begin(q.query);
}

// With one submission per frame the GPU idles from the end of a frame until the render thread
// has recorded all of the next one. Every --rexnative_submit_draws draws, the frame so far is
// submitted and recording goes on in a fresh list, where nothing bound carries over. The texture
// copies recorded so far read upload memory the untile workers fill: while untiles are still
// running, the next draw or resolve tries again.
void NativeRenderer::SubmitEarly() {
  const int32_t every = REXCVAR_GET(rexnative_submit_draws);
  if (every <= 0 || stats_.draws - draws_submitted_ < uint64_t(every) || !textures_->UploadsDone())
    return;
  profile::Scope scope(profile::kSubmit);
  if (!CloseUploadLists() || !device_->Submit(upload_lists_)) return Fail();
  draw_->InvalidateListState(DrawRenderer::ListBreak::kSubmit);
  draws_submitted_ = stats_.draws;
  ++stats_.submits;
}

bool NativeRenderer::SubmitAndWait(uint64_t* wait_ns) {
  profile::Scope scope(profile::kSubmit);
  textures_->FinishUploads();
  if (!CloseUploadLists() || !device_->Submit(upload_lists_)) {
    Fail();
    return false;
  }
  draw_->InvalidateListState(DrawRenderer::ListBreak::kSubmit);
  draws_submitted_ = stats_.draws;
  ++stats_.submits;
  const uint64_t t0 = NowNs();
  device_->WaitIdle();
  *wait_ns = NowNs() - t0;
  return true;
}

bool NativeRenderer::CloseUploadLists() {
  profile::Scope scope(profile::kUploadList);
  upload_lists_.clear();
  return textures_->CloseUploadLists(&upload_lists_);
}

void NativeRenderer::Fail() {
  g_failed = true;
  paces_frames_.store(false, std::memory_order_relaxed);
  if (pacer_) pacer_->Stop();
  REXLOG_ERROR("ReXNative: frame {} failed; native path off for the rest of the run",
               frame_index_);
  if (device_) LogDeviceRemoved(device_->device());
  if (!primary_) {
    live_.store(nullptr, std::memory_order_relaxed);
    return;
  }
  // The Xenos plugin skips its draws until the guest thread's next Swap hands the game back
  // (HandOver: it needs the guest device, between two of the guest's D3D calls); live_ stays
  // set so that the hooks reach that Swap.
  ReleaseGameWindow();
}

void NativeRenderer::ReleaseGameWindow() {
  D3D12Device& d = *device_;
  paces_frames_.store(false, std::memory_order_relaxed);
  if (d.has_swapchain()) {
    d.DetachSwapchain();
    if (pacer_) pacer_->Detach();
  }
  if (window_taken_) {
    ConnectSdkPresenter(true);
    window_taken_ = false;
    REXLOG_INFO("ReXNative: the game window shows ReXGlue's presenter");
  }
}

bool NativeRenderer::OwnGameWindow(bool own) {
  D3D12Device& d = *device_;
  // Without a swap chain the window may still be off ReXGlue's presenter (taken at start, before
  // ReXNative's first frame): letting it go connects the presenter again.
  if (own == d.has_swapchain() && (own || !window_taken_)) return true;
  if (!own) {
    ReleaseGameWindow();
    return true;
  }
  ConnectSdkPresenter(false);
  window_taken_ = true;
  // The Xenos driver lets the window go at its next present: until then the window has its swap
  // chain and making one fails.
  bool attached = false;
  for (int i = 0; i < 100 && !(attached = d.AttachSwapchain(/*quiet=*/i < 99)); ++i) Sleep(20);
  if (!attached) return false;
  if (pacer_) {
    HANDLE waitable =
        d.TakeFrameLatencyWaitable(FramePacer::kMaxFrameLatency * D3D12Device::kMaxFramePresents);
    if (!waitable) return false;
    pacer_->Attach(waitable, d.fence());
    // The pacing only while the native renderer draws: the window taken back after an XAM
    // dialog while the Xenos plugin draws (ExecuteClassicSwap) leaves the game waiting for the
    // plugin's vblanks, as HandOver set.
    paces_frames_.store(pacer_->pacing() && live_.load(std::memory_order_relaxed) != nullptr,
                        std::memory_order_relaxed);
  }
  return true;
}

void NativeRenderer::ExecuteSwap(const SwapCall& swap) {
  const uint64_t t0 = NowNs();
  BeginFrame();
  if (REXCVAR_GET(rexnative_texture_audit) >= 3) [[unlikely]] {
    rex::cvar::SetFlagByName("rexnative_texture_audit", "1");
    textures_->AuditGpu(device_->command_list(), [this] {
      uint64_t ns = 0;
      return SubmitAndWait(&ns);
    });
  }
  // What the frame's memexport draws wrote, into guest memory before the guest's next frame.
  draw_->FlushMemexport();
  D3D12Device& d = *device_;
  if (dbg::Markers()) [[unlikely]] {
    dbg::EndPass(d.command_list());
    dbg::BeginEvent(d.command_list(), std::format("swap: frame {}", frame_index_));
  }
  if (gpu_timer_) gpu_timer_->Pass(d.command_list(), GpuTimer::KeyPresent());
  // A scene's first frame without the draws whose pipelines were compiling: the previous frame
  // stays on screen while the next one waits for them.
  const bool incomplete = draw_->FrameIncomplete();
  if (incomplete) ++stats_.frames_held;
  // Handing over, the window keeps the swap chain when the plugin's frames go through it.
  if (primary_ && !OwnGameWindow((!swap.hand_over || classic_) && !host::XamUIActive()))
    return Fail();
  const bool shown = !incomplete && d.has_swapchain();
  FrameGeneration* const fg = frame_generation_.get();
  const bool generate = shown && fg && fg->prepared(frame_index_);
  camera_scan_ = (upscaler_ || frame_generation_on_) ? 8 : 0;  // FindCamera's budget, next frame
  SwapCopy hudless;
  if (generate) {
    d.SetFrameImage(fg->image(), fg->image_rtv());
    if (const ResolvedImage* source = fg->hudless_source(frame_index_))
      hudless = {source, fg->hudless(), fg->hudless_rtv()};
  }
  // The resolved front buffer into the back buffer (PRESENT in and out).
  present_->OnSwap(swap, shown, hudless.image ? &hudless : nullptr);
  if (fg) fg->OnSwap(present_->front_buffer_base());
  if (neural_) neural_->OnSwap(present_->front_buffer_base());
  if (generate) {
    stats_.generated = 1;
    const ImageRect& r = present_->image_rect();
    Upscaler::SceneInputs scene;
    if (Upscaler* up = upscaler()) scene = up->ReadSceneInputs(d.command_list());
    d.SetGeneratedFrame(fg->Generate(d.command_list(), frame_index_,
                                     {r.left, r.top, r.width, r.height}, hudless.image,
                                     scene.motion, scene.width, scene.height));
    if (hudless.image) fg->MeasureHudless(d.command_list(), frame_index_, d.frame_fence());
    // A capture takes this frame's and the next 7 frames' images, one after the other (the
    // presenter's own captures skip a frame while one is written).
    if (present_->capturing(frame_index_) && !fg_captures_left_) fg_captures_left_ = 8;
    if (fg_captures_left_) {
      --fg_captures_left_;
      // With the scene's motion vectors (the frame generation's input), still this frame's.
      ID3D12Resource* motion = nullptr;
      if (Upscaler* up = upscaler()) motion = up->ReadSceneInputs(d.command_list()).motion;
      fg->Capture(d.command_list(), frame_index_, d.frame_fence(),
                  REXCVAR_GET(rexnative_capture_dir), hudless.image != nullptr, motion);
    }
  }
  // The frame's texture copies read upload memory the untile workers fill.
  const uint64_t u0 = NowNs();
  textures_->FinishUploads();
  stats_.swap_untile_ns = NowNs() - u0;
  draw_->BeforeSubmit();
  const uint64_t fence = d.frame_fence();
  if (occlusion_) occlusion_->Resolve(d.command_list(), fence);
  if (dbg::CaptureNext(frame_index_, present_->capturing(frame_index_)))
    d.CaptureNextFrame(frame_index_ + 1);
  // --rexnative_debug_frame_cost_us: a heavy frame's render thread time, for tests.
  if (const int32_t cost = REXCVAR_GET(rexnative_debug_frame_cost_us); cost > 0)
    for (const uint64_t until = NowNs() + uint64_t(cost) * 1000; NowNs() < until;) {}
  if (!CloseUploadLists() || !d.EndFrame(shown, upload_lists_)) return Fail();
  const D3D12Device::EndFrameTimes& et = d.end_frame_times();
  stats_.swap_submit_ns = et.submit_ns;
  stats_.swap_present_ns = et.present_ns;
  stats_.swap_fence_ns = et.fence_ns;
  stats_.stream_ns = et.stream_ns;
  stats_.stream_wait_ns = et.stream_wait_ns;
  stats_.present = et.presented ? et.presents : 0;
  if (pacer_ && et.presented)
    pacer_->Presented(fence, et.frame_presents);
  else if (pacer_)
    pacer_->RetireUnshown();
  ++frame_index_;
  frame_begun_ = false;
  d.SetTitleFrame(frame_index_);
  const uint64_t completed = d.completed_fence();
  if (queue_) {
    // Frees the guest thread's vertex upload memory of finished frames (Prepare's tags).
    Queue& q = *queue_;
    q.frame_fences.emplace_back(frame_index_, fence);
    while (!q.frame_fences.empty() && q.frame_fences.front().second <= completed) {
      q.gpu_frames_done.store(q.frame_fences.front().first, std::memory_order_release);
      q.frame_fences.pop_front();
    }
  }
  textures_->Retire(completed);
  if (REXCVAR_GET(rexnative_texture_reload)) {
    rex::cvar::SetFlagByName("rexnative_texture_reload", "false");
    textures_->InvalidateWritten(0, 0x20000000u);
    REXLOG_INFO("ReXNative textures: every texture uploads again (rexnative_texture_reload)");
  }
  if (REXCVAR_GET(rexnative_texture_audit) >= 2) [[unlikely]] {
    rex::cvar::SetFlagByName("rexnative_texture_audit", "1");
    textures_->Audit();
  }
  if (frame_generation_) frame_generation_->FinishCaptures(completed);
  if (occlusion_) occlusion_->Retire(completed);
  draw_->EndFrame(completed);
  resolver_->AfterSwap();
  present_->AfterEndFrame();
  const uint64_t t1 = NowNs();
  stats_.swap_ns = t1 - t0;
  stats_.draw_ns = TicksToNs(draw_ticks_);
  stats_.clear_ns = TicksToNs(clear_ticks_);
  stats_.resolve_ns = TicksToNs(resolve_ticks_);
  draw_ticks_ = clear_ticks_ = resolve_ticks_ = 0;
  if (!queue_)
    stats_.hook_ns = stats_.draw_ns + stats_.clear_ns + stats_.resolve_ns + stats_.swap_ns;
  WriteCsvRow(stats_, t1 - frame_start_ns_);
  AddToStatsLog(stats_, t1 - frame_start_ns_);
  ResetFrameCounters();
  frame_start_ns_ = t1;
  stats_ = FrameStats{};
  draws_submitted_ = 0;
}

void NativeRenderer::AppendCsvColumns(const FrameStats& s, uint64_t frame_ns, bool header,
                                      std::string* line) {
  const DrawRenderer::Stats& ds = draw_->stats();
  const TextureCache::Stats& ts = textures_->stats();
  const CsvCell cells[] = {
      Uint("frame", frame_index_ - 1),
      Uint("draws", s.draws),
      Uint("clears", s.clears),
      Uint("resolves", s.resolves),
      Uint("state_fail", s.state_fail),
      Fix1("draw_us", s.draw_ns / 1000.0),
      Fix1("clear_us", s.clear_ns / 1000.0),
      Fix1("resolve_us", s.resolve_ns / 1000.0),
      Fix1("swap_us", s.swap_ns / 1000.0),
      Fix4("us_per_draw", s.draws ? double(s.draw_ns) / 1000.0 / double(s.draws) : 0.0),
      Fix3("frame_ms", frame_ns / 1e6),
      Uint("host_draws", ds.draws),
      Uint("skipped", ds.skipped),
      Uint("unsupported", ds.unsupported),
      Uint("failed", ds.failed),
      Uint("pso_created", ds.pso_created),
      Fix1("upload_kb", ds.upload_bytes / 1024.0),
      Uint("textures", ds.textures),
      Uint("textures_missing", ds.textures_missing),
      Uint("front_buffer", present_->front_buffer_found() ? 1 : 0),
      Fix1("hook_us", s.hook_ns / 1000.0),
      Fix1("hook_wait_us", s.hook_wait_ns / 1000.0),
      Uint("reg_writes", s.reg_writes),
      Fix1("swap_untile_us", s.swap_untile_ns / 1000.0),
      Fix1("swap_submit_us", s.swap_submit_ns / 1000.0),
      Fix1("swap_present_us", s.swap_present_ns / 1000.0),
      Fix1("swap_fence_us", s.swap_fence_ns / 1000.0),
      Fix1("hook_max_us", s.hook_max_ns / 1000.0),
      Uint("watch_faults", ds.watch_faults),
      Fix3("gpu_prev_ms", gpu_timer_ ? gpu_timer_->last_frame_ms() : 0.0),
      Uint("copy_batches", ds.copy_batches),
      Uint("watch_arms", ds.watch_arms),
      Uint("watch_arm_pages", ds.watch_arm_pages),
      Uint("watch_arm_changed", ds.watch_arm_changed),
      Fix1("watch_arm_us", ds.watch_arm_ns / 1000.0),
      Fix3("t_ms", NowNs() / 1e6),
      Uint("hash_pages", ds.hash_pages),
      Uint("hash_copies", ds.hash_copies),
      Uint("pso_pending", ds.pso_pending),
      Uint("pso_wait_us", ds.pso_wait_us),
      Uint("frames_partial", ds.frames_partial),
      Uint("held", s.frames_held),
      Uint("tex_stale", ds.textures_stale),
      Uint("coh_stale", ds.stale_pages),
      Uint("coh_late", ds.late_pages),
      Uint("coh_gpu", ds.gpu_mismatch_pages),
      Uint("coh_regs", ds.reg_mismatches),
      Fix1("tex_mb", textures_->resident_bytes() / 1048576.0),
      Uint("tex_count", textures_->texture_count()),
      Uint("tex_evicted", ts.evicted - csv_->evicted_prev),
      Fix1("tex_evict_us", (ts.evict_ns - csv_->evict_ns_prev) / 1000.0),
      Uint("views", ds.views),
      Uint("pso_generic", ds.pso_generic),
      Uint("occ_unknown", occlusion_ ? occlusion_->unknown_pairs() - csv_->occ_unknown_prev : 0),
      // rex_xam_take_frame_pad's layout.
      Hex4("pad_buttons", s.pad & 0xFFFF),
      Uint("pad_lt", uint8_t(s.pad >> 16)),
      Uint("pad_rt", uint8_t(s.pad >> 24)),
      Int("pad_lx", int8_t(s.pad >> 32)),
      Int("pad_ly", int8_t(s.pad >> 40)),
      Int("pad_rx", int8_t(s.pad >> 48)),
      Int("pad_ry", int8_t(s.pad >> 56)),
      Uint("submits", s.submits),
      Fix3("guest_begin_ms", s.guest_begin_ns / 1e6),
      Fix3("guest_swap_ms", s.guest_swap_ns / 1e6),
      Fix1("pace_wait_us", s.pace_wait_ns / 1000.0),
      Uint("present", s.present),
      Fix3("gpu_busy_ms", device_->gpu_busy_ns() / 1e6),
      Uint("render_height", upscaler() ? upscaler()->render_height() : 0),
      Fix1("stream_us", s.stream_ns / 1000.0),
      Fix1("stream_wait_us", s.stream_wait_ns / 1000.0),
  };
  AppendCsvCells(cells, header, false, line);
}

void NativeRenderer::AppendCsvProfileColumns(bool header, std::string* line) {
  for (uint32_t i = 0; i < profile::kStageCount; ++i) {
    if (header)
      *line += std::string(",st_") + profile::kStageNames[i] + "_us";
    else
      AppendCsvCell(Fix1("", double(TicksToNs(profile::g.ticks[i])) / 1000.0), false, true, line);
  }
  const DrawRenderer::Stats& ds = draw_->stats();
  const TextureCache::Stats& t = textures_->stats();
  const TextureCache::Stats& p = csv_->tex_prev;
  const CsvCell cells[] = {
      Uint("track_calls", profile::g.track_calls),
      Uint("tex_memo_miss", t.memo_misses - p.memo_misses),
      Uint("tex_created", t.created - p.created),
      Fix1("tex_create_us", (t.create_ns - p.create_ns) / 1000.0),
      Uint("tex_uploads", t.uploads - p.uploads),
      Uint("tex_deferred", t.deferred_uploads - p.deferred_uploads),
      Fix1("tex_upload_kb", (t.upload_bytes - p.upload_bytes) / 1024.0),
      Fix1("tex_upload_us", (t.upload_ns - p.upload_ns) / 1000.0),
      Fix1("tex_untile_us", (t.untile_ns - p.untile_ns) / 1000.0),
      Fix1("tex_record_us", (t.upload_record_ns - p.upload_record_ns) / 1000.0),
      Uint("tex_resolved_copies", t.resolved_copies - p.resolved_copies),
      Fix1("tex_copy_resolved_us", (t.copy_resolved_ns - p.copy_resolved_ns) / 1000.0),
      Uint("tex_resolved_aliases", t.resolved_aliases - p.resolved_aliases),
      Uint("srv_lookups", ds.srv_lookups),
      Uint("srv_created", ds.srv_created),
      Uint("samplers_created", ds.samplers_created),
      Uint("bind_hits", ds.bind_memo_hits),
      Uint("bind_misses", ds.bind_memo_misses),
  };
  AppendCsvCells(cells, header, true, line);
  for (uint32_t i = 0; i < profile::kCounterCount; ++i)
    AppendCsvCell(Uint(profile::kCounterNames[i], profile::g.counters[i]), header, true, line);
}

void NativeRenderer::AppendCsvLumaColumns(bool header, std::string* line) {
  const LumaProbe& l = present_->luma_probe();
  AppendCsvCell(Uint("luma_frame", l.frame), header, true, line);
  static constexpr const char* kRowNames[LumaProbe::kRows] = {"top", "mid", "low"};
  for (uint32_t row = 0; row < LumaProbe::kRows; ++row) {
    for (uint32_t s = 0; s < LumaProbe::kSegments; ++s) {
      if (header)
        *line += ",luma_" + std::string(kRowNames[row]) + "_" + std::to_string(s);
      else
        AppendCsvCell(Fix1("", l.luma[row][s]), false, true, line);
    }
  }
}

void NativeRenderer::WriteCsvRow(const FrameStats& s, uint64_t frame_ns) {
  Csv& c = *csv_;
  if (!c.file) return;
  c.line.clear();
  AppendCsvColumns(s, frame_ns, false, &c.line);
  c.evicted_prev = textures_->stats().evicted;
  c.evict_ns_prev = textures_->stats().evict_ns;
  c.occ_unknown_prev = occlusion_ ? occlusion_->unknown_pairs() : 0;
  if (profile::On()) {
    AppendCsvProfileColumns(false, &c.line);
    c.tex_prev = textures_->stats();
  }
  if (c.luma) AppendCsvLumaColumns(false, &c.line);
  c.line += '\n';
  c.file->Append(c.line);
}

void NativeRenderer::AddToStatsLog(const FrameStats& s, uint64_t frame_ns) {
  Csv& w = *csv_;
  ++w.frames;
  w.draws += s.draws;
  w.draw_ns += s.draw_ns;
  w.swap_ns += s.swap_ns;
  w.frame_ns += frame_ns;
  w.hook_ns += s.hook_ns;
  w.hook_wait_ns += s.hook_wait_ns;
  w.generated += s.generated;
  {
    const DrawRenderer::Stats& ds = draw_->stats();
    w.pso_created += ds.pso_created;
    w.held += s.frames_held;
    w.partial += ds.frames_partial;
    w.pso_wait_us += ds.pso_wait_us;
    w.translations += ds.translations;
    w.translate_us += ds.translate_us;
    w.translate_max_us = std::max(w.translate_max_us, ds.translate_max_us);
    w.worst_frame_ns = std::max(w.worst_frame_ns, frame_ns);
    if (frame_ns > 50'000'000) ++w.slow_frames;
  }
  if (profile::On()) {
    for (uint32_t i = 0; i < profile::kStageCount; ++i) w.stage_ticks[i] += profile::g.ticks[i];
    w.track_calls += profile::g.track_calls;
  }
  const int32_t every = REXCVAR_GET(rexnative_d3d_stats_frames);
  if (every <= 0 || w.frames < uint64_t(every)) return;
  const double n = double(w.frames);
  // From a frame beginning (the game reading its pad) to the display showing it, paced frames.
  const double shown_ms = pacer_ ? pacer_->TakeShownLatencyMs() : 0.0;
  REXLOG_INFO(
      "ReXNative: frames {} | per frame: draws {:.1f}, draw CPU {:.1f} us "
      "({:.4f} us/draw), swap {:.1f} us, frame {:.2f} ms, game thread {:.1f} us "
      "(waiting {:.1f} us), begin to shown {:.1f} ms (latency {}){}",
      frame_index_, w.draws / n, w.draw_ns / 1000.0 / n,
      w.draws ? w.draw_ns / 1000.0 / double(w.draws) : 0.0, w.swap_ns / 1000.0 / n,
      w.frame_ns / 1e6 / n, w.hook_ns / 1000.0 / n, w.hook_wait_ns / 1000.0 / n, shown_ms,
      pacer_ ? pacer_->frame_latency() : 0,
      frame_generation_on_ || w.generated
          ? std::format(", frame generation: {} of {} frames ({} reset)", w.generated, w.frames,
                        frame_generation_ ? frame_generation_->TakeResets() : 0)
          : std::string());
  // What stopped frames in this window, when anything did (the Steam Deck's logs say which):
  // new pipelines and frames held for them or shown without some draws, shaders translated on
  // the render thread, upload blocks created during play, frames over 50 ms.
  {
    const uint32_t grown = UploadPool::grown_blocks.exchange(0, std::memory_order_relaxed);
    const uint64_t grown_ns = UploadPool::grown_ns.exchange(0, std::memory_order_relaxed);
    if (w.pso_created || w.held || w.partial || w.translations || grown || w.slow_frames) {
      REXLOG_INFO(
          "ReXNative hitches: {} new pipelines, {} frames held ({:.1f} ms waiting), {} shown "
          "without some draws; {} shaders translated ({:.1f} ms, longest {:.1f} ms); {} upload "
          "blocks created ({:.1f} ms); {} frames over 50 ms (worst {:.1f} ms)",
          w.pso_created, w.held, w.pso_wait_us / 1000.0, w.partial, w.translations,
          w.translate_us / 1000.0, w.translate_max_us / 1000.0, grown, grown_ns / 1e6,
          w.slow_frames, w.worst_frame_ns / 1e6);
    }
  }
  // The audio mix over the same frames: underruns are periods a stream had nothing queued (a
  // gap, a click), the sound the Steam Deck's players heard as missing or odd.
  if (host::AudioStats a; host::GetAudioStats(&a)) {
    static uint64_t last_underruns = 0;
    REXLOG_INFO("ReXNative audio: {} streams, {} frames queued, {} underruns in these frames, "
                "latency {:.1f} ms, {} output channels",
                a.streams, a.queued, a.underruns - last_underruns, a.latency_us / 1000.0,
                a.channels);
    last_underruns = a.underruns;
  }
  if (profile::On() && w.draws) {
    // ns per draw for each stage (the hook-side ones are per draw too, for comparison).
    std::string line;
    char item[64];
    for (uint32_t i = 0; i < profile::kStageCount; ++i) {
      std::snprintf(item, sizeof(item), " %s %.0f", profile::kStageNames[i],
                    double(TicksToNs(w.stage_ticks[i])) / double(w.draws));
      line += item;
    }
    REXLOG_INFO("ReXNative profile: ns/draw{} | track calls/frame {:.0f}", line,
                double(w.track_calls) / n);
    for (uint64_t& t : w.stage_ticks) t = 0;
    w.track_calls = 0;
  }
  if (gpu_timer_) {
    gpu_timer_->Log();
    for (uint64_t hash : gpu_timer_->TakeNewTopShaders())
      REXLOG_INFO("ReXNative GPU: shader {:016X} ucode:\n{}", hash,
                  draw_->ShaderDisassembly(hash));
  }
  w.frames = w.draws = w.draw_ns = w.swap_ns = w.frame_ns = w.hook_ns = w.hook_wait_ns =
      w.generated = 0;
  w.pso_created = w.held = w.partial = w.pso_wait_us = w.translations = w.slow_frames =
      w.worst_frame_ns = 0;
  w.translate_us = w.translate_max_us = 0;
}

void NativeRenderer::ResetFrameCounters() {
  draw_->ResetStats();
  if (profile::On()) {
    for (uint64_t& t : profile::g.ticks) t = 0;
    profile::g.track_calls = 0;
    for (uint64_t& c : profile::g.counters) c = 0;
  }
}
}  // namespace rexnative
