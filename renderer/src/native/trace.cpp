#include "trace.h"

#include "clock.h"
#include "d3d12_device.h"
#include "d3d_state.h"
#include "draw.h"
#include "frame_pacer.h"
#include "log.h"
#include "present.h"
#include "render_targets.h"
#include "renderer.h"
#include "resolve.h"
#include "textures.h"
#include "../host.h"

#include <rex/cvar.h>
#include <rex/kernel/heap.h>
#include <rex/logging.h>

#include <windows.h>
#include <xxhash.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <type_traits>
#include <unordered_map>
#include <vector>

REXCVAR_DEFINE_INT32(rexnative_trace_record, 0, "ReXNative",
                     "Debug: record the next this many frames into a frame trace (live; "
                     "trace.h), for --rexnative_trace_play");
REXCVAR_DEFINE_STRING(rexnative_trace_dir, "", "ReXNative",
                      "Debug: the folder of the frame traces (<game>-f<frame>.rxt); empty: "
                      "--rexnative_d3d_trace_dir, else trace beside the executable");
REXCVAR_DEFINE_STRING(rexnative_trace_play, "", "ReXNative",
                      "Debug: play this frame trace instead of the game, from the game's first "
                      "Swap on (trace.h)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(rexnative_trace_play_loops, 0, "ReXNative",
                     "Debug: exit after playing the trace this many times (0: until closed)");
REXCVAR_DEFINE_INT32(rexnative_trace_play_fps, 30, "ReXNative",
                     "Debug: frames per second of a played trace (0: as fast as they go)");
REXCVAR_DEFINE_BOOL(rexnative_trace_play_dump, false, "ReXNative",
                    "Debug: capture every frame of the last time through a played trace "
                    "(--rexnative_capture_dir, with --rexnative_trace_play_loops)");
REXCVAR_DECLARE(std::string, rexnative_d3d_trace_dir);
REXCVAR_DECLARE(std::string, rexnative_d3d_control);

