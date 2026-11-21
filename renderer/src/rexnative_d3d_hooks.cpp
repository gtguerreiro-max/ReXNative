// ReXNative renderer: guest D3D API hooks and frame trace.
//
// Every function in d3d_surface.inc (the XDK D3D entry points PGR4's game code calls, plus the
// internal draw builders and MakeSpace) is overridden with a REX_HOOK_RAW that forwards to the
// original __imp__sub_X and counts the call (the pass-through ones only while the tap or the
// profiler is on). For debugging and oracle comparisons, forwarding can be switched off per class
// at run time (`skip draw|clear|resolve|none`). The native renderer as the presenter
// (--rexnative) does not use it: it keeps forwarding and stops the Xenos plugin's host
// draws with the SDK's --gpu_skip_draws (patches/0031).
//
// With --rexnative_d3d_tap the hooks also follow the D3D push buffer: the D3D device keeps its write
// pointer at device+48 (last written dword) and its limit at device+56, and every packet the
// library emits is written there before the GPU sees it. At the entry and exit of each device
// call the new dwords are decoded (PM4 types 0-3), so register, constant, shader and draw state
// is known per API call, without running or parsing the GPU side. Each draw packet is counted
// and, on traced frames, written with its shaders, fetch constants, render-target registers and
// a cross-check of the decoded registers against the device's own register shadow.
//
// Control at run time: --rexnative_d3d_control=<file>. The file is read (then deleted) once per
// frame, one command per line:
//   trace <tag> [frames]   write the next N complete frames to --rexnative_d3d_trace_dir (ignored
//                          while the tap is off)
//   skip <draw,clear,resolve|none>   (on the presenting device only, see MayDropPackets)
//   tap on|off             stop or resume decoding (hooks keep counting and forwarding); off
//                          also drops the frames a trace has still to write
//   cmdbuf 0|1|2           recorded command buffers off, counted or replayed, from the next
//                          frame (native/cmd_replay.h)
//   cmdbuf log N           log the next N RunCommandBuffer calls in detail
//   cmdbuf dump [tag]      write the next two decoded calls, packet by packet, to
//                          --rexnative_d3d_trace_dir
//   pad <A+START|none> [lx=N] [ly=N] [ms]   hold buttons (A B X Y START BACK UP DOWN LEFT RIGHT
//                          LB RB LS RS) and the left stick on user 0's pad for ms (150), in what
//                          the game's XInputGetState returns (when the layer found it): no window
//                          focus needed
//   renderer native|stock  as the renderer switch (F9): the native renderer or the Xenos plugin
//                          draws from the next Swap on, without the window in front
//   watch <address> [nan|any] [count]   a hardware breakpoint on one guest dword (native/watch.h):
//                          logs the recompiled functions that write it (a NaN, or any value);
//                          watch off clears it
//   set <setting> <value>  a run-time setting, as on the command line (set rexnative_dlss5_look cel):
//                          the live ones take effect at once, as from the settings HUD
// --rexnative_d3d_tap, --rexnative_d3d_tap_check and --rexnative_d3d_skip set the state at the first device the
// game creates; the one it creates at the title screen keeps what the commands changed since.
//
// With --rexnative_beside_xenos the draw, clear, resolve and Swap hooks also call the native D3D12
// renderer (src/native/renderer.h) after the guest function returned.
//
// Built into every game by rexnative_add() (renderer/rexnative.cmake), with the game's layer.

#include "native/guard.h"
#include "native/watch.h"
#include "native/clock.h"
#include "native/cmd_replay.h"
#include "native/d3d_state.h"
#include "native/guest_d3d.h"
#include "native/guest_memory.h"
#include "native/log.h"
#include "native/profile.h"
#include "native/renderer.h"
#include "native/resolve.h"
#include "hud/hud.h"
#include "game_layer.h"  // this game's layer (rexnative adapt): addresses, offsets

#include <rex/cvar.h>
#include "native/sdk.h"
#include <rex/ppc/hook.h>
#include <rex/logging.h>
#include "host.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <numeric>
#include <sstream>
#include <string>
#include <unordered_map>
#include <thread>
#include <vector>

REXCVAR_DEFINE_BOOL(rexnative_d3d_tap, false, "ReXNative",
                    "Decode the guest D3D push buffer at each hooked D3D call (trace/oracle)");
REXCVAR_DEFINE_BOOL(rexnative_inline_draws, false, "ReXNative",
                    "Draws the game wrote past the hooked D3D functions (inlined by its build: "
                    "Orcs Must Die!'s title and menus): the push buffer between two hooked "
                    "calls is walked as a command buffer and its draws drawn");
REXCVAR_DEFINE_BOOL(rexnative_inline_draws_auto, true, "ReXNative",
                    "Tries rexnative.inline_draws by itself after 30 frames in a row with at most "
                    "2 hooked draw calls and 8 KB or more of push buffer written, for 60 frames: "
                    "kept on for the rest of the run if the walk drew something (a game drawing "
                    "its screens with code inlined past the hooks), off again otherwise; and "
                    "every 150 frames probes the push buffer between hooked calls for 3 frames, "
                    "keeping it on if the game's own code loads shader constants there (XDK "
                    "GPU-owned constants: Deus Ex: Human Revolution); once kept, the game's "
                    "<cache>/rexnative/inline_draws.txt turns it on from the start of the next "
                    "runs (delete the file to learn again)");
REXCVAR_DEFINE_STRING(rexnative_d3d_control, "", "ReXNative",
                      "Command file polled once per frame (trace/skip/tap commands)");
REXCVAR_DEFINE_STRING(rexnative_d3d_trace_dir, "", "ReXNative", "Directory for frame traces");
REXCVAR_DEFINE_STRING(rexnative_d3d_skip, "", "ReXNative",
                      "Classes not forwarded to the guest D3D at start: draw,clear,resolve");
REXCVAR_DEFINE_STRING(rexnative_d3d_frame_csv, "", "ReXNative",
                      "Per-frame counter CSV (frame, calls, draw calls, tap draws, ...)");
REXCVAR_DECLARE(bool, rexnative_profile);
REXCVAR_DECLARE(bool, rexnative_inline_resolves);
REXCVAR_DEFINE_BOOL(rexnative_resolve_check_registers, false, "ReXNative",
                    "With the tap on: decode each hooked Resolve's copy again from the "
                    "registers its packets set (DecodeRegisterResolve, which "
                    "rexnative.inline_resolves uses) and log where the two decodes disagree. "
                    "The tap's register view can be older than the GPU's (SSX: a 4x surface "
                    "of half the pitch, which ReXGlue's own decode rarely sees): a right edge "
                    "cut to the pitch or the scissor points there");
REXCVAR_DEFINE_INT32(rexnative_d3d_stats_frames, 150, "ReXNative",
                     "Frames per summary line in the log (0 = off)");
REXCVAR_DEFINE_BOOL(rexnative_d3d_tap_check, false, "ReXNative",
                    "With --rexnative_d3d_tap: compare the tracked state with the stream at every draw "
                    "of every frame and log the frames that differ (slow)");
REXCVAR_DEFINE_STRING(rexnative_d3d_tap_check_range, "", "ReXNative",
                      "With --rexnative_d3d_tap_check: \"first-last\" registers (hex) alone, each "
                      "draw whose tracked values there differ from the stream's logged (4070-415F: "
                      "vertex shader constants c28-c87)");

namespace {

namespace ucode = rex::graphics::ucode;
namespace xenos = rex::graphics::xenos;

enum Cat : uint8_t {
  kDraw,
  kResolve,
  kSwap,
  kPush,
  kSync,
  kConst,
  kShader,
  kTexture,
  kStream,
  kSurface,
  kState,
  kGet,
  kClear,
  kTiling,
  kQuery,
  kDevice,
  kResource,
  kOther,
};

enum FnId : uint32_t {
#define D3D_FN(a, n, c) kFn_##a,
#include "d3d_surface.inc"
#undef D3D_FN
  kFnCount
};

struct FnInfo {
  uint32_t addr;
  const char* name;
  Cat cat;
};

constexpr FnInfo kFns[kFnCount] = {
#define D3D_FN(a, n, c) {0x##a##u, n, c},
#include "d3d_surface.inc"
#undef D3D_FN
};

// Ids of the hooked functions in d3d_surface.inc, by their address in game_layer.h; kFnCount
// (no hook has it) when the game has none.
constexpr FnId FnIdOf(uint32_t addr) {
  if (addr)
    for (uint32_t i = 0; i < kFnCount; ++i)
      if (kFns[i].addr == addr) return FnId(i);
  return kFnCount;
}
namespace game = rexnative::game;
constexpr FnId kFnSwap = FnIdOf(game::kFnSwap), kFnResolve = FnIdOf(game::kFnResolve),
               kFnCreateDevice = FnIdOf(game::kFnCreateDevice),
               kFnRestoreSegment13392 = FnIdOf(game::kFnRestoreSegment13392),
               kFnRestoreSegment = FnIdOf(game::kFnRestoreSegment),
               kFnStartSegment = FnIdOf(game::kFnStartSegment),
               kFnSwitchSegment = FnIdOf(game::kFnSwitchSegment),
               kFnMakeSpace = FnIdOf(game::kFnMakeSpace),
               kFnBeginVertices = FnIdOf(game::kFnBeginVertices),
               kFnRestorePush = FnIdOf(game::kFnRestorePush),
               kFnDrawVerticesUP = FnIdOf(game::kFnDrawVerticesUP),
               kFnBeginIndexedVertices = FnIdOf(game::kFnBeginIndexedVertices),
               kFnDrawVertices = FnIdOf(game::kFnDrawVertices),
               kFnDrawIndexed = FnIdOf(game::kFnDrawIndexedVertices),
               kFnClear = FnIdOf(game::kFnClear), kFnBeginTiling = FnIdOf(game::kFnBeginTiling),
               kFnEndTiling = FnIdOf(game::kFnEndTiling),
               kFnQueryIssue = FnIdOf(game::kFnQueryIssue),
               kFnQueryGetData = FnIdOf(game::kFnQueryGetData);

// The five draw builders, which every game draw goes through.
constexpr FnId kDrawCounterFns[] = {kFnBeginVertices, kFnDrawVertices, kFnDrawIndexed,
                                    FnIdOf(game::kFnDrawIndx2Inline),
                                    FnIdOf(game::kFnDrawIndx2Split)};
constexpr bool IsDrawCounterFn(FnId id) {
  for (FnId d : kDrawCounterFns)
    if (d == id) return true;
  return false;
}
// The calls rex_drawcount_api_counters reports: the draw builders and Resolve.
constexpr bool IsCountedFn(FnId id) { return IsDrawCounterFn(id) || id == kFnResolve; }

// RunCommandBuffer (native/cmd_replay.h): kFnCount, and every path for it compiled out, when the
// game has no hook for it.
constexpr FnId kFnRunCommandBuffer = FnIdOf(game::kFnRunCommandBuffer);

// Skippable entry points: void, and nothing in the caller depends on their result.
enum Skip : uint32_t { kSkipDraw = 1, kSkipClear = 2, kSkipResolve = 4 };
// Not a switch: the functions a game lacks all have the id kFnCount, which would repeat a case.
constexpr uint32_t SkipClassOf(FnId id) {
  if (id == kFnCount) return 0;
  if (id == kFnDrawIndexed || id == kFnDrawVertices || id == kFnDrawVerticesUP) return kSkipDraw;
  if (id == kFnClear) return kSkipClear;
  if (id == kFnResolve) return kSkipResolve;
  return 0;
}

// The game's time step: the time between two frames' begins, which the Swap hook returns at
// (after the frame pacer's wait in the native renderer, the plugin's vblank wait otherwise); the
// game reads its timer just after. It draws a frame while it runs the next, so the frame it draws
// moved by the step before the last: that one matches the game's within 0.11 ms in the median and
// 1.4 ms in 99 % of the frames at 90 fps (td-125). NoteFrameBegin runs on the thread that calls
// Swap; the motion blur pass reads g_drawn_step_ns.
uint64_t g_frame_begin_ns = 0;
int64_t g_frame_step_ns = 0;
std::atomic<int64_t> g_drawn_step_ns{0};
// The drawn step smoothed over ~1/2 s (an EMA whose weight follows the frame's length). The
// motion blur's strength means to match the sustained frame rate, a slow quantity; the raw
// step's jitter was invisible in a race, where the reprojection dominates, but in pause the
// reprojection is zero and the jitter was the whole of the visible blur: the still frame pumped
// (issue #1).
std::atomic<int64_t> g_smooth_step_ns{0};

void NoteFrameBegin() {
  const uint64_t now = rexnative::NowNs();
  g_drawn_step_ns.store(g_frame_step_ns, std::memory_order_relaxed);
  if (g_frame_begin_ns) {
    g_frame_step_ns = int64_t(now - g_frame_begin_ns);
    double smooth = double(g_smooth_step_ns.load(std::memory_order_relaxed));
    // Clamped like the reader's: a load's long frame must not drag the average up. The first
    // measured frame starts the average at itself, not at a ramp from zero.
    const int64_t sample = std::min<int64_t>(g_frame_step_ns, 100'000'000);
    if (smooth == 0) smooth = double(sample);
    smooth += (sample - smooth) * (sample / 500'000'000.0);
    g_smooth_step_ns.store(int64_t(smooth), std::memory_order_relaxed);
  }
  g_frame_begin_ns = now;
}

constexpr uint32_t kRegCount = rexnative::kGuestRegisterCount;
namespace dev = rexnative::dev;
// First register of each constant kind in SET_CONSTANT and LOAD_ALU_CONSTANT: ALU (float),
// fetch, bool, loop, register.
constexpr uint32_t kConstantBase[] = {0x4000, 0x4800, 0x4900, 0x4908, 0x2000};

using rexnative::Load32;  // the D3D's push buffer can live in the 0xE0000000 view

uint64_t Fnv(const uint32_t* d, size_t n, uint64_t h = 1469598103934665603ull) {
  for (size_t i = 0; i < n; ++i) {
    h ^= d[i];
    h *= 1099511628211ull;
  }
  return h;
}

const char* RegName(uint32_t r) {
  static const std::unordered_map<uint32_t, const char*> names = [] {
    std::unordered_map<uint32_t, const char*> m;
#define REXGPU_REGISTER(index, type, name) m.emplace(index, #name);
#include <rex/xenos/register_table.inc>
#undef REXGPU_REGISTER
    return m;
  }();
  auto it = names.find(r);
  return it == names.end() ? nullptr : it->second;
}

const char* OpName(uint32_t op) {
  switch (op) {
    case 0x10:
      return "NOP";
    case 0x21:
      return "REG_RMW";
    case 0x22:
      return "DRAW_INDX";
    case 0x23:
      return "VIZ_QUERY";
    case 0x26:
      return "WAIT_FOR_IDLE";
    case 0x27:
      return "IM_LOAD";
    case 0x2B:
      return "IM_LOAD_IMMEDIATE";
    case 0x2D:
      return "SET_CONSTANT";
    case 0x2F:
      return "LOAD_ALU_CONSTANT";
    case 0x34:
      return "DRAW_INDX_BIN";
    case 0x35:
      return "DRAW_INDX_2_BIN";
    case 0x36:
      return "DRAW_INDX_2";
    case 0x37:
      return "INDIRECT_BUFFER_PFD";
    case 0x3B:
      return "INVALIDATE_STATE";
    case 0x3C:
      return "WAIT_REG_MEM";
    case 0x3D:
      return "MEM_WRITE";
    case 0x3E:
      return "REG_TO_MEM";
    case 0x3F:
      return "INDIRECT_BUFFER";
    case 0x44:
      return "COND_EXEC";
    case 0x45:
      return "COND_WRITE";
    case 0x46:
      return "EVENT_WRITE";
    case 0x48:
      return "ME_INIT";
    case 0x4A:
      return "SET_SHADER_BASES";
    case 0x4B:
      return "SET_BIN_BASE_OFFSET";
    case 0x50:
      return "SET_BIN_MASK";
    case 0x51:
      return "SET_BIN_SELECT";
    case 0x52:
      return "WAIT_REG_EQ";
    case 0x53:
      return "WAIT_REG_GTE";
    case 0x54:
      return "INTERRUPT";
    case 0x55:
      return "SET_CONSTANT2";
    case 0x56:
      return "SET_SHADER_CONSTANTS";
    case 0x58:
      return "EVENT_WRITE_SHD";
    case 0x59:
      return "EVENT_WRITE_CFL";
    case 0x5A:
      return "EVENT_WRITE_EXT";
    case 0x5B:
      return "EVENT_WRITE_ZPD";
    case 0x5C:
      return "WAIT_UNTIL_READ";
    case 0x5D:
      return "WAIT_IB_PFD_COMPLETE";
    case 0x5E:
      return "CONTEXT_UPDATE";
    case 0x60:
      return "SET_BIN_MASK_LO";
    case 0x61:
      return "SET_BIN_MASK_HI";
    case 0x62:
      return "SET_BIN_SELECT_LO";
    case 0x63:
      return "SET_BIN_SELECT_HI";
    case 0x64:
      return "XE_SWAP";
    default:
      return nullptr;
  }
}

// Fetch constants a shader reads (texture fetch slots, vertex fetch constants).
struct ShaderInfo {
  uint32_t type = 0, addr = 0, dwords = 0;
  uint64_t hash = 0;
  uint32_t tfetch_mask = 0;  // bit i: tf[i]
  uint64_t vfetch_lo = 0;    // bit i: vf[i], i < 64
  uint32_t vfetch_hi = 0;    // bit i: vf[64 + i]
  uint32_t uses = 0;
};

void ScanUcode(const uint32_t* host_le, uint32_t dwords, ShaderInfo& info) {
  uint32_t first_exec = dwords / 3;
  for (uint32_t pair = 0; pair * 3 + 3 <= dwords && pair < first_exec; ++pair) {
    ucode::ControlFlowInstruction cf[2];
    ucode::UnpackControlFlowInstructions(host_le + pair * 3, cf);
    bool end = false;
    for (auto& c : cf) {
      auto op = c.GetOpcode();
      if (!ucode::IsControlFlowOpcodeExec(op)) continue;
      uint32_t addr = c.exec.GetAddress(), count = c.exec.GetCount(), seq = c.exec.GetSequence();
      if (count) first_exec = std::min(first_exec, addr);
      for (uint32_t k = 0; k < count; ++k) {
        if (!((seq >> (k * 2)) & 1)) continue;  // ALU
        uint32_t at = (addr + k) * 3;
        if (at + 3 > dwords) break;
        ucode::FetchInstruction fi;
        std::memcpy(&fi, host_le + at, sizeof(fi));
        if (fi.GetOpcode() == ucode::FetchOpcode::kVertexFetch) {
          uint32_t vf = fi.GetVertexFetch().GetFetchConstantIndex();
          if (vf < 64)
            info.vfetch_lo |= 1ull << vf;
          else if (vf < 96)
            info.vfetch_hi |= 1u << (vf - 64);
        } else {
          info.tfetch_mask |= 1u << (fi.GetTextureFetch().GetFetchConstantIndex() & 31);
        }
      }
      end |= ucode::IsControlFlowOpcodeEnd(op);
    }
    if (end) break;
  }
}

struct FrameCounters {
  uint64_t calls = 0, draw_calls = 0;  // hooked calls; DrawCounter's five builders
  uint64_t tap_draws = 0, tap_aux = 0, tap_resolves = 0;  // predicated / other draw packets
  uint64_t dwords = 0, segments = 0, resyncs = 0, torn = 0, unknown_ops = 0, ibs = 0, zero_pad = 0;
  uint64_t checked = 0, differ = 0;  // --rexnative_d3d_tap_check: draws compared, draws that differ
};

// Belongs to the thread that drives the guest D3D device: the game uses the device from one
// thread at a time (renderer.h), and only the hooked calls on that device and CreateDevice, which
// runs before the calls on the device it creates, change this state. Hooked calls on other
// threads (r3 not the device) read `device` and `enabled` only.
struct Tap {
  uint32_t device = 0;
  uint32_t cursor = 0;
  bool enabled = false;
  bool configured = false;  // the cvars were read (at the first CreateDevice)

