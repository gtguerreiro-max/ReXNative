// ReXNative renderer: recorded command buffers replayed as native draws. See cmd_replay.h.
//
// PM4 semantics follow the SDK's command processor (rexglue-sdk, command_processor.cpp): type-0
// runs and single-register writes, type-1 pairs (registers below 0x800, none of which a
// draw reads), type-2 and zero dwords as padding, and type-3 packets, which run only when their
// predicate bit is clear or (bin select & bin mask) != 0. Only what the native draw path needs
// is carried out (register state, shader loads, draws); fences, events, memory writes and waits
// are the SDK command processor's, which still executes them inside the same buffers under
// --gpu_skip_draws.
#include "cmd_replay.h"

#include "clock.h"
#include "d3d_state.h"
#include "guest_memory.h"
#include "log.h"
#include "profile.h"
#include "renderer.h"

#include <rex/cvar.h>
#include <rex/kernel/heap.h>
#include <rex/logging.h>
#include "host.h"
#include <xxhash.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

REXCVAR_DEFINE_INT32(rexnative_cmdbuf, -1, "ReXNative",
                     "Recorded command buffers (RunCommandBuffer): -1 the game layer's default, "
                     "0 off, 1 count (decode and log them, no effect), 2 replay their draws");
REXCVAR_DEFINE_INT32(rexnative_cmdbuf_log, 4, "ReXNative",
                     "With --rexnative_cmdbuf 1 or 2: how many RunCommandBuffer calls to log in "
                     "detail (header, chunks, opcode histogram)");
REXCVAR_DEFINE_BOOL(rexnative_cmdbuf_predication, true, "ReXNative",
                    "Replayed command buffers: skip predicated packets when bin select & bin mask "
                    "is 0, as the GPU does (false: run them all)");
REXCVAR_DEFINE_STRING(rexnative_cmdbuf_bin_select, "", "ReXNative",
                      "Replayed command buffers: the bin select they start with, in hex (empty: "
                      "the device's, 00000000FFFFFFFF)");
REXCVAR_DEFINE_STRING(rexnative_cmdbuf_bin_mask, "", "ReXNative",
                      "Replayed command buffers: the bin mask they start with, in hex (empty: the "
                      "device's)");
REXCVAR_DEFINE_BOOL(rexnative_cmdbuf_nested, true, "ReXNative",
                    "Replayed command buffers: follow the INDIRECT_BUFFERs inside a buffer that "
                    "runs straight from the ring (its flag 0x20); others are never followed");
REXCVAR_DEFINE_BOOL(rexnative_cmdbuf_internal_draws, false, "ReXNative",
                    "Replayed command buffers: also issue their DRAW_INDX_2 packets (the D3D's "
                    "own clear and copy quads, dropped by default)");