namespace rexnative::trace {
namespace detail {
std::atomic<uint32_t> g_recording{0};
std::atomic<uint32_t> g_executing{0};
}  // namespace detail

namespace {

constexpr char kMagic[8] = {'R', 'X', 'T', 'R', 'A', 'C', 'E', '1'};
constexpr uint32_t kPhysicalSize = 0x20000000, kPageShift = 12, kPage = 1u << kPageShift;

enum Type : uint32_t {
  kRegsFull = 1,  // kGuestRegisterCount dwords
  kRegs,          // RegWrite[]
  kMem,           // u32 physical, bytes
  kDraw,          // DrawState, u32 vs dwords, u32 ps dwords, the immediate ucodes (big-endian)
  kClear,         // ClearCall
  kResolve,       // ResolveParams
  kSwap,          // SwapCall
  kTextureMem,    // u32 command (number + 1), u32 physical, bytes
};

struct Header {
  char magic[8];
  uint32_t draw_size, clear_size, resolve_size, swap_size;
  uint64_t first_frame;
  char game[64];
};

// A ClearCall without its vectors; its rects, then its edram_rects, follow it.
struct FlatClear {
  uint32_t flags;
  float color[4], depth;
  uint8_t stencil, tiling, edram_alias, depth_bound;
  SurfaceRegs regs;
  uint32_t bound_colors, height, rects, edram_rects;
};
static_assert(std::is_trivially_copyable_v<DrawState> &&
              std::is_trivially_copyable_v<ResolveParams> &&
              std::is_trivially_copyable_v<SwapCall>);

struct Recorder {
  std::mutex mutex;  // the file: the guest thread's records and the render thread's
  std::FILE* file = nullptr;
  std::string path;
  // XXH3 of each page as last recorded: the guest thread's ranges, and the textures' apart (the
  // player applies those when the render thread read them, a frame behind the others).
  std::unordered_map<uint32_t, uint64_t> pages, texture_pages;
  std::vector<std::pair<uint32_t, uint32_t>> runs;  // Mem's scratch
  uint32_t commands = 0, frames_left = 0, frames = 0;
  uint64_t bytes = 0, memory_bytes = 0, texture_bytes = 0, first_frame = 0;
  uint32_t id = 0;
  bool first = true;
} g_rec;

std::string GameName() {
  wchar_t path[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
  return std::filesystem::path(std::wstring(path, n)).stem().string();
}

std::filesystem::path TraceDir() {
  std::string dir = REXCVAR_GET(rexnative_trace_dir);
  if (dir.empty()) dir = REXCVAR_GET(rexnative_d3d_trace_dir);
  if (!dir.empty()) return dir;
  wchar_t path[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
  return std::filesystem::path(std::wstring(path, n)).parent_path() / "trace";
}

// mutex held.
void Write(Type type, const void* a, size_t an, const void* b = nullptr, size_t bn = 0,
           const void* c = nullptr, size_t cn = 0) {
  const uint32_t head[2] = {type, uint32_t(an + bn + cn)};
  std::fwrite(head, 1, sizeof(head), g_rec.file);
  if (an) std::fwrite(a, 1, an, g_rec.file);
  if (bn) std::fwrite(b, 1, bn, g_rec.file);
  if (cn) std::fwrite(c, 1, cn, g_rec.file);
  g_rec.bytes += sizeof(head) + an + bn + cn;
}

// The pages of [physical, physical + size) whose content differs from `pages`, as runs, written
// as records of `type` (with `tag` first for kTextureMem). mutex held.
uint64_t WritePages(std::unordered_map<uint32_t, uint64_t>& pages, Type type, uint32_t tag,
                    uint32_t physical, uint32_t size) {
  if (!size || physical >= kPhysicalSize) return 0;
  const uint8_t* membase = host::Memory()->GetPhysicalMembase();
  const uint32_t end = uint32_t(std::min<uint64_t>(uint64_t(physical) + size, kPhysicalSize));
  uint64_t written = 0;
  uint32_t run = ~0u;
  const auto flush = [&](uint32_t page_end) {
    if (run == ~0u) return;
    const uint32_t at = run << kPageShift, bytes = (page_end - run) << kPageShift;
    if (type == kTextureMem) {
      const uint32_t head[2] = {tag, at};
      Write(type, head, sizeof(head), membase + at, bytes);
    } else {
      Write(type, &at, sizeof(at), membase + at, bytes);
    }
    written += bytes;
    run = ~0u;
  };
  for (uint32_t page = physical >> kPageShift; page <= (end - 1) >> kPageShift; ++page) {
    const uint64_t hash = XXH3_64bits(membase + (page << kPageShift), kPage);
    const auto [it, inserted] = pages.try_emplace(page, hash);
    if (!inserted && it->second == hash) {
      flush(page);
      continue;
    }
    it->second = hash;
    if (run == ~0u) run = page;
  }
  flush(((end - 1) >> kPageShift) + 1);
  return written;
}

// mutex held: what precedes a command's own record. Returns its number + 1.
uint32_t BeforeCommand(const uint32_t* all, const RegWrite* regs, size_t count) {
  if (g_rec.first) {
    g_rec.first = false;
    Write(kRegsFull, all, kGuestRegisterCount * sizeof(uint32_t));
  } else if (count) {
    Write(kRegs, regs, count * sizeof(RegWrite));
  }
  return ++g_rec.commands;
}

template <class T>
uint32_t Command(Type type, const T& value, const uint32_t* all, const RegWrite* regs,
                 size_t count) {
  std::lock_guard<std::mutex> lock(g_rec.mutex);
  if (!g_rec.file) return 0;
  const uint32_t command = BeforeCommand(all, regs, count);
  Write(type, &value, sizeof(T));
  return command;
}

}  // namespace

bool AtSwap(uint64_t frame) {
  if (Recording()) {
    std::lock_guard<std::mutex> lock(g_rec.mutex);
    if (--g_rec.frames_left) return true;
    detail::g_recording.store(0, std::memory_order_relaxed);
    std::fclose(g_rec.file);
    g_rec.file = nullptr;
    REXLOG_INFO(
        "ReXNative trace: recorded {} frames from {}, {} commands, {:.1f} MB ({:.1f} MB of "
        "vertices and shaders, {:.1f} MB of textures): {}",
        g_rec.frames, g_rec.first_frame, g_rec.commands, double(g_rec.bytes) / (1 << 20),
        double(g_rec.memory_bytes) / (1 << 20), double(g_rec.texture_bytes) / (1 << 20), g_rec.path);
    g_rec.pages.clear();
    g_rec.texture_pages.clear();
    return false;
  }
  const int32_t asked = REXCVAR_GET(rexnative_trace_record);
  if (asked <= 0) return false;
  rex::cvar::SetFlagByName("rexnative_trace_record", "0");
  const std::filesystem::path dir = TraceDir();
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const std::string game = GameName();
  std::lock_guard<std::mutex> lock(g_rec.mutex);
  g_rec.path = (dir / std::format("{}-f{}.rxt", game, frame)).string();
  g_rec.file = std::fopen(g_rec.path.c_str(), "wb");
  if (!g_rec.file) {
    REXLOG_ERROR("ReXNative trace: cannot write {}", g_rec.path);
    return false;
  }
  std::setvbuf(g_rec.file, nullptr, _IOFBF, 4 << 20);
  Header h = {};
  std::memcpy(h.magic, kMagic, sizeof(kMagic));
  h.draw_size = sizeof(DrawState);
  h.clear_size = sizeof(FlatClear);
  h.resolve_size = sizeof(ResolveParams);
  h.swap_size = sizeof(SwapCall);
  h.first_frame = frame;
  std::strncpy(h.game, game.c_str(), sizeof(h.game) - 1);
  std::fwrite(&h, 1, sizeof(h), g_rec.file);
  g_rec.commands = 0;
  g_rec.frames = g_rec.frames_left = uint32_t(std::min(asked, 600));
  g_rec.bytes = sizeof(h);
  g_rec.memory_bytes = g_rec.texture_bytes = 0;
  g_rec.first_frame = frame;
  g_rec.first = true;
  detail::g_executing.store(0, std::memory_order_relaxed);
  detail::g_recording.store(++g_rec.id, std::memory_order_relaxed);
  REXLOG_INFO("ReXNative trace: recording {} frames from {}", g_rec.frames, frame);
  return true;
}

uint32_t Draw(const DrawState& state, const uint32_t* all, const RegWrite* regs, size_t count) {
  std::lock_guard<std::mutex> lock(g_rec.mutex);
  if (!g_rec.file) return 0;
  const uint32_t command = BeforeCommand(all, regs, count);
  DrawState s = state;
  s.regs = nullptr;
  s.vs.immediate = s.ps.immediate = nullptr;
  const uint32_t dwords[2] = {state.vs.immediate ? state.vs.dwords : 0,
                              state.ps.immediate ? state.ps.dwords : 0};
  const uint32_t head[2] = {kDraw, uint32_t(sizeof(s) + sizeof(dwords) + (dwords[0] + dwords[1]) * 4)};
  std::fwrite(head, 1, sizeof(head), g_rec.file);
  std::fwrite(&s, 1, sizeof(s), g_rec.file);
  std::fwrite(dwords, 1, sizeof(dwords), g_rec.file);
  if (dwords[0]) std::fwrite(state.vs.immediate, 4, dwords[0], g_rec.file);
  if (dwords[1]) std::fwrite(state.ps.immediate, 4, dwords[1], g_rec.file);
  g_rec.bytes += sizeof(head) + head[1];
  return command;
}

uint32_t Clear(const ClearCall& c, const uint32_t* all, const RegWrite* regs, size_t count) {
  std::lock_guard<std::mutex> lock(g_rec.mutex);
  if (!g_rec.file) return 0;
  const uint32_t command = BeforeCommand(all, regs, count);
  FlatClear f = {};
  f.flags = c.p.flags;
  std::memcpy(f.color, c.p.color, sizeof(f.color));
  f.depth = c.p.depth;
  f.stencil = c.p.stencil;
  f.tiling = c.p.tiling;
  f.edram_alias = c.p.edram_alias;
  f.depth_bound = c.depth_bound;
  f.regs = c.regs;
  f.bound_colors = c.bound_colors;
  f.height = c.height;
  f.rects = uint32_t(c.p.rects.size());
  f.edram_rects = uint32_t(c.p.edram_rects.size());
  Write(kClear, &f, sizeof(f), c.p.rects.data(), f.rects * sizeof(D3D12_RECT),
        c.p.edram_rects.data(), f.edram_rects * sizeof(D3D12_RECT));
  return command;
}
uint32_t Resolve(const ResolveParams& p, const uint32_t* all, const RegWrite* regs, size_t count) {
  return Command(kResolve, p, all, regs, count);
}
uint32_t Swap(const SwapCall& s, const uint32_t* all, const RegWrite* regs, size_t count) {
  return Command(kSwap, s, all, regs, count);
}

void Regs(const RegWrite* regs, size_t count) {
  std::lock_guard<std::mutex> lock(g_rec.mutex);
  // Before the trace's first command its whole register file is written, with these in it.
  if (g_rec.file && !g_rec.first && count) Write(kRegs, regs, count * sizeof(RegWrite));
}

void Mem(uint32_t physical, uint32_t size) {
  std::lock_guard<std::mutex> lock(g_rec.mutex);
  if (g_rec.file) g_rec.memory_bytes += WritePages(g_rec.pages, kMem, 0, physical, size);
}

void TextureMem(uint32_t physical, uint32_t size) {
  std::lock_guard<std::mutex> lock(g_rec.mutex);
  if (g_rec.file)
    g_rec.texture_bytes +=
        WritePages(g_rec.texture_pages, kTextureMem,
                   detail::g_executing.load(std::memory_order_relaxed), physical, size);
}

const std::string& PlayPath() {
  static const std::string path = REXCVAR_GET(rexnative_trace_play);
  return path;
}

}  // namespace rexnative::trace

// ---------------------------------------------------------------------------------------------
// The player.

namespace rexnative {

void NativeRenderer::PlayTrace(uint8_t* base) {
  using namespace trace;
  struct Record {
    uint32_t type, size;
    const uint8_t* data;
  };
  std::vector<uint8_t> file;
  std::vector<Record> records;
  std::vector<Record> texture_mem;  // in the order recorded: by command
  uint32_t frames = 0, draws = 0;
  {
    std::ifstream in(PlayPath(), std::ios::binary | std::ios::ate);
    const std::streamoff size = in ? std::streamoff(in.tellg()) : 0;
    file.resize(size_t(std::max<std::streamoff>(size, 0)));
    in.seekg(0);
    if (size) in.read(reinterpret_cast<char*>(file.data()), size);
    Header h = {};
    if (file.size() >= sizeof(h)) std::memcpy(&h, file.data(), sizeof(h));
    if (std::memcmp(h.magic, kMagic, sizeof(kMagic)) != 0) {
      REXLOG_ERROR("ReXNative trace: {} is not a frame trace", PlayPath());
      TerminateProcess(GetCurrentProcess(), 2);
    }
    if (h.draw_size != sizeof(DrawState) || h.clear_size != sizeof(FlatClear) ||
        h.resolve_size != sizeof(ResolveParams) || h.swap_size != sizeof(SwapCall)) {
      REXLOG_ERROR("ReXNative trace: {} was recorded by another build (its commands have other "
                   "sizes): record it again",
                   PlayPath());
      TerminateProcess(GetCurrentProcess(), 2);
    }
    for (size_t at = sizeof(h); at + 8 <= file.size();) {
      Record r;
      std::memcpy(&r, file.data() + at, 8);
      r.data = file.data() + at + 8;
      at += 8 + size_t(r.size);
      if (at > file.size()) break;  // a trace cut short
      if (r.type == kTextureMem) {
        texture_mem.push_back(r);
      } else {
        records.push_back(r);
        frames += r.type == kSwap;
        draws += r.type == kDraw;
      }
    }
    // The texture records were written as the render thread ran each command.
    std::stable_sort(texture_mem.begin(), texture_mem.end(), [](const Record& a, const Record& b) {
      uint32_t ca, cb;
      std::memcpy(&ca, a.data, 4);
      std::memcpy(&cb, b.data, 4);
      return ca < cb;
    });
    REXLOG_INFO("ReXNative trace: playing {} ({}, recorded from frame {}): {} frames, {} draws, "
                "{:.1f} MB",
                PlayPath(), h.game, h.first_frame, frames, draws, double(file.size()) / (1 << 20));
  }
  if (!frames) {
    REXLOG_ERROR("ReXNative trace: {} has no whole frame", PlayPath());
    TerminateProcess(GetCurrentProcess(), 2);
  }
  // The captures' Xenos copy is the plugin's frame of the game, which is not running.
  rex::cvar::SetFlagByName("rexnative_capture_xenos", "false");
  // Guest memory through a CPU view, as the game writes it: the page watches see the writes
  // (textures and shared memory pages are read again).
  // ... and the renderer is told as well: a page copied a moment ago has no watch yet.
  const auto write = [this, base](uint32_t physical, const uint8_t* bytes, uint32_t size) {
    if (physical >= kPhysicalSize || size > kPhysicalSize - physical) return;
    std::memcpy(base + 0xA0000000u + physical, bytes, size);
    draw_->GuestWritten(physical, size);
    textures_->GuestWritten(physical, size);
  };
  static uint32_t regs[kGuestRegisterCount];
  std::vector<std::vector<uint32_t>> ucode(2);
  const int32_t loops = REXCVAR_GET(rexnative_trace_play_loops);
  const bool dump = REXCVAR_GET(rexnative_trace_play_dump) && loops > 0;
  const std::string control = REXCVAR_GET(rexnative_d3d_control);
  // A dump's last captures are read back while the frames after them are drawn: once more through.
  const int32_t total = loops + (dump ? 1 : 0);
  for (int32_t loop = 0; loops <= 0 || loop < total; ++loop) {
    const uint64_t loop_start = NowNs();
    uint64_t frame_start = loop_start;
    uint32_t command = 0;
    size_t texture_at = 0;
    const auto before_command = [&] {
      ++command;
      bool waited = false;
      for (; texture_at < texture_mem.size(); ++texture_at) {
        const Record& r = texture_mem[texture_at];
        uint32_t head[2];
        std::memcpy(head, r.data, 8);
        if (head[0] > command) break;
        // The untile workers read what the commands before this one bound.
        if (!std::exchange(waited, true)) textures_->FinishUploads();
        write(head[1], r.data + 8, r.size - 8);
      }
    };
    for (const Record& r : records) {
      switch (r.type) {
        case kRegsFull:
          std::memcpy(regs, r.data, std::min<size_t>(r.size, sizeof(regs)));
          break;
        case kRegs:
          for (uint32_t i = 0; i + sizeof(RegWrite) <= r.size; i += sizeof(RegWrite)) {
            RegWrite w;
            std::memcpy(&w, r.data + i, sizeof(w));
            if (w.reg < kGuestRegisterCount) regs[w.reg] = w.value;
          }
          break;
        case kMem: {
          uint32_t physical;
          std::memcpy(&physical, r.data, 4);
          write(physical, r.data + 4, r.size - 4);
          break;
        }
        case kDraw: {
          before_command();
          DrawState s;
          std::memcpy(&s, r.data, sizeof(s));
          uint32_t dwords[2];
          std::memcpy(dwords, r.data + sizeof(s), sizeof(dwords));
          const uint8_t* at = r.data + sizeof(s) + sizeof(dwords);
          ShaderRef* refs[2] = {&s.vs, &s.ps};
          for (int i = 0; i < 2; ++i) {
            if (!dwords[i]) continue;
            ucode[i].resize(dwords[i]);
            std::memcpy(ucode[i].data(), at, size_t(dwords[i]) * 4);
            at += size_t(dwords[i]) * 4;
            refs[i]->immediate = ucode[i].data();
          }
          s.regs = regs;
          ++frame_draw_calls_;
          ExecuteDraw(s, nullptr);
          break;
        }
        case kClear: {
          before_command();
          FlatClear f;
          std::memcpy(&f, r.data, sizeof(f));
          ClearCall c;
          c.p.flags = f.flags;
          std::memcpy(c.p.color, f.color, sizeof(f.color));
          c.p.depth = f.depth;
          c.p.stencil = f.stencil;
          c.p.tiling = f.tiling;
          c.p.edram_alias = f.edram_alias;
          c.depth_bound = f.depth_bound;
          c.regs = f.regs;
          c.bound_colors = f.bound_colors;
          c.height = f.height;
          c.p.rects.resize(f.rects);
          c.p.edram_rects.resize(f.edram_rects);
          std::memcpy(c.p.rects.data(), r.data + sizeof(f), f.rects * sizeof(D3D12_RECT));
          std::memcpy(c.p.edram_rects.data(), r.data + sizeof(f) + f.rects * sizeof(D3D12_RECT),
                      f.edram_rects * sizeof(D3D12_RECT));
          ExecuteClear(c);
          break;
        }
        case kResolve: {
          before_command();
          ResolveParams p;
          std::memcpy(&p, r.data, sizeof(p));
          ExecuteResolve(p);
          break;
        }
        case kSwap: {
          before_command();
          SwapCall s;
          std::memcpy(&s, r.data, sizeof(s));
          s.hand_over = false;
          s.capture_request = dump && loop == loops - 1 ? 1 : -1;
          stats_.guest_swap_ns = NowNs();
          ExecuteSwap(s);
          if (!Live()) {
            REXLOG_ERROR("ReXNative trace: the renderer failed; the player stops");
            TerminateProcess(GetCurrentProcess(), 3);
          }
          frame_draw_calls_ = 0;
          if (pacer_) pacer_->WaitToBegin();
          if (const int32_t fps = REXCVAR_GET(rexnative_trace_play_fps); fps > 0) {
            const uint64_t due = frame_start + 1'000'000'000ull / uint64_t(std::min(fps, 1000));
            const uint64_t now = NowNs();
            if (now < due) Sleep(DWORD((due - now) / 1'000'000));
          }
          frame_start = stats_.guest_begin_ns = NowNs();
          // The control file's `set` lines (the hooks that read it are the game's).
          std::error_code ec;
          if (!control.empty() && std::filesystem::exists(control, ec)) {
            std::ifstream in(control);
            std::string line;
            while (std::getline(in, line)) {
              std::istringstream ss(line);
              std::string cmd, name, value;
              ss >> cmd >> name;
              std::getline(ss >> std::ws, value);
              if (cmd == "set" && !name.empty() && !value.empty()) {
                rex::cvar::SetFlagByName(name, value);
                REXLOG_INFO("ReXNative trace control: {}", line);
              }
            }
            in.close();
            std::filesystem::remove(control, ec);
          }
          break;
        }
        default:
          break;
      }
    }
    REXLOG_INFO("ReXNative trace: time {} through: {} frames in {:.0f} ms", loop + 1, frames,
                double(NowNs() - loop_start) / 1e6);
  }
  // The captures' files are written off this thread.
  device_->WaitIdle();
  Sleep(2500);
  REXLOG_INFO("ReXNative trace: played {} times; exit", loops);
  TerminateProcess(GetCurrentProcess(), 0);
}

}  // namespace rexnative