  // Decoder.
  uint32_t regs[kRegCount] = {};
  uint8_t written[kRegCount] = {};
  std::vector<uint32_t> pkt;
  uint32_t header = 0, need = 0;
  bool in_packet = false;
  uint32_t vs_addr = 0, vs_dwords = 0, ps_addr = 0, ps_dwords = 0;
  uint64_t vs_imm_hash = 0, ps_imm_hash = 0;  // IM_LOAD_IMMEDIATE

  FrameCounters frame;
  uint64_t frame_index = 0;

  // Tracing.
  bool tracing = false;     // the current frame is being written
  uint32_t trace_left = 0;  // frames still to trace after this one
  std::string trace_tag;
  std::string out;
  uint64_t trace_frame = 0, call_seq = 0, draw_seq = 0;
  // Oracle for native/d3d_state: its GPU view vs the decoded stream at every top-level draw.
  uint64_t state_draws = 0, state_bad_draws = 0, state_shader_bad = 0, state_unchecked = 0;
  uint64_t state_details = 0, state_per_tile = 0;
  bool state_pending = false;  // BeginVertices: compared when RestorePush publishes its packets
  bool check = false;          // --rexnative_d3d_tap_check
  std::string check_first;     // the frame's first differing draw
  std::vector<uint64_t> state_regs = std::vector<uint64_t>(kRegCount);
  std::map<uint32_t, uint64_t> op_counts;
  std::map<uint64_t, ShaderInfo> shaders;                      // key: type<<63 | addr<<20 ^ dwords
  std::map<std::pair<uint32_t, uint32_t>, uint64_t> textures;  // (dword1, dword2) -> draws
  uint64_t fn_calls[kFnCount] = {};
};

Tap g_tap;
std::atomic<uint64_t> g_calls[kFnCount + 1];  // (+1: kFnCount, functions the game lacks) IsCountedFn's, for rex_drawcount_api_counters
std::atomic<uint32_t> g_skip{0};
// The control file's `pad`: buttons (XINPUT_GAMEPAD wButtons) and, with bit 48, the left stick
// (lx in bits 16-31, ly in 32-47) held on user 0's pad until g_pad_until_ns, in what the game's
// XInputGetState returns. A game is driven so where no key reaches it: another window holds the
// focus (a firewall prompt), RenderDoc's capture, or the Steam Deck.
std::atomic<uint64_t> g_pad_state{0};
// And, with bit 32, the right stick (rx in bits 0-15, ry in 16-31); with bit 56, the triggers
// (left in bits 40-47, right in 48-55).
std::atomic<uint64_t> g_pad_state2{0};
std::atomic<uint64_t> g_pad_until_ns{0};

// "A+START", "UP", "none": XINPUT_GAMEPAD button bits; false for an unknown name.
bool ParsePadButtons(const std::string& s, uint16_t* out) {
  static const std::pair<const char*, uint16_t> kNames[] = {
      {"UP", 0x0001}, {"DOWN", 0x0002}, {"LEFT", 0x0004}, {"RIGHT", 0x0008}, {"START", 0x0010},
      {"BACK", 0x0020}, {"LS", 0x0040}, {"RS", 0x0080}, {"LB", 0x0100}, {"RB", 0x0200},
      {"A", 0x1000}, {"B", 0x2000}, {"X", 0x4000}, {"Y", 0x8000}};
  *out = 0;
  if (s == "none") return true;
  std::istringstream ss(s);
  std::string name;
  while (std::getline(ss, name, '+')) {
    for (char& c : name) c = char(std::toupper(static_cast<unsigned char>(c)));
    const auto it = std::find_if(std::begin(kNames), std::end(kNames),
                                 [&](const auto& n) { return name == n.first; });
    if (it == std::end(kNames)) return false;
    *out |= it->second;
  }
  return true;
}
// The device the game presents with (the first Swap's), the one the renderer follows; set only
// with game_layer.h kFollowSwapDeviceOnly (OnHook).
std::atomic<uint32_t> g_main_device{0};

// Whether a call on `device` may lose its PM4 (the debug skip classes, the replaced
// DrawIndexedVertices): only on the device the game presents with. A game that records command
// buffers draws on a second device too, and what that one writes is replayed later by
// RunCommandBuffer: a skipped packet there would be missing from the recorded buffer. Before the
// first Swap no call qualifies. A game that follows every device has no presenting device of its
// own (g_main_device stays 0): every call qualifies, as before.
inline bool MayDropPackets(uint32_t device) {
  if constexpr (!rexnative::game::kFollowSwapDeviceOnly) return true;
  const uint32_t main_device = g_main_device.load(std::memory_order_relaxed);
  return main_device && device == main_device;
}
// Pass-through hooks (IsPassThrough) take OnHook's path only while the tap decodes the stream or
// the profiler times the guest D3D; otherwise they jump straight to the original.
std::atomic<bool> g_hook_all{false};
// rexnative.inline_draws, read once (with g_hook_all: the pass-through hooks must run OnHook).
std::atomic<bool> g_inline_draws{false};
bool InlineDraws() {
  const bool on = rexnative::game::kStreamOnly || REXCVAR_GET(rexnative_inline_draws);
  g_inline_draws.store(on, std::memory_order_relaxed);
  return on;
}
std::atomic<uint64_t> g_tap_counts[3];  // draws, aux, resolves (cumulative, for stream stats)

struct CallFrame {
  FnId id;
  uint64_t seq;
};
thread_local CallFrame t_stack[64];
thread_local uint32_t t_depth = 0;

uint32_t ParseSkip(const std::string& s) {
  uint32_t m = 0;
  if (s.find("draw") != std::string::npos) m |= kSkipDraw;
  if (s.find("clear") != std::string::npos) m |= kSkipClear;
  if (s.find("resolve") != std::string::npos) m |= kSkipResolve;
  return m;
}

void AppendV(std::string& s, const char* f, va_list args) {
  char buf[4096];
  int n = std::vsnprintf(buf, sizeof(buf), f, args);
  if (n > 0) s.append(buf, std::min<size_t>(size_t(n), sizeof(buf) - 1));
}

__attribute__((format(printf, 2, 3))) void Append(std::string& s, const char* f, ...) {
  va_list args;
  va_start(args, f);
  AppendV(s, f, args);
  va_end(args);
}

FnId CurrentFn() { return t_depth ? t_stack[std::min<uint32_t>(t_depth, 64) - 1].id : kFnCount; }
uint64_t CurrentSeq() { return t_depth ? t_stack[std::min<uint32_t>(t_depth, 64) - 1].seq : 0; }

// `how`: 1, or GuestStateTracker::kTapWrittenPredicated for a predicated (per-tile) packet.
void WriteReg(Tap& t, uint32_t reg, uint32_t v, uint8_t how = 1) {
  if (reg >= kRegCount) return;
  t.regs[reg] = v;
  t.written[reg] = how;
}

const ShaderInfo* Shader(Tap& t, uint32_t type, uint32_t addr, uint32_t dwords) {
  if (!addr || !dwords) return nullptr;
  uint64_t key = (uint64_t(type) << 63) | ((uint64_t(addr) << 20) ^ dwords);
  auto it = t.shaders.find(key);
  if (it != t.shaders.end()) return &it->second;
  ShaderInfo info;
  info.type = type;
  info.addr = addr;
  info.dwords = dwords;
  auto* mem = rexnative::host::Memory();
  const uint8_t* p = mem->TranslatePhysical<const uint8_t*>(addr);
  std::vector<uint32_t> le(dwords);
  for (uint32_t i = 0; i < dwords; ++i) {
    uint32_t v;
    std::memcpy(&v, p + i * 4, 4);
    le[i] = __builtin_bswap32(v);
  }
  info.hash = Fnv(le.data(), le.size());
  ScanUcode(le.data(), dwords, info);
  return &t.shaders.emplace(key, info).first->second;
}

// The GPU view d3d_state keeps (dirty masks + bound objects + draw arguments) against what the
// tap decoded: every register the tap has seen written, and the shaders it saw IM_LOADed.
void CompareState(Tap& t, FnId id) {
  const auto& gs = rexnative::GuestState();
  if (gs.device() != t.device) {
    ++t.state_unchecked;
    if (t.tracing && t.state_details < 64) {
      ++t.state_details;
      Append(t.out, "# state_unchecked d=%llu fn=%s tracker_device=%08X tap_device=%08X\n",
             (unsigned long long)t.draw_seq, kFns[id].name, gs.device(), t.device);
    }
    return;
  }
  ++t.state_draws;
  ++t.frame.checked;
  static const auto range = [] {
    const std::string r = REXCVAR_GET(rexnative_d3d_tap_check_range);
    unsigned a = 0, b = 0;
    return std::sscanf(r.c_str(), "%x-%x", &a, &b) == 2 ? std::pair<uint32_t, uint32_t>{a, b}
                                                       : std::pair<uint32_t, uint32_t>{0, 0};
  }();
  if (range.second) {
    const uint32_t* v = gs.regs();
    std::string s;
    uint32_t n = 0, first = 0;
    for (uint32_t r = range.first; r <= range.second && r < kRegCount; ++r) {
      if (t.written[r] != 1 || t.regs[r] == v[r]) continue;
      if (!n++) first = r;
      if (n <= 8) Append(s, " %04X tap=%08X state=%08X", r, t.regs[r], v[r]);
    }
    if (n) {
      REXNATIVE_LOG_FIRST(400, WARN, "ReXNative D3D check: frame {} draw {} fn={} vs={:08X}: {} "
                          "registers from {:04X} differ from the stream:{}",
                          t.frame_index, t.draw_seq, kFns[id].name, gs.vs().ucode, n, first, s);
      if (t.tracing)
        Append(t.out, "# range_diff d=%llu fn=%s n=%u first=%04X%s\n",
               (unsigned long long)t.draw_seq, kFns[id].name, n, first, s.c_str());
      // The first difference traces the next two frames (rexnative-d3d-rangediff-f*.txt).
      static bool traced = false;
      if (!traced && !t.tracing && !t.trace_left) {
        traced = true;
        t.trace_tag = "rangediff";
        t.trace_left = 2;
      }
    }
    return;
  }
  uint32_t bad = gs.CompareWithTap(t.regs, t.written, t.state_regs.data(), &t.state_per_tile);
  const auto& gvs = gs.vs();
  bool vs_bad = t.vs_addr && (gvs.ucode != t.vs_addr || gvs.dwords != t.vs_dwords || gvs.immediate);
  if (!t.vs_addr && t.vs_dwords) {  // IM_LOAD_IMMEDIATE: the tracker's copy must hash the same
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; gvs.immediate && i < gvs.dwords; ++i) {
      uint32_t d = __builtin_bswap32(gvs.immediate[i]);
      h = Fnv(&d, 1, h);
    }
    vs_bad = !gvs.immediate || gvs.dwords != t.vs_dwords || h != t.vs_imm_hash;
  }
  const bool ps_bad = t.ps_addr && (gs.ps().ucode != t.ps_addr || gs.ps().dwords != t.ps_dwords);
  if (vs_bad || ps_bad) ++t.state_shader_bad;
  if (!bad && !vs_bad && !ps_bad) return;
  ++t.state_bad_draws;
  ++t.frame.differ;
  // Traced frames list 64 differing draws; --rexnative_d3d_tap_check keeps each frame's first.
  const bool detail = t.tracing && t.state_details < 64;
  if (!detail && (t.tracing || !t.check_first.empty())) return;
  if (detail) ++t.state_details;
  std::string s;
  Append(s, "d=%llu fn=%s", (unsigned long long)t.draw_seq, kFns[id].name);
  const uint32_t* v = gs.regs();
  uint32_t shown = 0;
  for (uint32_t r = 0x2000; r < kRegCount && shown < 16; ++r) {
    if (t.written[r] != 1 || t.regs[r] == v[r] || rexnative::IsTapOnlyRegister(r)) continue;
    Append(s, " %04X tap=%08X state=%08X", r, t.regs[r], v[r]);
    ++shown;
  }
  if (vs_bad)
    Append(s, " vs tap=%08X:%u%s state=%08X:%u%s", t.vs_addr, t.vs_dwords, t.vs_addr ? "" : ":imm",
           gvs.ucode, gvs.dwords, gvs.immediate ? ":imm" : "");
  if (ps_bad)
    Append(s, " ps tap=%08X:%u state=%08X:%u", t.ps_addr, t.ps_dwords, gs.ps().ucode,
           gs.ps().dwords);
  if (detail)
    Append(t.out, "# state_diff %s\n", s.c_str());
  else
    t.check_first = std::move(s);
}