namespace rexnative::cmdbuf {
namespace {

constexpr uint32_t kPhysicalEnd = 0x20000000;  // 512 MB of physical memory
constexpr uint32_t kMaxNodes = 256;            // chunk list nodes per buffer (a sanity bound)
constexpr uint32_t kMaxNodeEntries = 16;       // 136-byte nodes: 16 entries of 8 bytes
constexpr uint32_t kMaxIbDwords = 0xFFFFF;     // the CP's INDIRECT_BUFFER length field
constexpr uint32_t kBudgetDwords = 1u << 21;   // per walk, nested buffers included
// First register of each constant kind in SET_CONSTANT and LOAD_ALU_CONSTANT: ALU (float),
// fetch, bool, loop, register.
constexpr uint32_t kConstantBase[] = {0x4000, 0x4800, 0x4900, 0x4908, 0x2000};
// BeginVertices and BeginIndexedVertices (Iron Brigade's sub_8264D078, sub_8264D590) write
// VGT_INDX_OFFSET alone, then fetch constant 31 as one six-register run, straight into the
// stream: vertex fetch 95 then points at the vertices they allocated, and the DRAW_INDX after it
// at the indices. The flush that restores fetch 95 (stream 0) from the shadow at the next draw
// writes the same run, but not right after VGT_INDX_OFFSET [INFERENCE: no D3D call dirties that
// register in the shadow; the draw calls write it into the stream].
constexpr uint32_t kIndxOffsetWrite = 0x00002102;  // type 0, one register: VGT_INDX_OFFSET
constexpr uint32_t kFetch31Write = 0x000548BA;     // type 0, six registers from 0x4800 + 31 * 6
constexpr uint32_t kFetch95 = 0x48BE;              // vertex fetch 95, dword 0

// Why a walk failed (pass 1: the call is rejected and has no effect).
enum class Reason : uint8_t {
  kNone,
  kBadChunkList,
  kBadEntry,
  kBudget,
  kOverrun,
  kShort,
  kBadAluSource,
  kBadShaderType,
  kBadShaderRange,
  kBinDraw,
  kUnknownOp,
  kNestedRange,
  kCount
};
constexpr const char* kReasonNames[] = {
    "ok",       "bad_chunk_list", "bad_entry",       "budget",           "overrun",
    "short",    "bad_alu_source", "bad_shader_type", "bad_shader_range", "bin_draw",
    "unknown_op", "nested_range"};
static_assert(std::size(kReasonNames) == size_t(Reason::kCount));

// Why a draw packet was not issued.
enum Drop : uint32_t {
  kDropInvalid,   // source select 3
  kDropImm,       // immediate indices: unsupported, as in both references
  kDropZero,      // no indices
  kDropCopy,      // RB_MODECONTROL kCopy: a resolve recorded in the buffer
  kDropInternal,  // DRAW_INDX_2: only the D3D's own paths (Clear, Resolve) emit it
  kDropViz,       // PA_SC_VIZ_QUERY enable + kill: a visibility survey, never drawn
  kDropNoVs,      // no vertex shader loaded
  kDropNoIndex,   // DMA indices at address 0
  kDropCount
};
constexpr const char* kDropNames[] = {"invalid",      "imm", "zero",  "copy",
                                      "d3d_internal", "viz", "no_vs", "no_index"};
static_assert(std::size(kDropNames) == kDropCount);

// Counters, per frame; added to the window at each Swap.
enum Count : uint32_t {
  kCalls,
  kBuffers,
  kIbs,
  kDwords,
  kDrawsSeen,
  kDrawsSeen2,  // of which DRAW_INDX_2
  kDrawsEmitted,
  kDropFirst,
  kPredSkipped = kDropFirst + kDropCount,
  kPredSkippedDraws,
  kPredNonDraw,             // predicated packets other than draws, run or skipped
  kPredShader,              // predicated IM_LOAD / IM_LOAD_IMMEDIATE, run or skipped
  kPredShaderRan,           // of those, run
  kPredShaderOverwritten,   // run while another of the same type ran since the last draw
  kBinPackets,
  kNestedFollowed,
  kNestedRefused,
  kRejected,
  kAsync,
  kInvalid,
  kNotEnded,
  kBadStatus,
  kOom,
  kNotMain,
  kChangedDuringDecode,
  kRegsApplied,
  kRegsIgnored,
  kKeptWrites,
  kKeptBlockWrites,
  kImLoad,
  kImLoadImm,
  kImLoadStart,
  kLoadAlu,
  kConstKindUnknown,
  kInlineRanges,
  kDecodeTicks,
  kDrawTicks,
  kRejectFirst,  // rejected calls by Reason
  kCountCount = kRejectFirst + uint32_t(Reason::kCount)
};
using Counters = std::array<uint64_t, kCountCount>;

struct Ib {
  uint32_t phys, dwords;
  uint8_t flag;  // 0x81, or 0x8F (the async kind's PFD buffers); older XDKs: any (CollectChunks)
};
struct Range {
  uint32_t begin;
  uint64_t end;
};

// Everything below runs on the Swap thread (cmd_replay.h): plain variables.
struct State {
  bool initialized = false;
  int32_t requested = 0, mode = 0;
  Counters frame{}, window{};
  uint64_t window_frames = 0, wrong_thread_seen = 0;
  int32_t log_left = 0;
  uint32_t dump_left = 0;
  std::string dump_tag, dump_dir;
  uint64_t call_index = 0;
  // The first distinct registers the window's buffers wrote that no draw reads.
  uint32_t ignored_ids[8] = {};
  uint32_t ignored_count = 0;
  std::vector<Ib> ibs;  // the current call's chunk list
  // InlineData: the Begin call data of the replayed draw OnDraw is running for.
  Range inline_data[2] = {};
  uint32_t inline_count = 0;
};
State g_state;

// A hash of the thread that last called Swap on the presenting device (0: none yet), and the
// calls refused on other threads (the only state AfterRunOtherThread touches).
std::atomic<uint64_t> g_swap_thread{0};
std::atomic<uint64_t> g_wrong_thread{0};

uint64_t ThreadHash() {
  thread_local const uint64_t hash = [] {
    const uint64_t h = std::hash<std::thread::id>{}(std::this_thread::get_id());
    return h ? h : 1;
  }();
  return hash;
}

__attribute__((format(printf, 2, 3))) void Append(std::string& s, const char* f, ...) {
  char buf[1024];
  va_list args;
  va_start(args, f);
  const int n = std::vsnprintf(buf, sizeof(buf), f, args);
  va_end(args);
  if (n > 0) s.append(buf, std::min<size_t>(size_t(n), sizeof(buf) - 1));
}

const char* OpName(uint32_t op) {
  switch (op) {
    case 0x10: return "NOP";
    case 0x21: return "REG_RMW";
    case 0x22: return "DRAW_INDX";
    case 0x23: return "VIZ_QUERY";
    case 0x26: return "WAIT_FOR_IDLE";
    case 0x27: return "IM_LOAD";
    case 0x2B: return "IM_LOAD_IMMEDIATE";
    case 0x2D: return "SET_CONSTANT";
    case 0x2F: return "LOAD_ALU_CONSTANT";
    case 0x34: return "DRAW_INDX_BIN";
    case 0x35: return "DRAW_INDX_2_BIN";
    case 0x36: return "DRAW_INDX_2";
    case 0x37: return "INDIRECT_BUFFER_PFD";
    case 0x3B: return "INVALIDATE_STATE";
    case 0x3C: return "WAIT_REG_MEM";
    case 0x3D: return "MEM_WRITE";
    case 0x3E: return "REG_TO_MEM";
    case 0x3F: return "INDIRECT_BUFFER";
    case 0x45: return "COND_WRITE";
    case 0x46: return "EVENT_WRITE";
    case 0x48: return "ME_INIT";
    case 0x50: return "SET_BIN_MASK";
    case 0x51: return "SET_BIN_SELECT";
    case 0x54: return "INTERRUPT";
    case 0x55: return "SET_CONSTANT2";
    case 0x56: return "SET_SHADER_CONSTANTS";
    case 0x58: return "EVENT_WRITE_SHD";
    case 0x5A: return "EVENT_WRITE_EXT";
    case 0x5B: return "EVENT_WRITE_ZPD";
    case 0x5D: return "WAIT_IB_PFD_COMPLETE";
    case 0x5E: return "CONTEXT_UPDATE";
    case 0x60: return "SET_BIN_MASK_LO";
    case 0x61: return "SET_BIN_MASK_HI";
    case 0x62: return "SET_BIN_SELECT_LO";
    case 0x63: return "SET_BIN_SELECT_HI";
    case 0x64: return "XE_SWAP";
    default: return "?";
  }
}

// Big-endian dword i at p.
inline uint32_t Be32(const uint8_t* p, uint32_t i) {
  uint32_t v;
  std::memcpy(&v, p + size_t(i) * 4, 4);
  return __builtin_bswap32(v);
}

void Init(State& s) {
  if (s.initialized) return;
  s.initialized = true;
  const int32_t cvar = REXCVAR_GET(rexnative_cmdbuf);
  const int32_t mode = cvar >= 0 ? cvar : rexnative::game::kDefaultCmdBuf;
  s.requested = s.mode = std::clamp<int32_t>(mode, 0, 2);
  s.log_left = REXCVAR_GET(rexnative_cmdbuf_log);
  if (s.mode) REXLOG_INFO("ReXNative cmdbuf: mode {} (1 count, 2 replay)", s.mode);
}

// A hex override cvar; empty keeps `seed`.
uint64_t BinOverride(const std::string& text, uint64_t seed) {
  if (text.empty()) return seed;
  char* end = nullptr;
  errno = 0;
  const unsigned long long v = std::strtoull(text.c_str(), &end, 16);
  // strtoull takes a sign (negating the value) and saturates past 64 bits without stopping:
  // neither is a mask anyone meant.
  if (end == text.c_str() || *end || errno == ERANGE ||
      text.find_first_of("+-") != std::string::npos) {
    REXNATIVE_LOG_FIRST(2, WARN,
                        "ReXNative cmdbuf: bin override '{}' is not a 64-bit hex value; using "
                        "{:016X}",
                        text, seed);
    return seed;
  }
  return v;
}

// The chunk list at +116, in the guest's own order (sub_8264EF50 walks node by node, entry by
// entry). Pointers are physical memory's CPU views, which the CPU reads directly: 0xC0000000+
// in Iron Brigade (16 MB pages), 0xA0000000+ in Midnight Club: Los Angeles (64 KB pages, whose
// 67 buffers a frame hold its cars and were all refused), 0xE0000000+ for 4 KB pages.
Reason CollectChunks(const uint8_t* base, const CommandBufferHeader& h, std::vector<Ib>& ibs,
                     uint32_t* total, uint32_t* where) {
  uint64_t sum = 0;
  uint32_t nodes = 0;
  for (uint32_t node = h.chunks; node; node = Load32(base, node)) {
    *where = node;
    if (node < 0xA0000000u || (node & 3) || ++nodes > kMaxNodes)
      return Reason::kBadChunkList;
    const uint32_t count = Load32(base, node + 4);
    if (count < 1 || count > kMaxNodeEntries) return Reason::kBadChunkList;
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t d0 = Load32(base, node + 8 + 8 * i), addr = Load32(base, node + 12 + 8 * i);
      const uint32_t flag = d0 >> 24, n = d0 & 0xFFFFFF;
      // The high byte is 0x81, or 0x8F for the async kind's PFD buffers (Iron Brigade's XDK
      // 20xxx); XDK 6995's D3D writes the dword count alone and leaves the byte as the heap
      // left it (Midnight Club: Los Angeles: 0xCD, its allocator's fill). The walk checks the
      // buffer's packets before anything is applied.
      if (n > kMaxIbDwords) return Reason::kBadEntry;
      const uint32_t phys = GpuAddress(addr);  // plus 4 KiB in the 0xE0000000 view
      if (n && ((phys & 3) || uint64_t(phys) + 4ull * n > kPhysicalEnd)) return Reason::kBadEntry;
      ibs.push_back({phys, n, uint8_t(flag)});
      sum += n;
    }
  }
  if (sum > kBudgetDwords) return Reason::kBudget;
  *total = uint32_t(sum);
  return Reason::kNone;
}

// One pass over a call's buffers. Pass 1 (kApply false) checks everything pass 2 relies on and
// counts; pass 2 (kApply true) changes the tracker and issues the draws. Each pass starts from
// a fresh Walker: its bin state, budget and hash memo cannot differ between the passes, so pass
// 2 fails only where the guest changed the memory in between.
struct Walker {
  Walker(const uint8_t* base_, const uint8_t* pm_, const RunContext& c_, State& s_)
      : base(base_), pm(pm_), c(c_), s(s_), f(s_.frame), gs(GuestState()) {}