// rexnative.resolve_check_registers: the registers at the copy draw of the hooked Resolve that
// runs, for CheckRegisterResolve after it.
thread_local bool t_reg_resolve_seen = false;
thread_local rexnative::ResolveRegisters t_reg_resolve;
void CheckRegisterResolve(const uint8_t* base, const rexnative::HookArgs& a);

void OnDraw(Tap& t, uint32_t op, bool predicated) {
  // DRAW_INDX: viz, initiator, [index base, index size]; DRAW_INDX_2: initiator, indices.
  uint32_t initiator =
      op == 0x22 ? (t.pkt.size() > 1 ? t.pkt[1] : 0) : (t.pkt.empty() ? 0 : t.pkt[0]);
  bool resolve = (t.regs[0x2208] & 7) == uint32_t(xenos::EDRAMMode::kCopy);
  if (resolve) {
    if (REXCVAR_GET(rexnative_resolve_check_registers)) {
      t_reg_resolve = rexnative::ResolveRegisters::From([&](uint32_t r) { return t.regs[r]; });
      t_reg_resolve_seen = true;
    }
    ++t.frame.tap_resolves;
    g_tap_counts[2].fetch_add(1, std::memory_order_relaxed);
  }
  if (predicated) {
    ++t.frame.tap_draws;
    g_tap_counts[0].fetch_add(1, std::memory_order_relaxed);
  } else {
    ++t.frame.tap_aux;
    g_tap_counts[1].fetch_add(1, std::memory_order_relaxed);
  }
  if (!t.tracing) return;

  uint32_t prim = initiator & 0x3F, src = (initiator >> 6) & 3, count = initiator >> 16;
  FnId fn = CurrentFn();
  Append(t.out, "D %llu c=%llu fn=%s op=%s pred=%d %s prim=%u src=%s idx32=%u count=%u",
         (unsigned long long)t.draw_seq++, (unsigned long long)CurrentSeq(),
         fn < kFnCount ? kFns[fn].name : "-", OpName(op), predicated ? 1 : 0,
         resolve ? "RESOLVE" : "draw", prim,
         src == 0   ? "dma"
         : src == 2 ? "auto"
                    : "imm",
         (initiator >> 11) & 1, count);
  if (op == 0x22 && src == 0 && t.pkt.size() >= 4)
    Append(t.out, " ib=%08X:%u", t.pkt[2], t.pkt[3] & 0xFFFFFF);
  if (resolve) {
    Append(t.out, " copy_ctl=%08X dest=%08X pitch=%08X info=%08X surf=%08X c0=%08X d=%08X\n",
           t.regs[0x2318], t.regs[0x2319], t.regs[0x231A], t.regs[0x231B], t.regs[0x2000],
           t.regs[0x2001], t.regs[0x2002]);
    return;
  }
  const ShaderInfo* vs = Shader(t, 0, t.vs_addr, t.vs_dwords);
  const ShaderInfo* ps = Shader(t, 1, t.ps_addr, t.ps_dwords);
  Append(t.out, " vs=%08X:%u:%016llX ps=%08X:%u:%016llX", t.vs_addr, t.vs_dwords,
         (unsigned long long)(vs ? vs->hash : t.vs_imm_hash), t.ps_addr, t.ps_dwords,
         (unsigned long long)(ps ? ps->hash : t.ps_imm_hash));
  Append(t.out,
         " mode=%08X surf=%08X c0=%08X c1=%08X c2=%08X c3=%08X d=%08X mask=%08X depth=%08X"
         " blend0=%08X sc=%08X,%08X vgt_offs=%u",
         t.regs[0x2208], t.regs[0x2000], t.regs[0x2001], t.regs[0x2003], t.regs[0x2004],
         t.regs[0x2005], t.regs[0x2002], t.regs[0x2104], t.regs[0x2200], t.regs[0x2201],
         t.regs[0x2081], t.regs[0x2082], t.regs[0x2102]);
  uint32_t tmask = (vs ? vs->tfetch_mask : 0) | (ps ? ps->tfetch_mask : 0);
  for (uint32_t s = 0; s < 32; ++s) {
    if (!(tmask >> s & 1)) continue;
    const uint32_t* f = &t.regs[0x4800 + s * 6];
    uint32_t w = (f[2] & 0x1FFF) + 1, h = ((f[2] >> 13) & 0x1FFF) + 1;
    Append(t.out, " t%u=%08X/fmt%u/%ux%u/%s/dim%u", s, f[1] & 0xFFFFF000u, f[1] & 0x3F, w, h,
           (f[0] >> 31) ? "tiled" : "linear", (f[5] >> 9) & 3);
    ++t.textures[{f[1], f[2]}];
  }
  if (vs) {
    for (uint32_t v = 0; v < 96; ++v) {
      bool used = v < 64 ? (vs->vfetch_lo >> v & 1) : (vs->vfetch_hi >> (v - 64) & 1);
      if (!used) continue;
      const uint32_t* f = &t.regs[0x4800 + v * 2];
      Append(t.out, " v%u=%08X:%u", v, f[0] & 0xFFFFFFFCu, (f[1] >> 2) & 0xFFFFFF);
    }
  }
  Append(t.out, "\n");
}

void HandlePacket(Tap& t) {
  uint32_t type = t.header >> 30;
  if (type == 0) {
    uint32_t reg = t.header & 0x7FFF;
    bool one = (t.header >> 15) & 1;
    for (size_t i = 0; i < t.pkt.size(); ++i) WriteReg(t, one ? reg : reg + uint32_t(i), t.pkt[i]);
    if (t.tracing) {
      Append(t.out, "W %04X", reg);
      for (size_t i = 0; i < t.pkt.size() && i < 64; ++i) Append(t.out, " %08X", t.pkt[i]);
      Append(t.out, "%s\n", t.pkt.size() > 64 ? " ..." : "");
    }
    return;
  }
  if (type == 1) {
    WriteReg(t, t.header & 0x7FF, t.pkt[0]);
    WriteReg(t, (t.header >> 11) & 0x7FF, t.pkt[1]);
    return;
  }
  uint32_t op = (t.header >> 8) & 0x7F;
  bool pred = t.header & 1;
  if (t.tracing) ++t.op_counts[op];
  if (!OpName(op)) ++t.frame.unknown_ops;
  auto range = [&](uint32_t reg, size_t first) {
    const uint8_t how = pred ? rexnative::GuestStateTracker::kTapWrittenPredicated : 1;
    for (size_t i = first; i < t.pkt.size(); ++i)
      WriteReg(t, reg + uint32_t(i - first), t.pkt[i], how);
    if (t.tracing) {
      size_t n = t.pkt.size() - first;
      if (reg >= 0x4000 && reg < 0x4800)
        Append(t.out, "K %04X n=%zu h=%016llX\n", reg, n,
               (unsigned long long)Fnv(t.pkt.data() + first, n));
      else {
        Append(t.out, "W %04X", reg);
        for (size_t i = first; i < t.pkt.size() && i < first + 64; ++i)
          Append(t.out, " %08X", t.pkt[i]);
        Append(t.out, "\n");
      }
    }
  };
  switch (op) {
    case 0x2D: {  // SET_CONSTANT
      if (t.pkt.empty()) break;
      uint32_t index = t.pkt[0] & 0x7FF, kind = (t.pkt[0] >> 16) & 0xFF;
      if (kind < std::size(kConstantBase)) range(kConstantBase[kind] + index, 1);
      break;
    }
    case 0x55:  // SET_CONSTANT2
    case 0x56:  // SET_SHADER_CONSTANTS
      if (!t.pkt.empty()) range(t.pkt[0] & 0xFFFF, 1);
      break;
    case 0x2F: {  // LOAD_ALU_CONSTANT
      if (t.pkt.size() < 3) break;
      uint32_t addr = t.pkt[0] & 0x3FFFFFFF, index = t.pkt[1] & 0x7FF,
               kind = (t.pkt[1] >> 16) & 0xFF;
      uint32_t n = t.pkt[2] & 0xFFF;
      if (kind >= std::size(kConstantBase)) break;
      const uint8_t* p = rexnative::host::Memory()->TranslatePhysical<const uint8_t*>(addr);
      for (uint32_t i = 0; i < n; ++i) {
        uint32_t v;
        std::memcpy(&v, p + i * 4, 4);
        WriteReg(t, kConstantBase[kind] + index + i, __builtin_bswap32(v));
      }
      if (t.tracing) Append(t.out, "L %04X n=%u from=%08X\n", kConstantBase[kind] + index, n, addr);
      break;
    }
    case 0x27:  // IM_LOAD
      if (t.pkt.size() < 2) break;
      if ((t.pkt[0] & 3) == 0) {
        t.vs_addr = t.pkt[0] & ~3u;
        t.vs_dwords = t.pkt[1] & 0xFFFF;
      } else {
        t.ps_addr = t.pkt[0] & ~3u;
        t.ps_dwords = t.pkt[1] & 0xFFFF;
      }
      if (t.tracing)
        Append(t.out, "S %s %08X %u\n", (t.pkt[0] & 3) ? "ps" : "vs", t.pkt[0] & ~3u,
               t.pkt[1] & 0xFFFF);
      break;
    case 0x2B: {  // IM_LOAD_IMMEDIATE
      if (t.pkt.size() < 2) break;
      uint64_t h = Fnv(t.pkt.data() + 2, t.pkt.size() - 2);
      if ((t.pkt[0] & 3) == 0) {
        t.vs_addr = 0;
        t.vs_dwords = t.pkt[1] & 0xFFFF;
        t.vs_imm_hash = h;
      } else {
        t.ps_addr = 0;
        t.ps_dwords = t.pkt[1] & 0xFFFF;
        t.ps_imm_hash = h;
      }
      if (t.tracing)
        Append(t.out, "S %s imm %u %016llX\n", (t.pkt[0] & 3) ? "ps" : "vs", t.pkt[1] & 0xFFFF,
               (unsigned long long)h);
      break;
    }
    case 0x22:
    case 0x36:
    case 0x34:
    case 0x35:
      OnDraw(t, op, pred);
      break;
    case 0x3F:
    case 0x37:
      ++t.frame.ibs;
      if (t.tracing && t.pkt.size() >= 2)
        Append(t.out, "I %08X %u\n", t.pkt[0], t.pkt[1] & 0xFFFFF);
      break;
    case 0x10:  // NOP (also carries inline vertex data for the UP draws)
      break;
    default:
      if (t.tracing) {
        Append(t.out, "P %s%s n=%zu", OpName(op) ? OpName(op) : "op", pred ? " pred" : "",
               t.pkt.size());
        if (!OpName(op)) Append(t.out, " opcode=%02X", op);
        for (size_t i = 0; i < t.pkt.size() && i < 8; ++i) Append(t.out, " %08X", t.pkt[i]);
        Append(t.out, "\n");
      }
      break;
  }
}

void Feed(Tap& t, uint32_t dw) {
  if (!t.in_packet) {
    // MakeSpace leaves dwords it patches later as zeros; the D3D never writes register 0, so
    // a zero header is padding, not a one-register type-0 packet.
    if (dw == 0) {
      ++t.frame.zero_pad;
      return;
    }
    t.header = dw;
    t.pkt.clear();
    switch (dw >> 30) {
      case 0:
      case 3:
        t.need = ((dw >> 16) & 0x3FFF) + 1;
        break;
      case 1:
        t.need = 2;
        break;
      default:
        return;  // type 2: NOP
    }
    t.in_packet = true;
    return;
  }
  t.pkt.push_back(dw);
  if (--t.need == 0) {
    t.in_packet = false;
    HandlePacket(t);
  }
}

// Decode everything the D3D wrote since the last sync.
// `jump`: the call that just returned moved the push pointer to a new segment by assignment,
// so the dwords up to the new pointer (reserved patch space, the unwritten segment start) were
// not written in this lap and are skipped.
void Sync(Tap& t, uint8_t* base, bool segment_switch_expected, bool jump = false) {
  uint32_t cur = Load32(base, t.device + dev::kPush);
  if (cur == t.cursor) return;
  if (cur > t.cursor && cur - t.cursor <= (16u << 20) && !jump) {
    // Each window starts at a packet boundary: the D3D completes its packets before it calls
    // another function or returns. A packet still open here was cut short (or the decoder
    // misread a length); drop it rather than let a bad length swallow the following stream.
    if (t.in_packet) {
      ++t.frame.torn;
      if (t.tracing)
        Append(t.out, "# torn packet header=%08X still needs %u dwords at window %08X\n", t.header,
               t.need, t.cursor + 4);
      t.in_packet = false;
    }
    if (t.tracing) {  // raw dwords, for checking the decoder against the stream
      uint32_t n = (cur - t.cursor) / 4;
      Append(t.out, "X %08X n=%u", t.cursor + 4, n);
      for (uint32_t i = 0; i < n && i < 24; ++i)
        Append(t.out, " %08X", Load32(base, t.cursor + 4 + i * 4));
      Append(t.out, "%s\n", n > 24 ? " ..." : "");
    }
    for (uint32_t a = t.cursor + 4; a <= cur; a += 4) Feed(t, Load32(base, a));
    t.frame.dwords += (cur - t.cursor) / 4;
  } else {
    // The pointer moved to another segment (MakeSpace, RestorePush). Packets end at segment
    // boundaries, so a partial packet here means the decoder lost data.
    if (segment_switch_expected)
      ++t.frame.segments;
    else
      ++t.frame.resyncs;
    if (t.in_packet) ++t.frame.resyncs;
    t.in_packet = false;
    if (t.tracing)
      Append(t.out, "# segment %08X -> %08X%s\n", t.cursor, cur,
             segment_switch_expected ? "" : " (unexpected)");
  }
  t.cursor = cur;
}

void FlushTrace(Tap& t) {
  Append(t.out,
         "# summary frame=%llu calls=%llu draw_calls=%llu tap_draws=%llu tap_aux=%llu"
         " tap_resolves=%llu dwords=%llu segments=%llu resyncs=%llu torn=%llu unknown_ops=%llu"
         " ibs=%llu zero_pad=%llu state_checked_draws=%llu state_mismatch_draws=%llu"
         " state_shader_mismatch=%llu state_unchecked=%llu state_per_tile=%llu\n",
         (unsigned long long)t.trace_frame, (unsigned long long)t.frame.calls,
         (unsigned long long)t.frame.draw_calls, (unsigned long long)t.frame.tap_draws,
         (unsigned long long)t.frame.tap_aux, (unsigned long long)t.frame.tap_resolves,
         (unsigned long long)t.frame.dwords, (unsigned long long)t.frame.segments,
         (unsigned long long)t.frame.resyncs, (unsigned long long)t.frame.torn,
         (unsigned long long)t.frame.unknown_ops, (unsigned long long)t.frame.ibs,
         (unsigned long long)t.frame.zero_pad, (unsigned long long)t.state_draws,
         (unsigned long long)t.state_bad_draws, (unsigned long long)t.state_shader_bad,
         (unsigned long long)t.state_unchecked, (unsigned long long)t.state_per_tile);
  for (uint32_t r = 0; r < kRegCount; ++r)
    if (t.state_regs[r])
      Append(t.out, "# state_mismatch %04X %s draws=%llu\n", r, RegName(r) ? RegName(r) : "?",
             (unsigned long long)t.state_regs[r]);
  for (uint32_t i = 0; i < kFnCount; ++i)
    if (t.fn_calls[i])
      Append(t.out, "# calls %08X %s %llu\n", kFns[i].addr, kFns[i].name,
             (unsigned long long)t.fn_calls[i]);
  for (auto& [op, n] : t.op_counts)
    Append(t.out, "# packets %s %llu\n", OpName(op) ? OpName(op) : "?", (unsigned long long)n);
  for (auto& [k, s] : t.shaders)
    Append(t.out, "# shader %s %08X dwords=%u hash=%016llX tfetch=%08X vfetch=%08X%016llX\n",
           s.type ? "ps" : "vs", s.addr, s.dwords, (unsigned long long)s.hash, s.tfetch_mask,
           s.vfetch_hi, (unsigned long long)s.vfetch_lo);
  for (auto& [k, n] : t.textures)
    Append(t.out, "# texture %08X fmt%u %ux%u draws=%llu\n", k.first & 0xFFFFF000u, k.first & 0x3F,
           (k.second & 0x1FFF) + 1, ((k.second >> 13) & 0x1FFF) + 1, (unsigned long long)n);
  std::filesystem::path dir = REXCVAR_GET(rexnative_d3d_trace_dir);
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  char name[256];
  std::snprintf(name, sizeof(name), "rexnative-d3d-%s-f%llu.txt", t.trace_tag.c_str(),
                (unsigned long long)t.trace_frame);
  std::ofstream(dir / name, std::ios::binary).write(t.out.data(), std::streamsize(t.out.size()));
  REXLOG_INFO("ReXNative D3D trace: wrote {} ({} bytes, {} draws, {} resolves)", (dir / name).string(),
              t.out.size(), t.frame.tap_draws, t.frame.tap_resolves);
  t.out.clear();
  t.out.shrink_to_fit();
}

void BeginTrace(Tap& t) {
  t.trace_frame = t.frame_index;
  t.call_seq = t.draw_seq = 0;
  t.state_draws = t.state_bad_draws = t.state_shader_bad = t.state_unchecked = t.state_details = 0;
  t.state_per_tile = 0;
  t.state_pending = false;
  std::fill(t.state_regs.begin(), t.state_regs.end(), 0);
  t.op_counts.clear();
  t.shaders.clear();
  t.textures.clear();
  std::memset(t.fn_calls, 0, sizeof(t.fn_calls));
  t.out.reserve(32u << 20);
  Append(t.out, "# rexnative-d3d-trace v1 tag=%s frame=%llu device=%08X push=%08X\n",
         t.trace_tag.c_str(), (unsigned long long)t.frame_index, t.device, t.cursor);
  Append(t.out, "# state at frame start (registers the tap has seen written)\n");
  for (uint32_t r = 0; r < kRegCount; ++r) {
    if (!t.written[r]) continue;
    if (r >= 0x4000 && r < 0x4800) continue;  // float constants: hashed per block below
    const char* n = RegName(r);
    Append(t.out, "R %04X %08X %s\n", r, t.regs[r], n ? n : "");
  }
  for (uint32_t b = 0x4000; b < 0x4800; b += 256)
    Append(t.out, "K %04X n=256 h=%016llX\n", b, (unsigned long long)Fnv(&t.regs[b], 256));
  Append(t.out, "S vs %08X %u\nS ps %08X %u\n", t.vs_addr, t.vs_dwords, t.ps_addr, t.ps_dwords);
}

void PollControl(Tap& t, uint8_t* base) {
  const std::string& path = REXCVAR_GET(rexnative_d3d_control);
  if (path.empty()) return;
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) return;
  std::ifstream in(path);
  std::string line;
  std::vector<std::string> lines;
  while (std::getline(in, line)) lines.push_back(line);
  in.close();
  std::filesystem::remove(path, ec);
  for (auto& l : lines) {
    std::istringstream ss(l);
    std::string cmd, arg;
    ss >> cmd >> arg;
    if (cmd == "trace") {
      if (!t.enabled) {
        REXNATIVE_WARN_ONCE("ReXNative D3D control: '{}' ignored: the tap is off", l);
        continue;
      }
      uint32_t frames = 1;
      ss >> frames;
      t.trace_tag = arg.empty() ? "frame" : arg;
      t.trace_left = std::max<uint32_t>(frames, 1);
    } else if (cmd == "skip") {
      g_skip.store(arg == "none" ? 0 : ParseSkip(arg));
    } else if (cmd == "tap") {
      bool on = arg != "off";
      if (on && !t.enabled && t.device) {  // resume at the current push position
        t.cursor = Load32(base, t.device + dev::kPush);
        t.in_packet = false;
      }
      t.enabled = on;
      if (!on) t.trace_left = 0;
      g_hook_all.store(on || REXCVAR_GET(rexnative_profile) || g_inline_draws.load(std::memory_order_relaxed),
                       std::memory_order_relaxed);
    } else if (cmd == "cmdbuf") {
      if (kFnRunCommandBuffer == kFnCount) {
        REXNATIVE_WARN_ONCE("ReXNative D3D control: '{}' ignored: no RunCommandBuffer hook", l);
        continue;
      }
      std::string arg2;
      ss >> arg2;
      rexnative::cmdbuf::Control(arg, arg2, REXCVAR_GET(rexnative_d3d_trace_dir));
    } else if (cmd == "pad") {
      // pad <buttons|none> [lx=N] [ly=N] [rx=N] [ry=N] [lt=N] [rt=N] [ms]: held on user 0's pad
      // for ms (default 150); sticks -32768 to 32767, triggers 0 to 255.
      uint16_t buttons = 0;
      if (!ParsePadButtons(arg, &buttons)) {
        REXNATIVE_LOG_FIRST(4, WARN, "ReXNative D3D control: '{}': unknown button", l);
        continue;
      }
      uint64_t state = buttons, state2 = 0, ms = 150;
      for (std::string tok; ss >> tok;) {
        if (tok.rfind("lx=", 0) == 0 || tok.rfind("ly=", 0) == 0) {
          const int v = std::clamp(std::atoi(tok.c_str() + 3), -32768, 32767);
          state |= uint64_t(uint16_t(int16_t(v))) << (tok[1] == 'x' ? 16 : 32) | 1ull << 48;
        } else if (tok.rfind("rx=", 0) == 0 || tok.rfind("ry=", 0) == 0) {
          const int v = std::clamp(std::atoi(tok.c_str() + 3), -32768, 32767);
          state2 |= uint64_t(uint16_t(int16_t(v))) << (tok[1] == 'x' ? 0 : 16) | 1ull << 32;
        } else if (tok.rfind("lt=", 0) == 0 || tok.rfind("rt=", 0) == 0) {
          const int v = std::clamp(std::atoi(tok.c_str() + 3), 0, 255);
          state2 |= uint64_t(v) << (tok[0] == 'l' ? 40 : 48) | 1ull << 56;
        } else {
          ms = std::strtoull(tok.c_str(), nullptr, 10);
        }
      }
      g_pad_state.store(state, std::memory_order_relaxed);
      g_pad_state2.store(state2, std::memory_order_relaxed);
      g_pad_until_ns.store(rexnative::NowNs() + ms * 1000000ull, std::memory_order_release);
    } else if (cmd == "watch") {
      if (arg == "off") {
        rexnative::watch::Clear();
      } else {
        std::string mode = "nan";
        uint32_t count = 16;
        if (!(ss >> mode)) mode = "nan";
        if (!(ss >> count)) count = 16;
        rexnative::watch::Set(uint32_t(std::strtoul(arg.c_str(), nullptr, 16)), mode, count);
      }
    } else if (cmd == "peek") {
      // peek <guest physical address hex> [bytes, default 256]: how many of them are not zero,
      // and the first 32, as guest memory holds them now (what the CPU wrote there).
      const uint32_t addr = uint32_t(std::strtoul(arg.c_str(), nullptr, 16)) & 0x1FFFFFFFu;
      uint32_t bytes = 256;
      ss >> bytes;
      bytes = std::clamp<uint32_t>(bytes, 1, 0x1000000);
      auto* mem = rexnative::host::Memory();
      const uint8_t* p = mem && addr + uint64_t(bytes) <= 0x20000000u
                             ? mem->TranslatePhysical<const uint8_t*>(addr)
                             : nullptr;
      if (!p) {
        REXLOG_WARN("ReXNative peek: {:08X}+{:X} is not guest physical memory", addr, bytes);
        continue;
      }
      uint32_t nonzero = 0;
      for (uint32_t i = 0; i < bytes; ++i) nonzero += p[i] != 0;
      std::string head;
      for (uint32_t i = 0; i < std::min<uint32_t>(bytes, 32); ++i) head += std::format("{:02X}", p[i]);
      REXLOG_INFO("ReXNative peek: {:08X}+{:X}: {} bytes not zero; {}", addr, bytes, nonzero, head);
      // peek <address> <bytes> <file>: all of them into the file too (a CPU-written texture).
      std::string file;
      if (ss >> file) {
        if (std::FILE* out = std::fopen(file.c_str(), "wb")) {
          std::fwrite(p, 1, bytes, out);
          std::fclose(out);
        }
      }
    } else if (cmd == "poke") {
      // poke <guest physical address hex> <dword hex>...: writes the dwords there as the guest's
      // CPU would (big-endian), to test what the game makes of a value (a read-back resolve's).
      const uint32_t addr = uint32_t(std::strtoul(arg.c_str(), nullptr, 16)) & 0x1FFFFFFCu;
      auto* mem = rexnative::host::Memory();
      std::string word;
      uint32_t written = 0;
      while (mem && ss >> word && addr + 4 * (written + 1) <= 0x20000000u) {
        const uint32_t v = uint32_t(std::strtoul(word.c_str(), nullptr, 16));
        uint8_t* p = mem->TranslatePhysical<uint8_t*>(addr + 4 * written);
        const uint8_t be[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
        std::memcpy(p, be, 4);
        ++written;
      }
      REXLOG_INFO("ReXNative poke: {:08X}: {} dwords written", addr, written);
    } else if (cmd == "set") {
      std::string value;
      std::getline(ss >> std::ws, value);
      if (arg.empty() || value.empty()) {
        REXNATIVE_LOG_FIRST(4, WARN, "ReXNative D3D control: '{}' ignored (set <setting> <value>)", l);
        continue;
      }
      rex::cvar::SetFlagByName(arg, value);
    } else if (cmd == "renderer") {
      rexnative::NativeRenderer* r = rexnative::NativeRenderer::Get();
      if (!r || (arg != "native" && arg != "stock")) {
        REXNATIVE_LOG_FIRST(4, WARN, "ReXNative D3D control: '{}' ignored (renderer native|stock)", l);
        continue;
      }
      r->RequestRenderer(arg == "native");
    }
    REXLOG_INFO("ReXNative D3D control: {} (skip mask {})", l, g_skip.load());
  }
}

struct Window {
  uint64_t frames = 0, calls = 0, draw_calls = 0, tap_draws = 0, tap_aux = 0, tap_resolves = 0;
  uint64_t dwords = 0, segments = 0, resyncs = 0, torn = 0, unknown_ops = 0, min_draws = ~0ull,
           max_draws = 0;
} g_window;

std::FILE* FrameCsv() {
  static std::FILE* f = [] {
    std::string p = REXCVAR_GET(rexnative_d3d_frame_csv);
    std::FILE* f = p.empty() ? nullptr : std::fopen(p.c_str(), "w");
    if (f)
      std::fprintf(f,
                   "frame,calls,draw_calls,tap_draws,tap_aux,tap_resolves,dwords,segments,resyncs,"
                   "torn,unknown_ops\n");
    return f;
  }();
  return f;
}