  const uint8_t* base;
  const uint8_t* pm;  // physical membase: the buffers, shaders and constants are physical
  const RunContext& c;
  State& s;
  Counters& f;
  GuestStateTracker& gs;
  // Options.
  bool nested_ok = false, predication = true, internal_draws = false, time_draws = false;
  uint64_t mask = 0, select = 0;  // bin state
  uint32_t budget = 0;            // dwords nested buffers may still add
  bool emit = true;               // pass 2: false in the state-only tail after a failure
  // Pass 2: vertex fetch 95's range while a Begin call in the buffer set it (kFetch31Write; end
  // 0: fetch 95 holds something else). The recording's allocator hands that data out apart from
  // its PM4 segments (sub_8263F400) and the game records again every pass: copied for each draw
  // like inline data, never watched.
  Range up = {};
  std::string* dump = nullptr;    // pass 1: one line per packet
  std::string* rd = nullptr;      // pass 2: one line per draw
  uint32_t* mirror = nullptr;     // pass 1 with a dump: the registers as the buffer sets them
  // Results.
  Reason reason = Reason::kNone;
  uint32_t fail_ib = 0, fail_offset = 0, fail_op = 0;
  uint32_t ops[128] = {};
  uint32_t draws_seen = 0, draws_emitted = 0, nested_followed = 0, nested_refused = 0;
  uint32_t bin_packets = 0;
  uint64_t draw_ticks = 0;
  bool pred_shader_ran[2] = {};  // vs, ps: a predicated load ran since the last draw
  struct HashSlot {
    uint32_t phys = 0, dwords = 0;  // dwords 0: empty
    uint64_t hash = 0;
  };
  HashSlot hashes[256];

  bool Fail(Reason r, uint32_t ib, uint32_t offset, uint32_t op) {
    reason = r;
    fail_ib = ib;
    fail_offset = offset;
    fail_op = op;
    return false;
  }

  // XXH3 of a shader's ucode: its ShaderRef binding (d3d_state.h kReplayedShaderSeq).
  uint64_t Hash(uint32_t phys, uint32_t dwords) {
    HashSlot& slot = hashes[((phys >> 2) ^ (dwords * 0x9E3779B1u)) & 0xFF];
    if (slot.dwords != dwords || slot.phys != phys) {
      slot = {phys, dwords, XXH3_64bits(pm + phys, size_t(dwords) * 4)};
    }
    return slot.hash;
  }

  void NoteIgnored(uint32_t reg) {
    ++f[kRegsIgnored];
    for (uint32_t i = 0; i < s.ignored_count; ++i)
      if (s.ignored_ids[i] == reg) return;
    if (s.ignored_count < std::size(s.ignored_ids)) s.ignored_ids[s.ignored_count++] = reg;
  }

  void Apply(uint32_t reg, uint32_t v) {
    switch (gs.ReplayPut(reg, v)) {
      case GuestStateTracker::ReplayWrite::kIgnored:
        NoteIgnored(reg);  // not mirrored either: the tracker dropped it
        return;
      case GuestStateTracker::ReplayWrite::kKept:
        ++f[kKeptWrites];
        if (reg < 0x4000) {
          // Iron Brigade keeps only float constants; a kept register block would change
          // render state for the game's own draws after the buffer.
          ++f[kKeptBlockWrites];
          REXNATIVE_WARN_ONCE("ReXNative cmdbuf: register {:04X} written into a kept group", reg);
        }
        [[fallthrough]];
      case GuestStateTracker::ReplayWrite::kApplied:
        ++f[kRegsApplied];
        if (c.tap_write) c.tap_write(reg, v);
        return;
    }
  }

  // Predicated tiling recorded in the buffer: one setup per tile, each under a SET_BIN_MASK of
  // that tile's bins, moving the window (PA_SC_WINDOW_OFFSET) and its scissor onto the tile.
  // The GPU runs the buffer once per tile with one bin selected; the replay draws once over
  // the whole surface, so those writes become offset 0 and the scissor of all the tiles
  // together. Midnight Club: Los Angeles's cars: tile 2's offset of -512 rows, the last write,
  // drew them 512 rows up.
  bool tile_write = false;  // the current packet is predicated on some of the selected bins
  uint32_t tile_br = 0;     // the tiles' scissor bottom-right so far (x | y << 16)
  uint32_t TileValue(uint32_t reg, uint32_t v) {
    switch (reg) {
      case 0x2080:  // PA_SC_WINDOW_OFFSET
        return 0;
      case 0x2081:  // PA_SC_WINDOW_SCISSOR_TL: window_offset_disable kept, the corner at 0, 0
        return v & 0x80000000u;
      case 0x2082: {  // PA_SC_WINDOW_SCISSOR_BR: the largest corner so far
        const uint32_t x = std::max(v & 0x7FFF, tile_br & 0x7FFF);
        const uint32_t y = std::max((v >> 16) & 0x7FFF, (tile_br >> 16) & 0x7FFF);
        return tile_br = x | y << 16;
      }
      default:
        return v;
    }
  }

  template <bool kApply>
  void Write(uint32_t reg, uint32_t v) {
    if (tile_write && reg >= 0x2080 && reg <= 0x2082) v = TileValue(reg, v);
    if constexpr (kApply) {
      if (reg == kFetch95) up = {};  // Execute sets it again after a Begin call's own run
      Apply(reg, v);
    } else if (mirror && reg < kGuestRegisterCount) {
      mirror[reg] = v;
    }
  }

  // Pass 2 failed where pass 1 passed: the memory changed in between. The draws already queued
  // stay; no more are issued, but the walk goes on setting state (A13), so the groups the buffer
  // keeps end as the GPU has them.
  void Changed() {
    if (emit) {
      ++f[kChangedDuringDecode];
      REXNATIVE_LOG_FIRST(4, WARN,
                          "ReXNative cmdbuf: buffer changed during the replay: {} at {:08X}+{} "
                          "(op {:02X}); state only from here",
                          kReasonNames[size_t(reason)], fail_ib, fail_offset, fail_op);
    }
    emit = false;
  }

  // A run of register values in the dump, as the tap writes them.
  void DumpRun(uint32_t reg, const uint8_t* p, uint32_t count, bool one) {
    if (!count) return;
    if (!one && reg >= 0x4000 && reg < 0x4800) {
      Append(*dump, "K %04X n=%u h=%016llX\n", reg, count,
             (unsigned long long)XXH3_64bits(p, size_t(count) * 4));
      return;
    }
    Append(*dump, "W %04X%s", reg, one ? " one" : "");
    for (uint32_t k = 0; k < count && k < 16; ++k) Append(*dump, " %08X", Be32(p, k));
    Append(*dump, "%s\n", count > 16 ? " ..." : "");
  }

  template <bool kApply>
  bool Walk(const std::vector<Ib>& ibs) {
    bool ok = true;
    for (const Ib& ib : ibs) {
      if (!ib.dwords) continue;
      if (Execute<kApply>(ib.phys, ib.dwords, 0)) continue;
      if constexpr (!kApply) {
        return false;
      } else {
        // The reference drops the rest of the failing buffer only: the next ones still run.
        Changed();
        ok = false;
      }
    }
    return ok;
  }