void EndFrame(Tap& t, uint8_t* base) {
  auto& c = t.frame;
  if (std::FILE* f = FrameCsv()) {
    std::fprintf(f, "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                 (unsigned long long)t.frame_index, (unsigned long long)c.calls,
                 (unsigned long long)c.draw_calls, (unsigned long long)c.tap_draws,
                 (unsigned long long)c.tap_aux, (unsigned long long)c.tap_resolves,
                 (unsigned long long)c.dwords, (unsigned long long)c.segments,
                 (unsigned long long)c.resyncs, (unsigned long long)c.torn,
                 (unsigned long long)c.unknown_ops);
    if ((t.frame_index & 31) == 0) std::fflush(f);
  }
  auto& w = g_window;
  ++w.frames;
  w.calls += c.calls;
  w.draw_calls += c.draw_calls;
  w.tap_draws += c.tap_draws;
  w.tap_aux += c.tap_aux;
  w.tap_resolves += c.tap_resolves;
  w.dwords += c.dwords;
  w.segments += c.segments;
  w.resyncs += c.resyncs;
  w.torn += c.torn;
  w.unknown_ops += c.unknown_ops;
  w.min_draws = std::min(w.min_draws, c.tap_draws);
  w.max_draws = std::max(w.max_draws, c.tap_draws);
  int32_t every = REXCVAR_GET(rexnative_d3d_stats_frames);
  if (every > 0 && w.frames >= uint64_t(every)) {
    double n = double(w.frames);
    REXLOG_INFO(
        "ReXNative D3D: frames {} | per frame: calls {:.0f}, draw calls {:.1f}, tap draws {:.1f} "
        "(min {} max {}), tap aux {:.1f}, tap resolves {:.1f}, dwords {:.0f}, segments {:.1f}, "
        "resyncs {}, torn {}, unknown ops {} | tap {} skip {}",
        t.frame_index, w.calls / n, w.draw_calls / n, w.tap_draws / n, w.min_draws, w.max_draws,
        w.tap_aux / n, w.tap_resolves / n, w.dwords / n, w.segments / n, w.resyncs, w.torn,
        w.unknown_ops, t.enabled ? "on" : "off", g_skip.load());
    w = Window{};
  }
  if (t.check && c.differ) {
    const rexnative::NativeRenderer* r = rexnative::NativeRenderer::Get();
    REXNATIVE_LOG_FIRST(
        2000, INFO,
        "ReXNative D3D check: frame {} (native {}): {} of {} draws differ from the stream; first {}",
        t.frame_index, r ? r->frame_index() : 0, c.differ, c.checked, t.check_first);
  }
  t.check_first.clear();
  if (t.tracing) {
    FlushTrace(t);
    t.tracing = false;
  }
  t.frame = FrameCounters{};
  ++t.frame_index;
  PollControl(t, base);
  if (t.trace_left) {  // a traced frame runs from one Swap's return to the next
    --t.trace_left;
    t.tracing = true;
    BeginTrace(t);
  }
}

// The game creates the device more than once (once more at the title screen); the device a Swap
// or draw call runs on is the current one.
[[gnu::noinline]] void FollowDevice(Tap& t, uint8_t* base, FnId id, uint32_t device) {
  REXNATIVE_LOG_FIRST(8, INFO, "ReXNative D3D: {} runs on device {:08X} (was {:08X}); following it",
                 kFns[id].name, device, t.device);
  t.device = device;
  t.cursor = Load32(base, t.device + dev::kPush);
  t.in_packet = false;
  // No CreateDevice hook (game_layer.h kFnCreateDevice 0): the first device seen is
  // configured here.
  if (!t.configured) {
    t.configured = true;
    t.enabled = REXCVAR_GET(rexnative_d3d_tap);
    t.check = t.enabled && REXCVAR_GET(rexnative_d3d_tap_check);
    g_hook_all.store(InlineDraws() || t.enabled || REXCVAR_GET(rexnative_profile), std::memory_order_relaxed);
    g_skip.store(ParseSkip(REXCVAR_GET(rexnative_d3d_skip)));
  }
}

// The tap's side of one hooked call: counts it, follows the device it runs on and, with the
// tap on, decodes the push buffer around it. `run` runs the guest function (OnHook's wrapper).
template <FnId kId, typename Run>
inline void OnCall(PPCContext& ctx, uint8_t* base, Run run) {
  // One guest thread drives the device: a plain increment keeps the counts exact, without the
  // locked add.
  if constexpr (IsCountedFn(kId))
    g_calls[kId].store(g_calls[kId].load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
  Tap& t = g_tap;
  const uint32_t r3 = ctx.r3.u32;
  constexpr uint32_t kSkip = SkipClassOf(kId);
  const bool forward = kSkip == 0 || !(kSkip & g_skip.load(std::memory_order_relaxed)) ||
                       !MayDropPackets(r3);
  auto call = [&] {
    if (forward)
      run(ctx, base);
    else
      ctx.r3.u64 = 0;
  };
  if constexpr (kId == kFnSwap || kId == kFnDrawIndexed || kId == kFnDrawVertices) {
    // game_layer.h kFollowSwapDeviceOnly: only Swap's device is followed (Iron Brigade
    // draws on a second device while levels load); otherwise every call's.
    if (r3 && r3 != t.device &&
        (rexnative::game::kFollowSwapDeviceOnly ? (kId == kFnSwap || !t.device)
                                             : (t.device || rexnative::game::kConfigureAtFirstDevice)))
        [[unlikely]]
      FollowDevice(t, base, kId, r3);
  }
  if (!(t.device && r3 == t.device)) {
    if constexpr (kId == kFnCreateDevice) {  // CreateDevice(..., ppDevice in r8)
      const uint32_t out = ctx.r8.u32;
      run(ctx, base);
      // Not a pointer: not Direct3D_CreateDevice's ppDevice (the tool's adapt step checks that
      // it reads r8); the first device the other calls use then (FollowDevice).
      t.device = out >= 0x10000 && !(out & 3) ? Load32(base, out) : 0;
      t.cursor = t.device ? Load32(base, t.device + dev::kPush) : 0;
      t.in_packet = false;
      // Only once: a later device would undo the control commands sent since.
      if (!t.configured) {
        t.configured = true;
        t.enabled = REXCVAR_GET(rexnative_d3d_tap);
        t.check = t.enabled && REXCVAR_GET(rexnative_d3d_tap_check);
        g_hook_all.store(InlineDraws() || t.enabled || REXCVAR_GET(rexnative_profile), std::memory_order_relaxed);
        g_skip.store(ParseSkip(REXCVAR_GET(rexnative_d3d_skip)));
      }
      REXLOG_INFO("ReXNative D3D: device {:08X}, push {:08X}, tap {}{}, {} hooked functions", t.device,
                  t.cursor, t.enabled ? "on" : "off", t.check ? " (state check)" : "",
                  uint32_t(kFnCount));
      return;
    }
    call();
    return;
  }
  if (!t.enabled) {  // the hooks only forward; Swap still polls the control file
    call();
    if constexpr (kId == kFnSwap) {
      ++t.frame_index;
      PollControl(t, base);
    }
    return;
  }

  Sync(t, base, false);
  const uint64_t seq = ++t.call_seq;
  ++t.frame.calls;
  if constexpr (IsDrawCounterFn(kId)) ++t.frame.draw_calls;
  if (t.tracing) {
    ++t.fn_calls[kId];
    Append(t.out, "C %llu %u %s r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X%s\n",
           (unsigned long long)seq, t_depth, kFns[kId].name, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
           ctx.r7.u32, ctx.r8.u32, forward ? "" : " SKIPPED");
  }
  if (t_depth < 64) t_stack[t_depth] = {kId, seq};
  ++t_depth;
  call();
  // These move the push pointer by assignment: MakeSpace and RestorePush stay in the written
  // stream; the segment functions leave a hole (reserved dwords, an unwritten new segment).
  constexpr bool kJump = kId == kFnSwitchSegment || kId == kFnStartSegment ||
                         kId == kFnRestoreSegment || kId == kFnRestoreSegment13392;
  Sync(t, base, kJump || kId == kFnMakeSpace || kId == kFnRestorePush || kId == kFnDrawVerticesUP,
       kJump);
  --t_depth;
  if (t.tracing) Append(t.out, "R %llu r3=%08X\n", (unsigned long long)seq, ctx.r3.u32);
  if (t_depth == 0 && (t.tracing || t.check)) {
    if (t.state_pending) {  // BeginVertices' packets are in the stream once RestorePush ran
      t.state_pending = false;
      if constexpr (kId == kFnRestorePush)
        CompareState(t, kFnBeginVertices);
      else
        ++t.state_unchecked;
    }
    if constexpr (kFns[kId].cat == kDraw) {
      if constexpr (kId == kFnBeginVertices)
        t.state_pending = true;
      else
        CompareState(t, kId);
    }
  }
  if constexpr (kId == kFnSwap) EndFrame(t, base);
}

// Native renderer dispatch (--rexnative_beside_xenos). Only the outermost draw/clear/resolve/Swap call
// reaches the renderer: the D3D's own draws inside them (Clear -> ClearF -> DrawIndx2Inline,
// Resolve's copy quads, Swap -> DrawIndx2Split, BeginVertices inside DrawVerticesUP) are not
// game draws. Other hooked functions do not count as outer calls (BeginTiling fn_820EC610
// clears through a nested ClearF, which must reach OnClear). A top-level BeginVertices (or
// BeginIndexedVertices) only reserves space: the game writes the vertices after it returns, so
// its OnDraw is held until the entry of RestorePush (EndVertices) or of the next hooked call that
// is not a pass-through.
// The query calls reach it at every depth (native/occlusion.h).
enum NativeClass : uint8_t { kNatNone, kNatDraw, kNatClear, kNatResolve, kNatSwap, kNatQuery };

constexpr NativeClass NativeClassOf(Cat c) {
  switch (c) {
    case kDraw:
      return kNatDraw;
    case kClear:
      return kNatClear;
    case kResolve:
      return kNatResolve;
    case kSwap:
      return kNatSwap;
    case kQuery:
      return kNatQuery;
    default:
      return kNatNone;
  }
}

thread_local uint32_t t_native_depth = 0;  // active draw/clear/resolve/Swap calls
thread_local bool t_pending_vertices = false;
thread_local rexnative::HookArgs t_pending_args;
thread_local uint32_t t_original_depth = 0;  // hooked originals running (profiler on)
thread_local rexnative::TilingCall t_tiling_call = rexnative::TilingCall::kNone;
thread_local bool t_tiling_open = false;  // between BeginTiling and EndTiling (TilingCall::kInside)
// The device of this thread's last hooked call that takes one (kFollowSwapDeviceOnly): the
// queries' (OnHook).
thread_local uint32_t t_device = 0;

// Hooked functions that neither the renderer nor the state tracker needs to see: no draw,
// clear, resolve or Swap, no state flush (d3d_state.h IsStateFlushingFn), not the tiling
// bracket and not CreateDevice. Without the tap they run straight through, and without the
// profiler too they skip OnHook (HasFastPath).
constexpr bool IsPassThrough(FnId id) {
  return NativeClassOf(kFns[id].cat) == kNatNone &&
         !rexnative::IsStateFlushingFn(kFns[id].addr) && id != kFnBeginTiling &&
         id != kFnEndTiling && id != kFnCreateDevice;
}

// The pass-through hooks with a fast path. RestorePush (EndVertices) keeps OnHook's: the draw a
// top-level BeginVertices holds is queued at its entry, once the game has written the vertices.
constexpr bool HasFastPath(FnId id) { return IsPassThrough(id) && id != kFnRestorePush; }

using GuestFn = void (*)(PPCContext&, uint8_t*);

// profile::kGuestD3d by function (--rexnative_profile), on the thread that calls Swap.
struct GuestD3dTimes {
  uint64_t ticks[kFnCount];
  uint64_t calls[kFnCount];
  uint32_t frames;
};
thread_local GuestD3dTimes t_guest_d3d_times;

// Every --rexnative_d3d_stats_frames Swaps: the functions with the most guest D3D time.
[[gnu::noinline]] void LogGuestD3dTimes() {
  GuestD3dTimes& t = t_guest_d3d_times;
  const int32_t every = REXCVAR_GET(rexnative_d3d_stats_frames);
  if (every <= 0 || ++t.frames < uint32_t(every)) return;
  uint32_t order[kFnCount];
  std::iota(order, order + kFnCount, 0u);
  const uint32_t shown = std::min<uint32_t>(12, kFnCount);
  std::partial_sort(order, order + shown, order + kFnCount,
                    [&](uint32_t a, uint32_t b) { return t.ticks[a] > t.ticks[b]; });
  const double n = t.frames;
  std::string line;
  char item[96];
  for (uint32_t k = 0; k < shown && t.ticks[order[k]]; ++k) {
    const uint32_t i = order[k];
    std::snprintf(item, sizeof(item), " %s %.0f us %.0f calls,", kFns[i].name,
                  double(rexnative::TicksToNs(t.ticks[i])) / 1e3 / n, double(t.calls[i]) / n);
    line += item;
  }
  if (!line.empty()) line.pop_back();
  REXLOG_INFO("ReXNative guest D3D: per frame by outermost call:{}", line);
  t = GuestD3dTimes{};
}

// Runs a hooked guest D3D function. The outermost one is timed as profile::kGuestD3d, with the
// hooked calls it makes (Clear's ClearF and DrawIndx2Inline, for one) and their tracking.
template <FnId kId, GuestFn kFn>
inline void RunOriginal(PPCContext& c, uint8_t* b) {
  namespace nat = rexnative;
  if (!nat::profile::On()) [[likely]] {
    kFn(c, b);
    return;
  }
  if (t_original_depth++ == 0) {
    const uint64_t start = nat::Ticks();
    kFn(c, b);
    const uint64_t ticks = nat::Ticks() - start;
    nat::profile::g.ticks[nat::profile::kGuestD3d] += ticks;
    t_guest_d3d_times.ticks[kId] += ticks;
    ++t_guest_d3d_times.calls[kId];
  } else {
    kFn(c, b);
  }
  --t_original_depth;
}

// The guest function, or its replacement (--rexnative_replace_draws, native/guest_d3d.h), which
// writes no packets: only on the presenting device (MayDropPackets).
template <FnId kId, GuestFn kOriginal>
void Body(PPCContext& c, uint8_t* b) {
  if constexpr (kId == kFnDrawIndexed)
    if (MayDropPackets(c.r3.u32) && rexnative::guest_d3d::DrawIndexed(c, b)) return;
  kOriginal(c, b);
}

// Queues the draw a top-level BeginVertices holds: the game has written its vertices by now.
// A hook's native work, guarded (native/guard.h): an access violation in it hands the game to
// the Xenos plugin (NativeRenderer::NoteCrash) instead of ending it. Nothing runs after one.
template <class F>
inline bool Native(const char* where, F&& f) {
  if (rexnative::NativeRenderer::Crashed()) [[unlikely]]
    return false;
  if (rexnative::Guarded(f)) [[likely]]
    return true;
  rexnative::NativeRenderer::NoteCrash(where);
  return false;
}

inline void QueuePendingVertices(rexnative::NativeRenderer* r, const uint8_t* base) {
  if (t_pending_vertices && r) {
    t_pending_vertices = false;
    r->OnDraw(base, t_pending_args);
  }
}

// r3..r10 at a hooked call's entry (HookArgs::r, the state tracker's `r`).
inline std::array<uint32_t, 8> EntryRegs(const PPCContext& ctx) {
  return {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
          ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32};
}

// r3..r10 of a hooked call in the public signature's order (hook_args.h), which the renderer and
// the state tracker read. LTCG builds drop and move arguments: the hooked Resolve may be an inner
// function, and Iron Brigade's DrawIndexedVertices has no BaseVertexIndex (start and count in
// r5, r6), its DrawVertices only the count (r5). game_layer.h gives, per function, the entry
// register of each public argument (0 = constant 0); the stack arguments stay where the public
// call has them.
inline std::array<uint32_t, 8> PublicRegs(uint32_t fn, const PPCContext& ctx) {
  namespace game = rexnative::game;
  const std::array<uint32_t, 8> raw = EntryRegs(ctx);
  const uint8_t* map = nullptr;
  switch (rexnative::HookFn(fn)) {
    case rexnative::HookFn::kResolve:
      if constexpr (game::kResolveRemapArgs) map = game::kResolveArgRegs;
      break;
    case rexnative::HookFn::kDrawIndexedVertices:
      if constexpr (game::kRemapDrawIndexedVertices) map = game::kArgsDrawIndexedVertices;
      break;
    case rexnative::HookFn::kDrawVertices:
      if constexpr (game::kRemapDrawVertices) map = game::kArgsDrawVertices;
      break;
    case rexnative::HookFn::kDrawVerticesUP:
      if constexpr (game::kRemapDrawVerticesUP) map = game::kArgsDrawVerticesUP;
      break;
    case rexnative::HookFn::kBeginVertices:
      if constexpr (game::kRemapBeginVertices) map = game::kArgsBeginVertices;
      break;
    default:
      break;
  }
  std::array<uint32_t, 8> r = raw;
  if (map)
    for (int i = 0; i < 8; ++i) r[i] = map[i] ? raw[map[i] - 3] : 0;
  // An LTCG BeginVertices with its stride built in (Darwinia+'s multiplies the count by 44 and
  // never reads r6, which holds whatever the caller left there): the vertex range it reserved
  // came out a few bytes long, the rest of each quad was stale shared memory, and the intro's
  // quads were drawn as long wedges across the screen.
#ifdef REXNATIVE_BEGIN_VERTICES_STRIDE
  if (rexnative::HookFn(fn) == rexnative::HookFn::kBeginVertices)
    r[3] = REXNATIVE_BEGIN_VERTICES_STRIDE;
#endif
  return r;
}

// A hooked call's arguments at its entry, before the guest function clobbers them.
inline rexnative::HookArgs EntryArgs(uint32_t fn, const PPCContext& ctx) {
  rexnative::HookArgs a;
  a.fn = rexnative::HookFn(fn);
  a.device = ctx.r3.u32;
  const std::array<uint32_t, 8> r = PublicRegs(fn, ctx);
  std::copy(r.begin(), r.end(), a.r);
  a.sp = ctx.r1.u32;
  a.f[0] = ctx.f1.f64;
  a.f[1] = ctx.f2.f64;
  a.f[2] = ctx.f3.f64;
  a.f[3] = ctx.f4.f64;
  a.ret = 0;
  a.tiling = t_tiling_call != rexnative::TilingCall::kNone ? t_tiling_call
             : t_tiling_open                                  ? rexnative::TilingCall::kInside
                                                              : rexnative::TilingCall::kNone;
  // An LTCG Resolve that passes ClearStencil in a register (game_layer.h; older layers have
  // none: on the stack).
#if defined(REXNATIVE_RESOLVE_STENCIL_REG) && REXNATIVE_RESOLVE_STENCIL_REG >= 3 && \
    REXNATIVE_RESOLVE_STENCIL_REG <= 10
  if (rexnative::HookFn(fn) == rexnative::HookFn::kResolve)
    a.stencil = int32_t(EntryRegs(ctx)[REXNATIVE_RESOLVE_STENCIL_REG - 3] & 0xFF);
#endif
  return a;
}

// Whether the state tracker (native/d3d_state) follows the guest D3D: for the native renderer
// `r` (NativeRenderer::Live) and for the tap's compare.
inline bool TrackState(const rexnative::NativeRenderer* r) {
  if constexpr (rexnative::game::kStreamOnly) return false;  // no device layout to read
  return r || g_tap.enabled || REXCVAR_GET(rexnative_d3d_tap);
}

// The tap's register mirror for the writes a replayed command buffer makes (cmd_replay.h).
void TapMirrorWrite(uint32_t reg, uint32_t v) { WriteReg(g_tap, reg, v); }

// After a RunCommandBuffer on `device` returned and the tracker's AfterCall ran for it: the
// replay (native/cmd_replay.h), or on any other thread than the Swap thread only the tracker's
// end of the call, before anything of the tap's is read. `r`: the live renderer, or null.
[[gnu::noinline]] void AfterRunCommandBuffer(const uint8_t* base, uint32_t device, uint32_t cb,
                                             rexnative::NativeRenderer* r) {
  namespace cmdbuf = rexnative::cmdbuf;
  if (!cmdbuf::OnSwapThread()) [[unlikely]] {
    cmdbuf::AfterRunOtherThread(base, device, cb);
    return;
  }
  cmdbuf::RunContext rc;
  rc.device = device;
  rc.cb = cb;
  rc.renderer = r;
  rc.main_device = g_main_device.load(std::memory_order_relaxed);
  rc.follow_swap_device_only = rexnative::game::kFollowSwapDeviceOnly;
  rc.in_native_call = t_native_depth != 0;
  const bool tap = g_tap.enabled && g_tap.device == device;
  rc.trace = tap && g_tap.tracing ? &g_tap.out : nullptr;
  rc.tap_write = tap ? &TapMirrorWrite : nullptr;
  rc.frame = g_tap.frame_index;
  cmdbuf::AfterRun(base, rc);
}

// The state tracker (native/d3d_state) needs the dirty masks at the entry of every flushing
// call, at every depth, and the call's exit before anything reads the state (the tap's per-draw
// compare inside OnCall, OnDraw here). So the original is wrapped; OnCall's skip path does not
// run it and nothing is tracked then. Everything that depends on the function is resolved at
// compile time: each hook holds only its own path.
template <FnId kId, GuestFn kOriginal>
inline void OnHookInner(PPCContext& ctx, uint8_t* base) {
  namespace nat = rexnative;
  constexpr uint32_t kAddr = kFns[kId].addr;
  constexpr NativeClass kClass = NativeClassOf(kFns[kId].cat);
  if constexpr (IsPassThrough(kId)) {
    if (!g_tap.enabled) {
      if constexpr (!HasFastPath(kId))
        if (t_pending_vertices)
          Native(kFns[kId].name, [&] { QueuePendingVertices(nat::NativeRenderer::Live(), base); });
      RunOriginal<kId, kOriginal>(ctx, base);
      return;
    }
  }
  // Iron Brigade draws on more than one guest D3D device: besides the one it presents with
  // (Swap's), a second device object (0x6000 bytes after it) is driven from another thread while
  // a level loads [INFERENCE: XDK command buffer recording, replayed by RunCommandBuffer]. The
  // renderer and its state tracker follow a single device, so calls on any other device run the
  // guest function only, as with the renderer off.
  if constexpr (rexnative::game::kFollowSwapDeviceOnly && kId == kFnSwap) {
    uint32_t none = 0;
    g_main_device.compare_exchange_strong(none, ctx.r3.u32, std::memory_order_relaxed);
  }
  // Recorded command buffers: the replay's thread and mode follow the Swaps on the presenting
  // device (every Swap when the game does not single one out).
  if constexpr (kId == kFnSwap && kFnRunCommandBuffer != kFnCount) {
    const uint32_t main_device = g_main_device.load(std::memory_order_relaxed);
    if (!main_device || ctx.r3.u32 == main_device)
      nat::cmdbuf::OnSwap(REXCVAR_GET(rexnative_d3d_stats_frames));
  }
  // RunCommandBuffer's arguments, for after the guest function (which clobbers r3 and r4). Its
  // replay runs from the kNatNone path only: as a draw it would reach OnDraw with the object.
  static_assert(kId != kFnRunCommandBuffer || kClass == kNatNone,
                "d3d_surface.inc: RunCommandBuffer must be kOther");
  [[maybe_unused]] const uint32_t entry_r3 = ctx.r3.u32, entry_r4 = ctx.r4.u32;
  nat::NativeRenderer* r = nat::NativeRenderer::Live();
  // After a fault in the native path (NoteCrash) only Swap reaches the renderer: it hands the game
  // to the Xenos plugin.
  if (kId != kFnSwap && r && nat::NativeRenderer::Crashed()) [[unlikely]]
    r = nullptr;
  // The state tracker is one object without locks, fed from the thread that draws on the
  // followed device (and read by the render thread's register log): a call on another device,
  // from the loading thread, must not reach it, with the tap on as well (the tap check run
  // corrupted the heap that way, 0xC0000374, right after the tracker had switched devices).
  bool other_device = false;
  if constexpr (rexnative::game::kFollowSwapDeviceOnly &&
                (nat::IsStateFlushingFn(kAddr) || kClass != kNatNone)) {
    const uint32_t main_device = g_main_device.load(std::memory_order_relaxed);
    // QueryIssue and QueryGetData take the query object, not the device: the device this thread
    // called the D3D with last stands for it. (Taken for a device, every query was "another
    // device's", so --rexnative_occlusion counted none in a game that follows Swap's device.)
    uint32_t device = ctx.r3.u32;
    if constexpr (kClass == kNatQuery)
      device = t_device;
    else
      t_device = device;
    if (main_device && device && device != main_device) [[unlikely]] {
      if (r)
        REXNATIVE_LOG_FIRST(4, INFO, "{} on device {:08X}, not the presenting {:08X}: "
                       "guest only", kFns[kId].name, device, main_device);
      r = nullptr;
      other_device = true;
    }
  }
  const bool track = !other_device && TrackState(r);
  auto run = [&](PPCContext& c, uint8_t* b) {
    if constexpr (nat::IsStateFlushingFn(kAddr)) {
      if (track && c.r3.u32) {
        const std::array<uint32_t, 8> rr = PublicRegs(kAddr, c);
        const uint32_t sp = c.r1.u32;  // at entry: stack arguments (BeginIndexedVertices)
        nat::CallEntry e{};
        const bool entered = Native(kFns[kId].name, [&] {
          nat::profile::Scope scope(nat::profile::kTrack);
          e = nat::ReadDirtyMasks(b, rr[0]);
          if constexpr (kId == kFnDrawIndexed) nat::PrefetchIndexBuffer(b, rr[0]);
        });
        RunOriginal<kId, Body<kId, kOriginal>>(c, b);
        if (entered)
          Native(kFns[kId].name, [&] {
            nat::profile::Scope scope(nat::profile::kTrack);
            ++nat::profile::g.track_calls;
            nat::GuestState().AfterCall(b, kAddr, e, rr.data(), c.r3.u64, sp);
          });
        return;
      }
    }
    RunOriginal<kId, Body<kId, kOriginal>>(c, b);
  };
  if (!r) {
    if constexpr (kClass == kNatDraw) {
      // The outermost draw call only, as the renderer counts them (rexnative.auto_handover).
      if (!other_device && t_native_depth == 0) nat::NativeRenderer::NoteSuspendedDraw();
      ++t_native_depth;
      OnCall<kId>(ctx, base, run);
      --t_native_depth;
    } else {
      OnCall<kId>(ctx, base, run);
    }
    // `track && entry_r3`: exactly when `run` called AfterCall, which EndCommandBuffer pairs.
    if constexpr (kId == kFnRunCommandBuffer)
      if (track && entry_r3)
        Native(kFns[kId].name,
               [&] { AfterRunCommandBuffer(base, entry_r3, entry_r4, nullptr); });
    if constexpr (kId == kFnSwap) {
      if (nat::NativeRenderer* suspended = nat::NativeRenderer::Get()) suspended->OnSwapSuspended();
      NoteFrameBegin();
    }
    return;
  }
  if constexpr (!HasFastPath(kId)) Native(kFns[kId].name, [&] { QueuePendingVertices(r, base); });
  if constexpr (kClass == kNatQuery) {
    // GetData reads the result the renderer writes at its entry; an Issue reaches the renderer
    // once the guest D3D has recorded it.
    static_assert(kId == kFnQueryIssue || kId == kFnQueryGetData);
    const nat::HookArgs a = EntryArgs(kAddr, ctx);
    if constexpr (kId == kFnQueryGetData)
      Native(kFns[kId].name, [&] { r->OnQueryGetData(base, a); });
    OnCall<kId>(ctx, base, run);
    if constexpr (kId == kFnQueryIssue) Native(kFns[kId].name, [&] { r->OnQueryIssue(base, a); });
  } else if constexpr (kClass == kNatNone) {
    if constexpr (kId == kFnBeginTiling || kId == kFnEndTiling) {
      const nat::TilingCall outer = t_tiling_call;
      t_tiling_call = kId == kFnBeginTiling ? nat::TilingCall::kBegin : nat::TilingCall::kEnd;
      OnCall<kId>(ctx, base, run);
      t_tiling_call = outer;
      t_tiling_open = kId == kFnBeginTiling;
      if constexpr (kId == kFnEndTiling)
        if (nat::NativeRenderer* const live = nat::NativeRenderer::Live()) live->OnTilingEnd();
    } else if constexpr (kId == kFnRunCommandBuffer) {
      // After OnCall: after AfterCall copied Run's inherited flush and after the tap's exit
      // Sync, so the tap, like the GPU, sees that flush before the buffer's own writes.
      OnCall<kId>(ctx, base, run);
      if (track && entry_r3)
        Native(kFns[kId].name, [&] { AfterRunCommandBuffer(base, entry_r3, entry_r4, r); });
    } else {
      OnCall<kId>(ctx, base, run);
    }
  } else {
    // Only the outermost call reaches the renderer, with its arguments at entry.
    const bool outer = t_native_depth++ == 0;
    nat::HookArgs a;
    if (outer) a = EntryArgs(kAddr, ctx);
    if constexpr (kClass == kNatResolve)
      if (outer) t_reg_resolve_seen = false;
    OnCall<kId>(ctx, base, run);
    --t_native_depth;
    if (!outer) return;
    a.ret = ctx.r3.u64;
    if constexpr (kClass == kNatDraw) {
      if constexpr (kId == kFnBeginVertices || kId == kFnBeginIndexedVertices) {
        t_pending_args = a;
        t_pending_vertices = true;
      } else {
        Native(kFns[kId].name, [&] { r->OnDraw(base, a); });
      }
    } else if constexpr (kClass == kNatClear) {
      Native(kFns[kId].name, [&] { r->OnClear(base, a); });
    } else if constexpr (kClass == kNatResolve) {
      Native(kFns[kId].name, [&] {
        if (std::exchange(t_reg_resolve_seen, false)) CheckRegisterResolve(base, a);
        r->OnResolve(base, a);
      });
    } else {
      static_assert(kClass == kNatSwap);
      // Also after a fault (NoteCrash): this Swap hands the game to the Xenos plugin.
      auto swap = [&] { r->OnSwap(base, a); };
      if (!nat::Guarded(swap)) {
        nat::NativeRenderer::NoteCrash(kFns[kId].name);
        r->HandOverAfterCrash(base, a.device);
      }
      NoteFrameBegin();
      if (nat::profile::On()) LogGuestD3dTimes();
    }
  }
}

// rexnative.resolve_check_registers: the hooked Resolve `a` decoded from its arguments and from
// the registers at its copy draw (t_reg_resolve): the same copy, rectangle, destination texel
// (the guest moves the base by whole rows of tiles for a destination point below the
// rectangle), format and clears, or a log line. A count every 300 checks.
[[gnu::noinline]] void CheckRegisterResolve(const uint8_t* base, const rexnative::HookArgs& a) {
  static uint64_t checks = 0, agree = 0, failed = 0;
  rexnative::ResolveParams h, g;
  if (!rexnative::DecodeResolve(base, a, &h)) return;
  ++checks;
  const uint8_t* physical = rexnative::host::Memory()->GetPhysicalMembase();
  bool same = rexnative::DecodeRegisterResolve(physical, t_reg_resolve, &g);
  if (!same) ++failed;
  std::string why;
  auto differ = [&](const char* what, int64_t x, int64_t y) {
    if (x == y) return;
    same = false;
    why += std::format(" {} {}/{}", what, x, y);
  };
  if (same) {
    differ("source", h.source, g.source);
    differ("copy", h.copy_command, g.copy_command);
    differ("samples", h.sample_select, g.sample_select);
    differ("x0", h.x0 & ~7, g.x0);
    differ("y0", h.y0 & ~7, g.y0);
    differ("x1", h.x1, g.x1);
    differ("y1", h.y1, g.y1);
    differ("format", h.dest_format, g.dest_format);
    differ("number", h.dest_number, g.dest_number);
    differ("swap", h.dest_swap, g.dest_swap);
    differ("bias", h.exp_bias, g.exp_bias);
    differ("clears", h.clear_flags, g.clear_flags);
    // Where the rectangle's first pixel lands: (dest_x, dest_y) at h's level base, its own
    // coordinates at g's base.
    const int32_t rows = h.dest_y - h.y0;
    const int64_t moved = rows % 32 ? -1
                                    : int64_t(rows / 32) * g.dest_pitch_blocks * 32 *
                                          (g.dest_extent_bytes / (uint64_t(g.dest_pitch_blocks) *
                                                                  ((g.dest_height + 31) & ~31u)));
    differ("dest_x", h.dest_x - h.x0, 0);
    differ("dest_base", int64_t(h.dest_base) + moved, int64_t(g.dest_base));
    if (h.clear_flags & 0x100)
      for (int i = 0; i < 4; ++i)
        differ("clear_color", std::lround(h.clear_color[i] * 255.0f),
               std::lround(g.clear_color[i] * 255.0f));
    if (h.clear_flags & 0x200) {
      differ("clear_depth", std::lround(h.clear_depth * 16777215.0f),
             std::lround(g.clear_depth * 16777215.0f));
      differ("clear_stencil", h.clear_stencil, g.clear_stencil);
    }
  }
  if (!same) {
    const uint32_t f0 = t_reg_resolve.vertex_fetch[0], f1 = t_reg_resolve.vertex_fetch[1];
    why += std::format(" (fetch {:08X} {:08X}:", f0, f1);
    for (uint32_t i = 0; i < 6 && (f0 & 0x1FFFFFFC) + 24 <= 0x20000000u; ++i) {
      uint32_t raw;
      std::memcpy(&raw, physical + (f0 & 0x1FFFFFFC) + 4 * i, 4);
      why += std::format(" {}", std::bit_cast<float>(__builtin_bswap32(raw)));
    }
    why += std::format(") surface {:08X} window {:08X} {:08X}-{:08X} screen {:08X}-{:08X} mode {:08X}",
                       t_reg_resolve.surface.surface_info, t_reg_resolve.window_offset,
                       t_reg_resolve.window_scissor_tl, t_reg_resolve.window_scissor_br,
                       t_reg_resolve.screen_scissor_tl, t_reg_resolve.screen_scissor_br,
                       t_reg_resolve.su_sc_mode_cntl);
  }
  if (same) {
    ++agree;
  } else {
    REXNATIVE_LOG_FIRST(16, WARN,
                        "ReXNative resolve check: registers disagree:{} (rect {},{}-{},{} point "
                        "{},{} dest {:08X} {}x{} format {}; copy {:08X} dest {:08X} pitch "
                        "{:08X} info {:08X}){}",
                        why, h.x0, h.y0, h.x1, h.y1, h.dest_x, h.dest_y, h.dest_base,
                        h.dest_width, h.dest_height, h.dest_format,
                        t_reg_resolve.copy_control, t_reg_resolve.copy_dest_base,
                        t_reg_resolve.copy_dest_pitch, t_reg_resolve.copy_dest_info,
                        failed && why.empty() ? " (no register decode)" : "");
  }
  if (checks % 300 == 0)
    REXLOG_INFO("ReXNative resolve check: {} resolves, {} decoded alike from the registers, "
                "{} not decoded",
                checks, agree, failed);
}

// rexnative.inline_draws (cmd_replay.h ReplayGap): the push buffer the game wrote between the
// exit of one outermost hooked call on the presenting device and the entry of the next. After a
// Begin call it is vertex data, not packets: no walk then. The cursor is the device's last
// written dword (dev::kPush), so the stream between is (cursor, push].
thread_local uint32_t t_gap_depth = 0;
thread_local uint32_t t_gap_cursor = 0;
thread_local uint32_t t_gap_cursor_device = 0;  // the device t_gap_cursor is a push of
thread_local bool t_gap_valid = false;
// The outer BeginVertices before this gap: its vertices (GPU address, bytes) and count. When
// they are not in the gap's stream (Sega Rally Revo's come from a ring of their own), the gap is
// walked all the same: the game writes the rest of the call there, EndVertices inlined, and then
// its next draw's constants (the skinned meshes' bones, c28 on: without them bone 0 kept another
// draw's values and those vertices went thousands of units away).
thread_local uint32_t t_begin_vertices = 0, t_begin_bytes = 0, t_begin_count = 0;
// The walk's generation: rexnative.inline_draws_auto starts a trial with a new one, and a cursor
// from an older one (before the walk was off) is not walked from.
std::atomic<uint32_t> g_gap_epoch{0};
thread_local uint32_t t_gap_epoch = 0;
// The device Swap presents with, the last one (a game may create its device again): the layer
// may follow every device (kFollowSwapDeviceOnly false, g_main_device stays 0), but only that
// one's stream is walked.
std::atomic<uint32_t> g_gap_device{0};
// rexnative.inline_draws_auto's probe (InlineDrawsAuto): the gaps are validated and the shader
// constants they change counted, nothing more.
std::atomic<bool> g_gap_probe{false};
// Only on the thread that calls Swap: the state tracker has no locks (Orcs Must Die! drives its
// device from a second thread while it loads).
std::atomic<uint64_t> g_gap_thread{0};
uint64_t GapThreadId() {
  static thread_local const uint64_t id = std::hash<std::thread::id>{}(std::this_thread::get_id());
  return id;
}
// The hooked calls another thread made on the gap device (a loader's draws), (entry, exit] push
// values: their packets lie inside the Swap thread's next gap and their hooks drew them already,
// so the walk leaves them out. Past kMaxForeign the gap is not walked at all.
struct GapForeign {
  std::mutex mutex;
  std::vector<std::pair<uint32_t, uint32_t>> calls;
  bool overflow = false;
  std::atomic<bool> any{false};
};
GapForeign g_gap_foreign;
constexpr size_t kMaxForeign = 4096;

[[gnu::noinline]] void NoteForeignCall(uint32_t entry, uint32_t exit) {
  std::lock_guard lock(g_gap_foreign.mutex);
  if (g_gap_foreign.calls.size() < kMaxForeign)
    g_gap_foreign.calls.emplace_back(entry, exit);
  else
    g_gap_foreign.overflow = true;
  g_gap_foreign.any.store(true, std::memory_order_relaxed);
}

[[gnu::noinline]] void GapEntry(uint8_t* base, uint32_t device) {
  const uint32_t push = Load32(base, device + rexnative::dev::kPush);
  std::vector<std::pair<uint32_t, uint32_t>> foreign;
  bool overflow = false;
  if (g_gap_foreign.any.load(std::memory_order_relaxed)) {
    std::lock_guard lock(g_gap_foreign.mutex);
    foreign.swap(g_gap_foreign.calls);
    overflow = std::exchange(g_gap_foreign.overflow, false);
    g_gap_foreign.any.store(false, std::memory_order_relaxed);
  }
  const uint32_t cursor = t_gap_cursor;
  uint32_t held_vertices = 0;
  if (!t_gap_valid && t_begin_count && t_gap_cursor_device == device && push > cursor) {
    const uint32_t v = t_begin_vertices, lo = rexnative::GpuAddress(cursor),
                   hi = rexnative::GpuAddress(push);
    if (v && (v + t_begin_bytes <= lo || v >= hi)) {
      t_gap_valid = true;
      held_vertices = t_begin_count;
    }
  }
  if (!t_gap_valid || t_gap_cursor_device != device ||
      t_gap_epoch != g_gap_epoch.load(std::memory_order_relaxed) || overflow || push <= cursor ||
      push - cursor > (4u << 20))
    return;
  rexnative::NativeRenderer* r = rexnative::NativeRenderer::Live();
  if (!r) return;
  if constexpr (rexnative::game::kStreamOnly) rexnative::GuestState().AttachStream(device);
  rexnative::cmdbuf::RunContext rc;
  rc.device = device;
  rc.renderer = r;
  rc.main_device = device;
  rc.follow_swap_device_only = true;
  rc.in_native_call = t_native_depth != 0;
  rc.probe = g_gap_probe.load(std::memory_order_relaxed);
  rc.held_vertices = held_vertices;
  // (cursor, push] without the other threads' calls in it: dword addresses [first, end).
  std::sort(foreign.begin(), foreign.end());
  uint32_t from = cursor;
  for (const auto& [entry, exit] : foreign) {
    if (exit <= from || entry >= push) continue;  // another gap's (or a wrapped ring's)
    if (entry > from) rexnative::cmdbuf::ReplayGap(base, rc, from + 4, entry + 4);
    from = std::max(from, exit);
  }
  if (from < push) rexnative::cmdbuf::ReplayGap(base, rc, from + 4, push + 4);
}

// rexnative.inline_draws_auto: the hooked draw calls of the frame so far, on the Swap thread,
// and the walk's generation (a cursor from before a trial is not walked from).
uint32_t g_auto_draws = 0;

// At each Swap, unless rexnative.inline_draws is set: a game whose frames reach the hooks with
// at most 2 draw calls while it writes 8 KB or more of push buffer, 30 frames in a row, may draw
// with code its build inlined past them (Orcs Must Die!'s title and menus: 2 hooked draws, the
// rest in the push buffer between). The walk is tried for 60 frames: kept if it drew something,
// else off again, and tried again only after 600 frames, then twice as many each time. Jet Set
// Radio's intro video and Perfect Dark's loading screens look the same and the walk finds no
// draw there: they do not keep the cost of every pass-through hook running OnHook
// (HasFastPath).
// Every kProbeEvery frames, whatever the draw count, a probe of kProbeFrames: the walk only
// validates the gaps and counts the shader constant registers they set to another value than
// the tracker holds. A game whose own code gives the GPU shader constants keeps the walk on:
// Deus Ex: Human Revolution's XDK GPU-owned constants, a LOAD_ALU_CONSTANT of c0-c7 from its own
// ring and a SET_SHADER_CONSTANTS of c8-c15 beside each of its 126 hooked draws, which the
// tracker never saw: every vertex shader's position was 0 and the picture black. Constants the
// tracker holds already do not count. Sega Rally Revo's race keeps the walk on too (its gaps
// change thousands of constants a frame) and draws as it did without it.
// A game whose own code writes resolves keeps the walk on as well when a probe finds at least one
// resolve a frame in its gaps (rexnative.inline_resolves): no hooked Resolve makes them, and the
// textures (or the front buffer: a black screen) they fill would stay empty. (A first probe at
// frame 30 caught SSX's start-up writing 234 constants and kept the walk for good: at 150, 6.)
// Two probes in a row that each find a few keep it as well: a game that goes on writing
// constants past the hooks, not a start-up burst (Sega Rally Revo's menus, 5 a probe, then its
// attract demo, hundreds: the probe after the demo began found them 6 s late, and until then
// the giraffes' skinned meshes, their bones those constants, came out as walls across the sky).
// The decision is kept per game (LearnedInlineDraws): the next runs walk from their first frame.
constexpr uint32_t kProbeEvery = 150, kProbeFrames = 3, kProbeMinConstants = 16;

// <cache root>/rexnative/inline_draws.txt (the pipeline cache's folder): written when the walk is
// kept, with why; read at the first Swap.
std::filesystem::path LearnedInlineDrawsPath() {
  std::filesystem::path dir = rexnative::settings::Path("app.cache_root");
  if (dir.empty()) {
    const std::filesystem::path user = rexnative::settings::Path("app.user_root");
    if (!user.empty()) dir = user / "cache";
  }
  return dir.empty() ? dir : dir / "rexnative" / "inline_draws.txt";
}

void LearnInlineDraws(const std::string& why) {
  const std::filesystem::path path = LearnedInlineDrawsPath();
  if (path.empty()) return;
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream(path) << why << '\n';
}

[[gnu::noinline]] void InlineDrawsAuto(const uint8_t* base, uint32_t device) {
  static uint32_t last_push = 0, quiet = 0, trial = 0, wait = 0, backoff = 600;
  static uint32_t probe = 0, probe_clock = 0;
  static uint64_t trial_draws = 0, probe_constants = 0, probe_resolves = 0;
  static uint32_t probes_with_constants = 0;
  static bool kept = false, learned_read = false;
  const uint32_t draws = std::exchange(g_auto_draws, 0);
  if (kept || !device || !REXCVAR_GET(rexnative_inline_draws_auto) ||
      REXCVAR_GET(rexnative_inline_draws) || rexnative::game::kStreamOnly)
    return;
  if (!learned_read) {
    learned_read = true;
    const std::filesystem::path path = LearnedInlineDrawsPath();
    std::error_code ec;
    if (!path.empty() && std::filesystem::exists(path, ec)) {
      kept = true;
      g_gap_epoch.fetch_add(1, std::memory_order_relaxed);
      g_inline_draws.store(true, std::memory_order_relaxed);
      g_hook_all.store(true, std::memory_order_relaxed);
      REXLOG_INFO("ReXNative: inline draws on from the start, as an earlier run of this game "
                  "found them needed ({}; rexnative.inline_draws_auto)",
                  path.string());
      return;
    }
  }
  if (probe) {
    if (--probe) return;
    g_gap_probe.store(false, std::memory_order_relaxed);
    const uint64_t found = rexnative::cmdbuf::GapConstants() - probe_constants;
    const uint64_t resolves = rexnative::cmdbuf::GapResolves() - probe_resolves;
    if (resolves >= kProbeFrames - 1 && REXCVAR_GET(rexnative_inline_resolves)) {
      kept = true;
      REXLOG_INFO("ReXNative: the game's own code resolves between hooked calls ({} resolves "
                  "in {} frames): inline draws on (rexnative.inline_draws_auto)",
                  resolves, kProbeFrames - 1);
      LearnInlineDraws(std::format("resolves between hooked calls: {} in {} frames", resolves,
                                   kProbeFrames - 1));
      return;
    }
    probes_with_constants = found ? probes_with_constants + 1 : 0;
    if (found >= kProbeMinConstants || probes_with_constants >= 2) {
      kept = true;  // the walk goes on from this Swap's gap
      REXLOG_INFO("ReXNative: the game's own code loads shader constants between hooked calls "
                  "({} constant registers changed in {} frames, {} probes in a row): inline "
                  "draws on (rexnative.inline_draws_auto)",
                  found, kProbeFrames - 1, probes_with_constants);
      LearnInlineDraws(std::format("shader constants between hooked calls: {} in {} frames",
                                   found, kProbeFrames - 1));
      return;
    }
    if (found)
      REXNATIVE_LOG_FIRST(4, INFO, "ReXNative: the push buffer between hooked calls changed {} "
                                   "shader constant registers in {} frames: too few, inline "
                                   "draws stay off until the next probe finds some too "
                                   "(rexnative.inline_draws_auto)",
                          found, kProbeFrames - 1);
    g_inline_draws.store(false, std::memory_order_relaxed);
    g_hook_all.store(g_tap.enabled || REXCVAR_GET(rexnative_profile), std::memory_order_relaxed);
    return;
  }
  if (trial) {
    if (rexnative::cmdbuf::GapDraws() > trial_draws) {
      kept = true;
      REXLOG_INFO("ReXNative: the push buffer between hooked calls holds draws: inline draws "
                  "on (rexnative.inline_draws_auto)");
      LearnInlineDraws("draws between hooked calls");
      return;
    }
    if (--trial) return;
    g_inline_draws.store(false, std::memory_order_relaxed);
    g_hook_all.store(g_tap.enabled || REXCVAR_GET(rexnative_profile), std::memory_order_relaxed);
    wait = backoff;
    backoff = std::min(backoff * 2, 38400u);
    quiet = 0;
    REXNATIVE_LOG_FIRST(4, INFO, "ReXNative: no draws between hooked calls: inline draws off "
                                 "again (rexnative.inline_draws_auto)");
    return;
  }
  const uint32_t push = Load32(base, device + rexnative::dev::kPush);
  const uint32_t written = push > last_push ? push - last_push : 0;  // 0 past a wrap
  last_push = push;
  if (++probe_clock >= kProbeEvery) {
    probe_clock = 0;
    probe = kProbeFrames;
    rexnative::cmdbuf::ProbeReset();
    probe_constants = rexnative::cmdbuf::GapConstants();
    probe_resolves = rexnative::cmdbuf::GapResolves();
    g_gap_epoch.fetch_add(1, std::memory_order_relaxed);
    g_gap_probe.store(true, std::memory_order_relaxed);
    g_inline_draws.store(true, std::memory_order_relaxed);
    g_hook_all.store(true, std::memory_order_relaxed);
    return;
  }
  if (wait) {
    --wait;
    return;
  }
  quiet = draws <= 2 && written >= 8192 ? quiet + 1 : 0;
  if (quiet < 30) return;
  trial = 60;
  trial_draws = rexnative::cmdbuf::GapDraws();
  g_gap_epoch.fetch_add(1, std::memory_order_relaxed);
  g_inline_draws.store(true, std::memory_order_relaxed);
  g_hook_all.store(true, std::memory_order_relaxed);
  REXNATIVE_LOG_FIRST(4, INFO, "ReXNative: {} frames with at most 2 hooked draw calls and {} bytes "
                               "of push buffer in the last: inline draws tried "
                               "(rexnative.inline_draws_auto)",
                      quiet, written);
}

template <FnId kId, GuestFn kOriginal>
inline void OnHook(PPCContext& ctx, uint8_t* base) {
  // game::kStreamOnly: only Swap and the push buffer's own functions bound the walk's gaps (a
  // gap leaves out what a hooked call writes, and the walk must see every packet); every other
  // hook runs the guest only, its device state unread.
  if constexpr (rexnative::game::kStreamOnly && kId != kFnSwap && kId != kFnCreateDevice &&
                kFns[kId].cat != kPush)
    return RunOriginal<kId, kOriginal>(ctx, base);
  if constexpr (kId == kFnSwap) InlineDrawsAuto(base, ctx.r3.u32);
  if (!g_inline_draws.load(std::memory_order_relaxed)) [[likely]] {
    if constexpr (NativeClassOf(kFns[kId].cat) == kNatDraw) ++g_auto_draws;
    return OnHookInner<kId, kOriginal>(ctx, base);
  }
  if constexpr (NativeClassOf(kFns[kId].cat) == kNatDraw) ++g_auto_draws;
  const uint32_t device = ctx.r3.u32;
  if constexpr (kId == kFnSwap) {
    if (device) {
      g_gap_device.store(device, std::memory_order_relaxed);
      g_gap_thread.store(GapThreadId(), std::memory_order_relaxed);
    }
  }
  const bool gap_device = t_gap_depth == 0 && device &&
                          device == g_gap_device.load(std::memory_order_relaxed);
  const bool outer =
      gap_device && GapThreadId() == g_gap_thread.load(std::memory_order_relaxed);
  const bool foreign = gap_device && !outer;
  const uint32_t foreign_entry = foreign ? Load32(base, device + rexnative::dev::kPush) : 0;
  if (outer) {
    // The draw a top-level BeginVertices holds comes before the stream the game wrote after its
    // vertices: the walk draws that stream and leaves its registers on the tracker. Fable II
    // inlines EndVertices (no RestorePush call), so the held draw waited for the next flushing
    // call: its tonemap rectangle came after the HUD's inline draws, with their shaders. The
    // vertices are written by now: no D3D call comes between BeginVertices and EndVertices.
    if (t_pending_vertices && !g_gap_probe.load(std::memory_order_relaxed))
      Native(kFns[kId].name,
             [&] { QueuePendingVertices(rexnative::NativeRenderer::Live(), base); });
    Native(kFns[kId].name, [&] { GapEntry(base, device); });
  }
  const uint32_t begin_count = ctx.r5.u32, begin_stride = ctx.r6.u32;
  ++t_gap_depth;
  OnHookInner<kId, kOriginal>(ctx, base);
  --t_gap_depth;
  if (outer) {
    t_gap_cursor = Load32(base, device + rexnative::dev::kPush);
    t_gap_cursor_device = device;
    t_gap_epoch = g_gap_epoch.load(std::memory_order_relaxed);
    t_gap_valid = kId != kFnBeginVertices && kId != kFnBeginIndexedVertices;
    if constexpr (kId == kFnBeginVertices) {
      // r3: the vertices' address, 0 when there was no space (nothing written).
      t_begin_vertices = ctx.r3.u32 ? rexnative::GpuAddress(ctx.r3.u32) : 0;
      t_begin_bytes = begin_count * begin_stride;
      t_begin_count = begin_count;
    } else {
      t_begin_count = 0;
    }
    if constexpr (kId == kFnSwap)
      rexnative::cmdbuf::OnSwapGaps(REXCVAR_GET(rexnative_d3d_stats_frames));
  } else if (foreign) {
    if (const uint32_t exit = Load32(base, device + rexnative::dev::kPush); exit != foreign_entry)
      NoteForeignCall(foreign_entry, exit);
  }
}
}  // namespace