  template <bool kApply>
  bool Execute(uint32_t phys, uint32_t n, uint32_t depth);
  template <bool kApply>
  bool Draw(uint32_t op, bool pred, const uint8_t* p, uint32_t d, uint32_t cnt, uint32_t phys,
            uint32_t at);
  void LogDraw(uint32_t init, int drop, bool issued);
};

template <bool kApply>
bool Walker::Execute(uint32_t phys, uint32_t n, uint32_t depth) {
  const uint8_t* const p = pm + phys;
  uint32_t i = 0;
  uint32_t indx_offset_end = ~0u;  // pass 2: the dword after the last lone VGT_INDX_OFFSET write
  while (i < n) {
    const uint32_t hdr = Be32(p, i);
    // Zero dwords are MakeSpace's padding (the D3D never writes register 0); type 2 is a NOP.
    if (!hdr || (hdr >> 30) == 2) {
      ++i;
      continue;
    }
    if ((hdr >> 30) == 1) {  // two registers below 0x800: none is draw state
      if (i + 3 > n) return Fail(Reason::kOverrun, phys, i, 0);
      if constexpr (kApply) {
        NoteIgnored(hdr & 0x7FF);
        NoteIgnored((hdr >> 11) & 0x7FF);
      }
      i += 3;
      continue;
    }
    const uint32_t cnt = ((hdr >> 16) & 0x3FFF) + 1;
    const uint32_t d = i + 1;  // the packet's first data dword
    if (d + cnt > n)
      return Fail(Reason::kOverrun, phys, i, (hdr >> 30) == 3 ? (hdr >> 8) & 0x7F : 0);
    if ((hdr >> 30) == 0) {  // `cnt` registers from base, or all into base (bit 15)
      tile_write = false;  // type 0 has no predicate: every tile runs it
      const uint32_t reg = hdr & 0x7FFF;
      const bool one = (hdr & 0x8000) != 0;
      for (uint32_t k = 0; k < cnt; ++k) Write<kApply>(one ? reg : reg + k, Be32(p, d + k));
      if constexpr (kApply) {
        if (hdr == kFetch31Write && i == indx_offset_end) {
          // A Begin call's fetch 95: address in dword 4, size in dwords in dword 5's bits 2-25.
          const uint32_t start = Be32(p, d + 4) & ~3u;
          up = {start, uint64_t(start) + 4ull * ((Be32(p, d + 5) >> 2) & 0xFFFFFF)};
        }
        if (hdr == kIndxOffsetWrite) indx_offset_end = d + cnt;
      }
      if (dump) DumpRun(reg, p + size_t(d) * 4, cnt, one);
      i = d + cnt;
      continue;
    }
    const uint32_t op = (hdr >> 8) & 0x7F;
    const bool pred = (hdr & 1) != 0;
    tile_write = pred && (select & mask) != select;
    ++ops[op];
    if (pred) {
      // The CP's rule, for every opcode: a predicated packet runs only if select & mask != 0;
      // a predicated XE_SWAP never.
      const bool draw = op == 0x22 || op == 0x36;
      const bool skip = op == 0x64 || (predication && (select & mask) == 0);
      if constexpr (!kApply) {
        if (!draw) ++f[kPredNonDraw];
        if (op == 0x27 || op == 0x2B) ++f[kPredShader];
        if (skip) {
          ++f[kPredSkipped];
          if (draw) ++f[kPredSkippedDraws];
        }
      }
      if (skip) {
        if (dump) Append(*dump, "X pred-skipped %s\n", OpName(op));
        i = d + cnt;
        continue;
      }
    }
    const uint32_t dphys = phys + 4 * d;  // physical address of the first data dword
    switch (op) {
      case 0x2D: {  // SET_CONSTANT: index | kind << 16, then the values
        const uint32_t d0 = Be32(p, d), index = d0 & 0x7FF, kind = (d0 >> 16) & 0xFF;
        if (kind >= std::size(kConstantBase)) {
          if constexpr (!kApply) ++f[kConstKindUnknown];
          break;
        }
        const uint32_t reg = kConstantBase[kind] + index;
        for (uint32_t k = 1; k < cnt; ++k) Write<kApply>(reg + k - 1, Be32(p, d + k));
        if (dump) DumpRun(reg, p + size_t(d + 1) * 4, cnt - 1, false);
        break;
      }
      case 0x55:    // SET_CONSTANT2
      case 0x56: {  // SET_SHADER_CONSTANTS: an absolute register, then the values
        const uint32_t reg = Be32(p, d) & 0xFFFF;
        for (uint32_t k = 1; k < cnt; ++k) Write<kApply>(reg + k - 1, Be32(p, d + k));
        if (dump) DumpRun(reg, p + size_t(d + 1) * 4, cnt - 1, false);
        break;
      }
      case 0x2F: {  // LOAD_ALU_CONSTANT: source, index | kind << 16, dwords; read now
        if (cnt < 3) return Fail(Reason::kShort, phys, i, op);
        const uint32_t src = Be32(p, d) & 0x1FFFFFFF, d1 = Be32(p, d + 1);
        const uint32_t index = d1 & 0x7FF, kind = (d1 >> 16) & 0xFF, m = Be32(p, d + 2) & 0xFFF;
        if (kind >= std::size(kConstantBase)) {
          if constexpr (!kApply) ++f[kConstKindUnknown];
          break;
        }
        if (uint64_t(src) + 4ull * m > kPhysicalEnd)
          return Fail(Reason::kBadAluSource, phys, i, op);
        const uint32_t reg = kConstantBase[kind] + index;
        for (uint32_t k = 0; k < m; ++k) Write<kApply>(reg + k, Be32(pm + src, k));
        if constexpr (kApply) ++f[kLoadAlu];
        if (dump) Append(*dump, "L %04X n=%u from=%08X\n", reg, m, src);
        break;
      }
      case 0x27:    // IM_LOAD: address | type, start << 16 | dwords
      case 0x2B: {  // IM_LOAD_IMMEDIATE: type, start << 16 | dwords, then the ucode
        if (cnt < 2) return Fail(Reason::kShort, phys, i, op);
        const bool immediate = op == 0x2B;
        const uint32_t d0 = Be32(p, d), d1 = Be32(p, d + 1);
        // The whole first dword is the immediate form's type; the low 2 bits IM_LOAD's.
        const uint32_t type = immediate ? d0 : d0 & 3;
        if (type > 1) return Fail(Reason::kBadShaderType, phys, i, op);  // the CP aborts too
        const uint32_t dwords = d1 & 0xFFFF;
        const uint32_t ucode = immediate ? dphys + 8 : (d0 & ~3u) & 0x1FFFFFFF;
        if (!dwords || (immediate ? dwords > cnt - 2
                                  : uint64_t(ucode) + 4ull * dwords > kPhysicalEnd))
          return Fail(Reason::kBadShaderRange, phys, i, op);
        if constexpr (!kApply) {
          if (d1 >> 16) ++f[kImLoadStart];  // a start offset: asserted 0 by the CP, ignored
          if (pred) {
            // Variant pairs: predicated loads of one type between SET_BIN_MASK changes. When
            // two run before a draw, the last one wins; the counters show whether it happens.
            ++f[kPredShaderRan];
            if (pred_shader_ran[type]) {
              ++f[kPredShaderOverwritten];
              REXNATIVE_LOG_FIRST(1, WARN,
                                  "ReXNative cmdbuf: two predicated {} loads ran before a draw "
                                  "(bin mask {:016X} select {:016X}): the last one wins",
                                  type ? "pixel shader" : "vertex shader", mask, select);
            }
            pred_shader_ran[type] = true;
          }
        }
        if constexpr (kApply) {
          // LoadShader reads the ucode in Prepare (or Draw), inside this hook: an immediate
          // load's ucode is read where the buffer holds it.
          gs.ReplayShader(type == 1, {0, ucode, dwords, nullptr, kReplayedShaderSeq,
                                      Hash(ucode, dwords)});
          ++f[immediate ? kImLoadImm : kImLoad];
        }
        if (dump)
          Append(*dump, "S %s %08X %u%s h=%016llX\n", type ? "ps" : "vs", ucode, dwords,
                 immediate ? " imm" : "", (unsigned long long)Hash(ucode, dwords));
        break;
      }
      case 0x22:  // DRAW_INDX
      case 0x36:  // DRAW_INDX_2
        if (!Draw<kApply>(op, pred, p, d, cnt, phys, i)) return false;
        break;
      case 0x34:  // DRAW_INDX_BIN, DRAW_INDX_2_BIN: neither reference implements them
      case 0x35:
        return Fail(Reason::kBinDraw, phys, i, op);
      case 0x60:  // SET_BIN_MASK_LO .. SET_BIN_SELECT_HI: one 32-bit half each
      case 0x61:
      case 0x62:
      case 0x63: {
        const uint64_t v = Be32(p, d);
        uint64_t& bin = op < 0x62 ? mask : select;
        bin = (op & 1) ? (bin & 0xFFFFFFFFull) | (v << 32) : (bin & ~0xFFFFFFFFull) | v;
        if constexpr (!kApply) ++f[kBinPackets];
        ++bin_packets;
        if (dump)
          Append(*dump, "B %s %016llX\n", op < 0x62 ? "mask" : "select", (unsigned long long)bin);
        break;
      }
      case 0x50:    // SET_BIN_MASK
      case 0x51: {  // SET_BIN_SELECT: high half, then low
        if (cnt < 2) return Fail(Reason::kShort, phys, i, op);
        (op == 0x50 ? mask : select) = (uint64_t(Be32(p, d)) << 32) | Be32(p, d + 1);
        if constexpr (!kApply) ++f[kBinPackets];
        ++bin_packets;
        if (dump)
          Append(*dump, "B %s %016llX\n", op == 0x50 ? "mask" : "select",
                 (unsigned long long)(op == 0x50 ? mask : select));
        break;
      }
      case 0x3F:    // INDIRECT_BUFFER
      case 0x37: {  // INDIRECT_BUFFER_PFD: address, dwords
        if (cnt < 2) return Fail(Reason::kShort, phys, i, op);
        const uint32_t ib = Be32(p, d) & 0x1FFFFFFF, ibn = Be32(p, d + 1) & 0xFFFFF;
        if (!ibn) break;
        // Only from a buffer that runs from the ring (flag 0x20) is a buffer inside it a
        // second-level IB. Pushed through the push segment, the chunks are second-level already
        // and one inside would be a third level [INFERENCE: Xenos's CP has two]; the main
        // stream (the tap) never follows those either.
        if (depth == 0 && nested_ok) {
          if ((ib & 3) || uint64_t(ib) + 4ull * ibn > kPhysicalEnd)
            return Fail(Reason::kNestedRange, phys, i, op);
          if (ibn > budget) return Fail(Reason::kBudget, phys, i, op);
          budget -= ibn;
          if constexpr (!kApply) ++f[kNestedFollowed];
          ++nested_followed;
          if (dump) Append(*dump, "I %08X %u d%u followed\n", ib, ibn, depth);
          if (!Execute<kApply>(ib, ibn, depth + 1)) {
            if constexpr (!kApply) {
              return false;
            } else {
              // The SDK's command processor drops the rest of the nested buffer only and goes
              // on with this one: its later writes still reach the GPU.
              Changed();
            }
          }
        } else {
          if constexpr (!kApply) {
            ++f[kNestedRefused];
            REXNATIVE_LOG_FIRST(1, INFO,
                                "ReXNative cmdbuf: nested INDIRECT_BUFFER {:08X} ({} dwords) "
                                "not followed (depth {}{})",
                                ib, ibn, depth,
                                nested_ok ? "" : ", the buffer does not run from the ring");
          }
          ++nested_refused;
          if (dump) Append(*dump, "I %08X %u d%u refused\n", ib, ibn, depth);
        }
        break;
      }
      case 0x64:  // XE_SWAP, not predicated
        if constexpr (!kApply)
          REXNATIVE_WARN_ONCE("ReXNative cmdbuf: XE_SWAP inside a recorded buffer: skipped");
        [[fallthrough]];
      // No draw state; the SDK's command processor carries out their memory effects (fences,
      // events, writes, waits) in these same buffers under --gpu_skip_draws.
      case 0x10:  // NOP
      case 0x3B:  // INVALIDATE_STATE
      case 0x26:  // WAIT_FOR_IDLE
      case 0x5E:  // CONTEXT_UPDATE
      case 0x48:  // ME_INIT
      case 0x5D:  // WAIT_IB_PFD_COMPLETE
      case 0x23:  // VIZ_QUERY
      case 0x46:  // EVENT_WRITE
      case 0x58:  // EVENT_WRITE_SHD
      case 0x5A:  // EVENT_WRITE_EXT
      case 0x5B:  // EVENT_WRITE_ZPD
      case 0x3D:  // MEM_WRITE
      case 0x3E:  // REG_TO_MEM
      case 0x3C:  // WAIT_REG_MEM
      case 0x45:  // COND_WRITE
      case 0x54:  // INTERRUPT
      case 0x21:  // REG_RMW: its target is & 0x1FFF, never render state
        if (dump) {
          Append(*dump, "P %s pred=%d n=%u", OpName(op), pred ? 1 : 0, cnt);
          for (uint32_t k = 0; k < cnt && k < 4; ++k) Append(*dump, " %08X", Be32(p, d + k));
          Append(*dump, "\n");
        }
        break;
      default:
        // The reference drops the rest of the buffer at an unknown opcode; all or nothing here
        // until the histogram shows one worth knowing.
        return Fail(Reason::kUnknownOp, phys, i, op);
    }
    i = d + cnt;
  }
  return true;
}

template <bool kApply>
bool Walker::Draw(uint32_t op, [[maybe_unused]] bool pred, const uint8_t* p, uint32_t d,
                  uint32_t cnt, uint32_t phys, uint32_t at) {
  // DRAW_INDX: viz token (ignored, as in the SDK), VGT_DRAW_INITIATOR, then VGT_DMA_BASE and
  // VGT_DMA_SIZE for DMA indices. DRAW_INDX_2 has no viz token. The three registers are not
  // written: no draw reads them, the draw carries them (ReadDrawState).
  const bool indx = op == 0x22;
  const uint32_t first = indx ? 1 : 0;
  if (cnt < first + 1) return Fail(Reason::kShort, phys, at, op);
  const uint32_t init = Be32(p, d + first);
  const uint32_t src = (init >> 6) & 3;
  if (src == 0 && cnt < first + 3) return Fail(Reason::kShort, phys, at, op);
  const uint32_t dma_base = src == 0 ? Be32(p, d + first + 1) : 0;
  const uint32_t dma_size = src == 0 ? Be32(p, d + first + 2) : 0;
  pred_shader_ran[0] = pred_shader_ran[1] = false;
  ++draws_seen;
  int drop = src == 3             ? int(kDropInvalid)
             : src == 1           ? int(kDropImm)
             : (init >> 16) == 0  ? int(kDropZero)
                                  : -1;
  if constexpr (!kApply) {
    ++f[kDrawsSeen];
    if (!indx) ++f[kDrawsSeen2];
    if (drop >= 0) ++f[kDropFirst + uint32_t(drop)];
    if (dump) {
      Append(*dump, "D %s pred=%d prim=%u src=%s idx32=%u count=%u", OpName(op), pred ? 1 : 0,
             init & 0x3F, src == 0 ? "dma" : src == 2 ? "auto" : src == 1 ? "imm" : "invalid",
             (init >> 11) & 1, init >> 16);
      if (src == 0) Append(*dump, " ib=%08X:%u", dma_base, dma_size & 0xFFFFFF);
      // The D3D's own quads (Clear's, Resolve's): their mode and surface tell which.
      if (!indx && mirror) Append(*dump, " mode=%08X surf=%08X", mirror[0x2208], mirror[0x2000]);
      Append(*dump, "\n");
    }
    return true;
  } else {
    if (drop < 0) {
      const uint32_t* regs = gs.regs();
      if ((regs[0x2208] & 7) == 6)
        drop = kDropCopy;
      else if (!indx && !internal_draws)
        drop = kDropInternal;
      else if ((regs[0x2293] & 0x81) == 0x81)
        drop = kDropViz;
      else if (!gs.vs().ucode)
        drop = kDropNoVs;
      else if (src == 0 && !(dma_base & 0x1FFFFFFF))
        drop = kDropNoIndex;
      if (drop >= 0) ++f[kDropFirst + uint32_t(drop)];
    }
    const bool issue = drop < 0 && emit;
    if (issue) {
      HookArgs a{};
      a.fn = HookFn::kRunCommandBuffer;
      a.device = c.device;
      a.r[0] = c.device;
      a.r[1] = init;
      a.r[2] = dma_base;
      a.r[3] = dma_size;
      a.tiling = TilingCall::kNone;
      if (up.end) {
        // A Begin call's draw: its vertices, and with DMA indices (BeginIndexedVertices) the
        // indices the same call allocated, as ReadDrawState will read them.
        s.inline_data[s.inline_count++] = up;
        if (src == 0) {
          const uint32_t size = ((init >> 11) & 1) ? 4u : 2u;
          const uint32_t ib = (dma_base & 0x1FFFFFFFu) & ~(size - 1);
          const uint32_t count = std::min(init >> 16, dma_size & 0xFFFFFFu);
          s.inline_data[s.inline_count++] = {ib, uint64_t(ib) + uint64_t(count) * size};
        }
      }
      const uint64_t t0 = time_draws ? Ticks() : 0;
      c.renderer->OnDraw(base, a);
      if (time_draws) draw_ticks += Ticks() - t0;
      s.inline_count = 0;
      ++f[kDrawsEmitted];
      ++draws_emitted;
    }
    if (rd) LogDraw(init, drop, issue);
    return true;
  }
}

void Walker::LogDraw(uint32_t init, int drop, bool issued) {
  const uint32_t* r = gs.regs();
  const ShaderRef& vs = gs.vs();
  const ShaderRef& ps = gs.ps();
  Append(*rd, "RD %u %s%s prim=%u count=%u", draws_seen - 1,
         issued ? "emitted" : drop >= 0 ? "drop=" : "not emitted (state only)",
         drop >= 0 ? kDropNames[drop] : "", init & 0x3F, init >> 16);
  Append(*rd, " vs=%08X:%u:%016llX ps=%08X:%u:%016llX", vs.ucode, vs.dwords,
         (unsigned long long)vs.binding, ps.ucode, ps.dwords, (unsigned long long)ps.binding);
  Append(*rd,
         " surf=%08X c=%08X,%08X,%08X,%08X d=%08X mask=%08X depth=%08X mode=%08X sc=%08X,%08X "
         "offs=%08X\n",
         r[0x2000], r[0x2001], r[0x2003], r[0x2004], r[0x2005], r[0x2002], r[0x2104], r[0x2200],
         r[0x2208], r[0x2081], r[0x2082], r[0x2102]);
}

// The detail line's first half: the object, its chunks and the bin state the walk starts with.
std::string Header(const uint8_t* pm, const RunContext& c, const CommandBufferHeader& h,
                   uint64_t call, const std::vector<Ib>& ibs, uint64_t mask, uint64_t select) {
  std::string s;
  Append(s,
         "call %llu thread %016llX device %08X cb %08X status %u flags %08X inherit %016llX "
         "%016llX %016llX %016llX %016llX keep %016llX %016llX %016llX %016llX %016llX bin "
         "mask %016llX select %016llX chunks [",
         (unsigned long long)call, (unsigned long long)ThreadHash(), c.device, c.cb, h.flags & 0xF,
         h.flags, (unsigned long long)h.inherit[0], (unsigned long long)h.inherit[1],
         (unsigned long long)h.inherit[2], (unsigned long long)h.inherit[3],
         (unsigned long long)h.inherit[4], (unsigned long long)h.keep[0],
         (unsigned long long)h.keep[1], (unsigned long long)h.keep[2],
         (unsigned long long)h.keep[3], (unsigned long long)h.keep[4],
         (unsigned long long)mask, (unsigned long long)select);
  // The hashes tell whether a buffer is patched in place between two runs.
  constexpr size_t kShown = 16;
  for (size_t i = 0; i < ibs.size() && i < kShown; ++i)
    Append(s, "%s%08X:%u:%02X %016llX", i ? " " : "", ibs[i].phys, ibs[i].dwords, ibs[i].flag,
           (unsigned long long)XXH3_64bits(pm + ibs[i].phys, size_t(ibs[i].dwords) * 4));
  if (ibs.size() > kShown) Append(s, " +%zu", ibs.size() - kShown);
  Append(s, "]");
  return s;
}

void LogWindow(State& s, uint64_t wrong_thread) {
  const Counters& w = s.window;
  const double n = double(s.window_frames);
  auto per = [&](Count k) { return double(w[k]) / n; };
  std::string line;
  Append(line,
         "ReXNative cmdbuf: mode %d | per frame: calls %.1f, buffers %.1f, ibs %.1f, dwords %.0f, "
         "draws emitted/seen %.1f/%.1f (DRAW_INDX_2 %.1f), dropped",
         s.mode, per(kCalls), per(kBuffers), per(kIbs), per(kDwords), per(kDrawsEmitted),
         per(kDrawsSeen), per(kDrawsSeen2));
  for (uint32_t k = 0; k < kDropCount; ++k)
    Append(line, "%s %s %.1f", k ? "," : "", kDropNames[k], double(w[kDropFirst + k]) / n);
  Append(line,
         ", pred skipped %.1f (draws %.1f), pred other %.1f, pred shader %.1f (ran %.1f, "
         "overwritten %.1f), bin packets %.1f, nested followed/refused %.1f/%.1f, inline ranges "
         "%.1f, regs applied %.0f, kept %.0f, shaders im_load/imm %.1f/%.1f, load_alu %.1f",
         per(kPredSkipped), per(kPredSkippedDraws), per(kPredNonDraw), per(kPredShader),
         per(kPredShaderRan), per(kPredShaderOverwritten), per(kBinPackets), per(kNestedFollowed),
         per(kNestedRefused), per(kInlineRanges), per(kRegsApplied), per(kKeptWrites),
         per(kImLoad), per(kImLoadImm), per(kLoadAlu));
  Append(line,
         " | in %llu frames: rejected %llu, async %llu, invalid %llu, not ended %llu, bad status "
         "%llu, oom %llu, not main %llu, wrong thread %llu, changed during decode %llu, kept "
         "block writes %llu, const kind unknown %llu, im_load start %llu, regs ignored %llu",
         (unsigned long long)s.window_frames, (unsigned long long)w[kRejected],
         (unsigned long long)w[kAsync], (unsigned long long)w[kInvalid],
         (unsigned long long)w[kNotEnded], (unsigned long long)w[kBadStatus],
         (unsigned long long)w[kOom], (unsigned long long)w[kNotMain],
         (unsigned long long)wrong_thread, (unsigned long long)w[kChangedDuringDecode],
         (unsigned long long)w[kKeptBlockWrites], (unsigned long long)w[kConstKindUnknown],
         (unsigned long long)w[kImLoadStart], (unsigned long long)w[kRegsIgnored]);
  for (uint32_t r = 1; r < uint32_t(Reason::kCount); ++r)
    if (w[kRejectFirst + r])
      Append(line, ", rejected %s %llu", kReasonNames[r], (unsigned long long)w[kRejectFirst + r]);
  if (s.ignored_count) {
    Append(line, " (");
    for (uint32_t i = 0; i < s.ignored_count; ++i)
      Append(line, "%s%04X", i ? " " : "", s.ignored_ids[i]);
    Append(line, ")");
  }
  // Draw time is measured per draw only with --rexnative_profile (else it is in decode).
  Append(line, " | decode %.0f us, draw %.0f us per frame",
         double(TicksToNs(w[kDecodeTicks])) / 1e3 / n, double(TicksToNs(w[kDrawTicks])) / 1e3 / n);
  REXLOG_INFO("{}", line);
}

}  // namespace