// Cumulative counters for the SDK's --gpu_stream_stats (instrument patch): names
// starting with "draw" are summed into its api_total, as in DrawCounter's hooks.
extern "C" __declspec(dllexport) uint32_t rex_drawcount_api_counters(const uint64_t** values,
                                                                     const char* const** names) {
  // kDrawCounterFns, then Resolve, then g_tap_counts.
  static constexpr const char* kNames[] = {"draw_BeginVertices", "draw_DrawVertices",
                                           "draw_DrawIndexedVertices", "draw_DrawIndx2Inline",
                                           "draw_DrawIndx2Split", "aux_Resolve",
                                           "tap_draws",     "tap_aux",       "tap_resolves"};
  constexpr size_t kDraws = std::size(kDrawCounterFns), kCount = std::size(kNames);
  static_assert(kCount == kDraws + 1 + std::size(g_tap_counts));
  static uint64_t snapshot[kCount];
  for (size_t i = 0; i < kDraws; ++i)
    snapshot[i] = g_calls[kDrawCounterFns[i]].load(std::memory_order_relaxed);
  snapshot[kDraws] = g_calls[kFnResolve].load(std::memory_order_relaxed);
  for (size_t i = 0; i < std::size(g_tap_counts); ++i)
    snapshot[kDraws + 1 + i] = g_tap_counts[i].load(std::memory_order_relaxed);
  *values = snapshot;
  *names = kNames;
  return uint32_t(kCount);
}