void OnSwap(int32_t stats_every) {
  const uint64_t self = ThreadHash();
  const uint64_t before = g_swap_thread.exchange(self, std::memory_order_relaxed);
  if (before != self)
    REXNATIVE_LOG_FIRST(4, INFO, "ReXNative cmdbuf: Swap thread {:016X} (was {:016X})", self,
                        before);
  State& s = g_state;
  Init(s);
  if (s.requested != s.mode) {
    REXLOG_INFO("ReXNative cmdbuf: mode {} (was {})", s.requested, s.mode);
    s.mode = s.requested;
  }
  for (uint32_t k = 0; k < kCountCount; ++k) s.window[k] += s.frame[k];
  s.frame = {};
  ++s.window_frames;
  if (stats_every <= 0 || s.window_frames < uint64_t(stats_every)) return;
  const uint64_t wrong = g_wrong_thread.load(std::memory_order_relaxed);
  if (s.window[kCalls] || wrong != s.wrong_thread_seen) LogWindow(s, wrong - s.wrong_thread_seen);
  s.wrong_thread_seen = wrong;
  s.window = {};
  s.window_frames = 0;
  s.ignored_count = 0;
}

bool OnSwapThread() { return g_swap_thread.load(std::memory_order_relaxed) == ThreadHash(); }