bool RexNativeGame60Fps();  // games/pgr4/src/pgr4_hooks.cpp

// The presentation interval KickOffEx (sub_826CACA0) hands to the GPU, read from the device
// (Pgr4PresentationIntervalHook). While the native renderer paces the frames on the display the
// guest presents at once: waiting for a vblank as well would pace the game on ReXGlue's vblank
// timer a second time (frame_pacer.h). Otherwise the game's own interval, or ONE with the 60 FPS
// option.
uint32_t RexNativePresentationInterval(uint32_t guest) {
  constexpr uint32_t kOne = 1, kImmediate = 0x80000000;  // D3DPRESENT_INTERVAL_*
  if (rexnative::NativeRenderer::PacesFrames()) return kImmediate;
  return RexNativeGame60Fps() ? kOne : guest;
}

// Whether the game runs faster than the console's 30 FPS.
bool RexNativeFrameRateUnlocked() {
  return rexnative::NativeRenderer::PacesFrames() || RexNativeGame60Fps();
}

// The motion blur pass (sub_82859FC0) smears each pixel along its reprojection to the previous
// frame (c154-c157), which spans the scene's motion over one time step. Its per-sample step
// (c159), the tweak's strength over the sample count, was tuned on the console's 30 FPS, so at
// 90 fps the blur was a third as long. Scaled by the steps per 1/30 s, it is the blur of a 30 FPS
// frame at any frame rate: in the same race at 30 and 90 fps the reprojection moved 2.95 times as
// far per frame at 30, and its product with the step matched within 2 % (td-127, td-128).
// Pgr4MotionBlurStepHook calls this.
float RexNativeMotionBlurStep(float step) {
  constexpr double kConsoleFrameNs = 1e9 / 30;
  constexpr int64_t kMinStepNs = 1'000'000, kMaxStepNs = 100'000'000;
  // Not measured yet (the first frames): the console's step. Clamped: steps under 1 ms are frames
  // queued behind a stall, which begin at once; steps over 100 ms are loads.
  const int64_t smooth_ns = g_smooth_step_ns.load(std::memory_order_relaxed);
  const int64_t frame_ns =
      smooth_ns ? std::clamp(smooth_ns, kMinStepNs, kMaxStepNs) : int64_t(kConsoleFrameNs);
  return float(step * (kConsoleFrameNs / double(frame_ns)));
}

#define D3D_FN(a, n, c)                                           \
  REX_EXTERN(__imp__sub_##a);                                     \
  REX_HOOK_RAW(sub_##a) {                                         \
    if constexpr (HasFastPath(kFn_##a)) {                         \
      if (!g_hook_all.load(std::memory_order_relaxed)) [[likely]] \
        return __imp__sub_##a(ctx, base);                         \
    }                                                             \
    OnHook<kFn_##a, __imp__sub_##a>(ctx, base);                   \
  }
#include "d3d_surface.inc"
#undef D3D_FN

// XInputGetState(user, state): the game's pad reads pass through the settings HUD (hud.h,
// FilterPad), which opens it on Back+Start, feeds it the pad, keeps the chord (and, for a
// moment, a lone Back or Start) from the game and gives the game a neutral pad while it is up.
// Here, on the thread the game reads its pads on: the runtime's input system is not made for
// reads from two threads at once (a poll from the Swap thread corrupted the heap). Only while the
// native renderer shows the frames, its own or the Xenos plugin's (it draws the HUD over them). A
// port that hooks the function itself defines
// REXNATIVE_GAME_HOOKS_XINPUT (From Dust: frame_rate.cpp).
#if defined(REXNATIVE_XINPUT_GET_STATE_FOUND) && REXNATIVE_XINPUT_GET_STATE_FOUND && \
    !defined(REXNATIVE_GAME_HOOKS_XINPUT)
#ifndef REXNATIVE_XINPUT_STATE_REG  // layers before it: XInputGetState(user, state)
#define REXNATIVE_XINPUT_STATE_REG 4
#endif
REX_EXTERN(REXNATIVE_IMP(REXNATIVE_XINPUT_GET_STATE));
REX_HOOK_RAW(REXNATIVE_SUB(REXNATIVE_XINPUT_GET_STATE)) {
  // XInputGetState(user, state), or the XamInputGetState thunk (user, flags, state).
  const uint32_t user = ctx.r3.u32,
                 state = REXNATIVE_XINPUT_STATE_REG == 5 ? ctx.r5.u32 : ctx.r4.u32;
  REXNATIVE_IMP(REXNATIVE_XINPUT_GET_STATE)(ctx, base);
  // The control file's `pad` (g_pad_state): on user 0, connected for it if no pad is, with a new
  // packet number each read (games read a pad again only when it changed).
  if (user == 0 && state &&
      rexnative::NowNs() < g_pad_until_ns.load(std::memory_order_acquire)) {
    const uint64_t pad = g_pad_state.load(std::memory_order_relaxed);
    uint8_t* s = base + state;
    if (ctx.r3.u32 != 0) {
      std::memset(s, 0, 16);
      ctx.r3.u64 = 0;
    }
    rex::memory::store_and_swap<uint32_t>(s, rex::memory::load_and_swap<uint32_t>(s) + 1);
    rex::memory::store_and_swap<uint16_t>(
        s + 4, uint16_t(rex::memory::load_and_swap<uint16_t>(s + 4) | uint16_t(pad)));
    if (pad >> 48 & 1) {
      rex::memory::store_and_swap<uint16_t>(s + 8, uint16_t(pad >> 16));
      rex::memory::store_and_swap<uint16_t>(s + 10, uint16_t(pad >> 32));
    }
    const uint64_t pad2 = g_pad_state2.load(std::memory_order_relaxed);
    if (pad2 >> 32 & 1) {
      rex::memory::store_and_swap<uint16_t>(s + 12, uint16_t(pad2));
      rex::memory::store_and_swap<uint16_t>(s + 14, uint16_t(pad2 >> 16));
    }
    if (pad2 >> 56 & 1) {
      s[6] = uint8_t(pad2 >> 40);
      s[7] = uint8_t(pad2 >> 48);
    }
  }
  if (ctx.r3.u32 != 0 || !state || user >= 4 || !rexnative::NativeRenderer::ShowsFrames()) return;
  rexnative::hud::FilterPad(user, base + state + 4);  // XINPUT_STATE: packet, then the pad
}
#endif

// ImmediateVs: the D3D's immediate vertex shader load (the variant copied into the push buffer
// and patched for the bound vertex declaration), called from the draw flush ShaderLoad. The
// state tracker keeps the patched ucode for the native draws (d3d_state.h, ShaderRef).
#if REXNATIVE_IMMEDIATE_VS_FOUND
REX_EXTERN(REXNATIVE_IMP(REXNATIVE_IMMEDIATE_VS));
REX_HOOK_RAW(REXNATIVE_SUB(REXNATIVE_IMMEDIATE_VS)) {
  const std::array<uint32_t, 8> r = EntryRegs(ctx);
  REXNATIVE_IMP(REXNATIVE_IMMEDIATE_VS)(ctx, base);
  // Only on the followed device (OnHook): a draw on another device, from another thread, would
  // otherwise hand its patched VS to the next draw on the followed one, and race the tracker.
  if constexpr (rexnative::game::kFollowSwapDeviceOnly) {
    const uint32_t main_device = g_main_device.load(std::memory_order_relaxed);
    if (main_device && r[0] != main_device) return;
  }
  if (TrackState(rexnative::NativeRenderer::Live()))
    Native("ImmediateVs", [&] {
      rexnative::profile::Scope scope(rexnative::profile::kImmediateVs);
      rexnative::GuestState().AfterImmediateVs(base, r.data());
    });
}
#endif