void AfterRunOtherThread(const uint8_t* base, uint32_t device, uint32_t cb) {
  g_wrong_thread.fetch_add(1, std::memory_order_relaxed);
  REXNATIVE_LOG_FIRST(4, WARN,
                      "ReXNative cmdbuf: RunCommandBuffer {:08X} on device {:08X} from a thread "
                      "that is not the Swap thread: not decoded",
                      cb, device);
  // AfterCall ran for it on this thread: the scissor and the shader identity still follow Run.
  CommandBufferHeader h;
  if (ReadCommandBufferHeader(base, cb, &h)) GuestState().EndCommandBuffer(base, device, h);
}

// Inline draws (cmd_replay.h ReplayGap). Counted apart from the buffers: gaps a frame, the ones
// with draws, their draws issued, and the gaps the check refused.
struct GapCounters {
  uint64_t gaps = 0, with_draws = 0, draws = 0, refused = 0, dwords = 0;
};
GapCounters g_gaps;

void ReplayGap(const uint8_t* base, const RunContext& c, uint32_t first, uint32_t end) {
  State& s = g_state;
  Init(s);
  if (end <= first || ((end - first) & 3) || !c.renderer || c.in_native_call) return;
  const uint32_t dwords = (end - first) / 4;
  if (dwords > kMaxIbDwords) return;
  const uint32_t phys = GpuAddress(first);
  if (uint64_t(phys) + 4ull * dwords > kPhysicalEnd) return;
  const uint8_t* const pm = rexnative::host::Memory()->GetPhysicalMembase();
  ++g_gaps.gaps;
  g_gaps.dwords += dwords;
  GuestStateTracker& gs = GuestState();
  // The bin state the GPU has there: the segment start's SET_BIN_MASK from the device and
  // SET_BIN_SELECT 0x00000000FFFFFFFF, as a buffer starts from.
  constexpr uint32_t kBinMask = rexnative::game::kCmdBufBinMaskOffset;
  const uint64_t mask = (uint64_t(Load32(base, c.device + kBinMask + 4)) << 32) |
                        Load32(base, c.device + kBinMask);
  auto setup = [&](Walker& w) {
    w.predication = REXCVAR_GET(rexnative_cmdbuf_predication);
    w.mask = mask;
    w.select = 0xFFFFFFFFull;
    w.budget = kBudgetDwords;
  };
  Walker w1(base, pm, c, s);
  setup(w1);
  uint32_t walk = dwords;
  if (!w1.Execute<false>(phys, walk, 0)) {
    // What precedes the packet the check refused is whole: walked alone. A stream that runs
    // into a hole the D3D reserved (a segment's start, data written later) ends there.
    const bool prefix = w1.fail_ib == phys && w1.fail_offset > 0 && w1.fail_offset < walk;
    ++g_gaps.refused;
    REXNATIVE_LOG_FIRST(8, WARN,
                        "ReXNative inline draws: the stream at {:08X} ({} dwords) refused: {} at "
                        "{:08X}+{} op {:02X} ({}){}",
                        first, dwords, kReasonNames[size_t(w1.reason)], w1.fail_ib,
                        w1.fail_offset, w1.fail_op, OpName(w1.fail_op),
                        prefix ? "; the packets before it walked" : "");
    if (!prefix) return;
    walk = w1.fail_offset;
    Walker again(base, pm, c, s);
    setup(again);
    if (!again.Execute<false>(phys, walk, 0)) return;
  }
  // Every register group kept: what the stream wrote stays on the GPU, as it does after these
  // packets. Not the shader loads (keep +16 bits 19 and 20): the stream's shader is the inlined
  // draw's, and the next hooked draw reloads the bound ones (Orcs Must Die!'s "Press Start",
  // drawn by DrawVerticesUP after an inlined draw's immediate vertex shader, got that shader).
  CommandBufferHeader h;
  std::fill(std::begin(h.keep), std::end(h.keep), ~0ull);
  h.keep[2] &= ~((1ull << 19) | (1ull << 20));
  gs.BeginReplay(h);
  Walker w2(base, pm, c, s);
  setup(w2);
  w2.Execute<true>(phys, walk, 0);
  gs.EndCommandBuffer(base, c.device, h);
  if (w2.draws_seen) gs.DropPendingImmediateVs();
  if (w2.draws_emitted) {
    ++g_gaps.with_draws;
    g_gaps.draws += w2.draws_emitted;
    REXNATIVE_LOG_FIRST(1, INFO,
                        "ReXNative inline draws: {} draws no hook saw, from the stream at {:08X}",
                        w2.draws_emitted, first);
  }
}

void OnSwapGaps(int32_t stats_every) {
  static uint32_t frames = 0;
  static GapCounters last;
  if (stats_every <= 0 || ++frames < uint32_t(stats_every)) return;
  const GapCounters& g = g_gaps;
  const double n = double(frames);
  REXLOG_INFO("ReXNative inline draws: per frame: streams {:.1f} ({:.1f} with draws, {:.1f} "
              "refused), draws {:.1f}, dwords {:.0f}",
              double(g.gaps - last.gaps) / n, double(g.with_draws - last.with_draws) / n,
              double(g.refused - last.refused) / n, double(g.draws - last.draws) / n,
              double(g.dwords - last.dwords) / n);
  last = g;
  frames = 0;
}

void AfterRun(const uint8_t* base, const RunContext& c) {
  State& s = g_state;
  Init(s);
  Counters& f = s.frame;
  ++f[kCalls];
  const uint64_t call = s.call_index++;
  CommandBufferHeader h;
  if (!ReadCommandBufferHeader(base, c.cb, &h)) {
    // AfterCall took its general path for it, which already wrote the scissor.
    const bool async = c.cb && !(c.cb & 3) && (h.common & 0xF) == kCbTypeAsync;
    ++f[async ? kAsync : kInvalid];
    REXNATIVE_LOG_FIRST(4, INFO,
                        "ReXNative cmdbuf: call {} object {:08X} Common {:08X}: {}, not decoded",
                        call, c.cb, h.common,
                        async ? "type 10 [INFERENCE: an async command buffer call]"
                              : "not a command buffer");
    return;
  }
  GuestStateTracker& gs = GuestState();

  // The mode for this call.
  int32_t mode = s.mode;
  const uint32_t status = h.flags & 0xF;
  if (mode && (status == 0 || status == 2)) {
    // Never begun, or still recording (BeginCommandBuffer's 2): its chunk list is not complete.
    ++f[kNotEnded];
    REXNATIVE_LOG_FIRST(4, WARN, "ReXNative cmdbuf: call {} cb {:08X} status {}: not ended", call,
                        c.cb, status);
    mode = 0;
  } else if (mode && status == 7) {
    // EndCommandBuffer out of memory: its chunk list is still complete.
    ++f[kOom];
    REXNATIVE_LOG_FIRST(4, WARN, "ReXNative cmdbuf: call {} cb {:08X} ended out of memory", call,
                        c.cb);
  } else if (mode && status != 1) {
    ++f[kBadStatus];
    REXNATIVE_LOG_FIRST(4, WARN, "ReXNative cmdbuf: call {} cb {:08X} status {}: unknown", call,
                        c.cb, status);
    mode = 0;
  }
  if (mode && c.follow_swap_device_only && (!c.main_device || c.device != c.main_device)) {
    ++f[kNotMain];
    mode = std::min(mode, 1);
  }
  if (mode == 2) {
    if (!c.follow_swap_device_only) {
      // The recording device's calls reach the renderer as well: its draws would show twice.
      REXNATIVE_WARN_ONCE(
          "ReXNative cmdbuf: replay needs a layer that follows only the presenting device "
          "(kFollowSwapDeviceOnly): counting instead");
      mode = 1;
    } else if (!c.renderer || c.in_native_call) {
      mode = 1;
    }
  }
  if (!mode) {
    gs.EndCommandBuffer(base, c.device, h);
    return;
  }

  const uint64_t t0 = Ticks();
  const uint8_t* const pm = rexnative::host::Memory()->GetPhysicalMembase();
  ++f[kBuffers];
  s.ibs.clear();
  uint32_t top_dwords = 0, bad_node = 0;
  const Reason collected = CollectChunks(base, h, s.ibs, &top_dwords, &bad_node);
  f[kIbs] += s.ibs.size();
  f[kDwords] += top_dwords;

  // The bin state the buffer starts with: the device's segment start writes SET_BIN_MASK from
  // its bin mask and SET_BIN_SELECT 0x00000000FFFFFFFF (Iron Brigade's sub_826430B0), and with
  // flag 0x40 Run writes SET_BIN_SELECT_HI = 0 before the IBs.
  constexpr uint32_t kBinMask = rexnative::game::kCmdBufBinMaskOffset;
  uint64_t mask = (uint64_t(Load32(base, c.device + kBinMask + 4)) << 32) |
                  Load32(base, c.device + kBinMask);
  uint64_t select = 0xFFFFFFFFull;
  mask = BinOverride(REXCVAR_GET(rexnative_cmdbuf_bin_mask), mask);
  select = BinOverride(REXCVAR_GET(rexnative_cmdbuf_bin_select), select);
  if (h.flags & 0x40) select &= 0xFFFFFFFFull;

  const bool detail = s.log_left > 0;
  const bool dumping = s.dump_left > 0;
  std::string dump, rd;
  std::vector<uint32_t> mirror;
  if (dumping) mirror.assign(gs.regs(), gs.regs() + kGuestRegisterCount);
  auto setup = [&](Walker& w) {
    w.nested_ok = (h.flags & 0x20) && REXCVAR_GET(rexnative_cmdbuf_nested);
    w.predication = REXCVAR_GET(rexnative_cmdbuf_predication);
    w.internal_draws = REXCVAR_GET(rexnative_cmdbuf_internal_draws);
    w.time_draws = profile::On();
    w.mask = mask;
    w.select = select;
    w.budget = kBudgetDwords - top_dwords;
  };

  Walker w1(base, pm, c, s);
  setup(w1);
  if (dumping) {
    w1.dump = &dump;
    w1.mirror = mirror.data();
  }
  bool ok = collected == Reason::kNone;
  if (!ok) w1.Fail(collected, bad_node, 0, 0);
  ok = ok && w1.Walk<false>(s.ibs);
  if (!ok) {
    ++f[kRejected];
    ++f[kRejectFirst + uint32_t(w1.reason)];
    REXNATIVE_LOG_FIRST(8, WARN,
                        "ReXNative cmdbuf: call {} cb {:08X} rejected: {} at {:08X}+{} op {:02X} "
                        "({})",
                        call, c.cb, kReasonNames[size_t(w1.reason)], w1.fail_ib, w1.fail_offset,
                        w1.fail_op, OpName(w1.fail_op));
    // A chunk list refused: the node as the guest wrote it (next, count, the entries), since
    // a per-frame buffer's memory is reused before anyone can read it.
    if (collected != Reason::kNone && bad_node >= 0xA0000000u && !(bad_node & 3))
      REXNATIVE_LOG_FIRST(4, WARN,
                          "ReXNative cmdbuf: node {:08X}: {:08X} {:08X} | {:08X} {:08X} | "
                          "{:08X} {:08X} | {:08X} {:08X}",
                          bad_node, Load32(base, bad_node), Load32(base, bad_node + 4),
                          Load32(base, bad_node + 8), Load32(base, bad_node + 12),
                          Load32(base, bad_node + 16), Load32(base, bad_node + 20),
                          Load32(base, bad_node + 24), Load32(base, bad_node + 28));
  }

  bool replayed = false;
  uint32_t emitted = 0;
  uint64_t draw_ticks = 0;
  if (ok && mode == 2) {
    Walker w2(base, pm, c, s);
    setup(w2);
    if (dumping || c.trace) w2.rd = &rd;
    gs.BeginReplay(h);
    w2.Walk<true>(s.ibs);
    replayed = true;
    emitted = w2.draws_emitted;
    draw_ticks = w2.draw_ticks;
  }
  const uint64_t ticks = Ticks() - t0;
  f[kDecodeTicks] += ticks - std::min(ticks, draw_ticks);
  f[kDrawTicks] += draw_ticks;

  gs.EndCommandBuffer(base, c.device, h);
  // The tap saw Run's scissor before the replay mirrored the buffer's writes: end in GPU order.
  if (c.tap_write && replayed)
    for (uint32_t reg = 0x2080; reg <= 0x2082; ++reg) c.tap_write(reg, gs.regs()[reg]);

  if (!detail && !dumping && !c.trace) return;
  const std::string head = Header(pm, c, h, call, s.ibs, mask, select);
  std::string result;
  Append(result, "mode %d result %s", mode, ok ? "ok" : "rejected:");
  if (!ok)
    Append(result, "%s@%08X+%u", kReasonNames[size_t(w1.reason)], w1.fail_ib, w1.fail_offset);
  Append(result, " op");
  for (uint32_t op = 0; op < 128; ++op)
    if (w1.ops[op]) Append(result, " %02X=%u", op, w1.ops[op]);
  Append(result, " draws %u/%u nested %u/%u bin %u", emitted, w1.draws_seen, w1.nested_followed,
         w1.nested_refused, w1.bin_packets);
  if (detail) {
    --s.log_left;
    REXLOG_INFO("ReXNative cmdbuf: {} | {}", head, result);
  }
  if (dumping) {
    --s.dump_left;
    std::error_code ec;
    const std::filesystem::path dir(s.dump_dir.empty() ? std::string(".") : s.dump_dir);
    std::filesystem::create_directories(dir, ec);
    char name[160];
    std::snprintf(name, sizeof(name), "rexnative-cmdbuf-%s-f%llu-c%llu.txt", s.dump_tag.c_str(),
                  (unsigned long long)c.frame, (unsigned long long)call);
    const std::filesystem::path path = dir / name;
    std::ofstream out(path, std::ios::binary);
    out << "# " << head << " | " << result << "\n" << dump << rd;
    REXLOG_INFO("ReXNative cmdbuf: wrote {} ({} bytes{})", path.string(),
                head.size() + result.size() + dump.size() + rd.size(),
                out.good() ? "" : ", failed");
  }
  if (c.trace) {
    std::string& t = *c.trace;
    t += "# cmdbuf " + head + "\n";
    for (size_t at = 0; at < rd.size();) {
      const size_t end = rd.find('\n', at);
      const size_t stop = end == std::string::npos ? rd.size() : end;
      t += "# cmdbuf ";
      t.append(rd, at, stop - at);
      t += "\n";
      at = stop + 1;
    }
    t += "# cmdbuf " + result + "\n";
  }
}

void Control(const std::string& arg, const std::string& arg2, const std::string& trace_dir) {
  State& s = g_state;
  Init(s);
  if (arg == "0" || arg == "1" || arg == "2") {
    s.requested = arg[0] - '0';
    REXLOG_INFO("ReXNative cmdbuf: mode {} requested (from the next frame)", s.requested);
    if (s.requested == 2 && !rexnative::game::kFollowSwapDeviceOnly)
      REXLOG_WARN("ReXNative cmdbuf: this layer follows every device: replay stays at count");
  } else if (arg == "log") {
    const long n = arg2.empty() ? 4 : std::strtol(arg2.c_str(), nullptr, 10);
    s.log_left = int32_t(std::clamp<long>(n, 0, 1 << 20));
    REXLOG_INFO("ReXNative cmdbuf: logging the next {} calls in detail", s.log_left);
  } else if (arg == "dump") {
    // `dump tag:N`: the next N calls (Midnight Club: Los Angeles runs 67 a frame).
    std::string tag = arg2;
    long count = 2;  // one Iron Brigade frame: two calls
    if (const size_t colon = tag.rfind(':'); colon != std::string::npos) {
      count = std::strtol(tag.c_str() + colon + 1, nullptr, 10);
      tag.resize(colon);
    }
    s.dump_tag = tag.empty() ? "cb" : tag;
    s.dump_dir = trace_dir.empty() ? "." : trace_dir;
    s.dump_left = uint32_t(std::clamp<long>(count, 1, 1000));
    REXLOG_INFO("ReXNative cmdbuf: dumping the next {} decoded calls to {}{}", s.dump_left,
                s.dump_dir, s.mode ? "" : " (none while the mode is 0)");
  } else {
    REXLOG_WARN("ReXNative cmdbuf: unknown command '{} {}' (cmdbuf 0|1|2, log N, dump [tag[:N]])",
                arg, arg2);
  }
}

bool InlineData(uint32_t start, uint32_t size) {
  State& s = g_state;
  if (!size) return false;
  for (uint32_t i = 0; i < s.inline_count; ++i) {
    const Range& r = s.inline_data[i];
    if (start >= r.begin && uint64_t(start) + size <= r.end) {
      ++s.frame[kInlineRanges];
      return true;
    }
  }
  return false;
}

}  // namespace rexnative::cmdbuf
