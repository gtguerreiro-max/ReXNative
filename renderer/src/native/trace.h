// ReXNative renderer: frame traces, the renderer's input recorded and replayed without the game
// (docs/DEBUGGING.md; in the spirit of Xenia's GPU trace and trace viewer).
//
// A trace holds, for a few frames in a row, what the game handed the renderer: each draw, clear,
// resolve and Swap as the hooks decoded it, the register writes between them, and the guest
// memory the renderer read for them (vertex and index ranges, shader ucode, texture data), each
// page once per content. It is recorded with the render thread on (the default):
//   --rexnative_trace_record=N   the next N frames (live: `set rexnative_trace_record N` in the
//                                control file), into <dir>\<game>-f<frame>.rxt
//   --rexnative_trace_dir        <dir>; empty: --rexnative_d3d_trace_dir, else trace beside the
//                                executable
// and played by the game's own executable, which has the renderer and the game's layer:
//   --rexnative_trace_play=<file>  at the game's first Swap the guest thread stops there for good
//                                  and the trace's frames are drawn instead, over and over, on
//                                  that thread (no render thread), deterministically: the same
//                                  frames every time, to capture, bisect or compare builds with
//   --rexnative_trace_play_loops=N   exit after N times through the trace (0: until closed)
//   --rexnative_trace_play_fps       frames per second (0: as fast as they go)
//   --rexnative_trace_play_dump      with --rexnative_capture_dir: the last time through, each
//                                    frame is captured (f<index>-native.png)
// Everything live works on a played trace: draw-debug.txt, captures, RenderDoc, `set` in the
// control file (the player reads the file itself, each frame).
//
// What a trace does not hold: images the GPU made before its first frame (render targets, the
// resolves of earlier frames that were never written back to guest memory). The first time
// through, the first frame reads them empty; from the second time on they are the last frame's.
// A trace replays in a build whose DrawState, ClearCall, ResolveParams and SwapCall have the
// sizes it was recorded with (checked).
//
// Threading: Mem and the command records on the guest thread; TextureMem and SetExecuting on the
// render thread.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace rexnative {
struct ClearCall;
struct DrawState;
struct RegWrite;
struct ResolveParams;
struct SwapCall;
}  // namespace rexnative

namespace rexnative::trace {

namespace detail {
extern std::atomic<uint32_t> g_recording;  // the trace's id while one is recorded, else 0
extern std::atomic<uint32_t> g_executing;  // the command the render thread runs (its number + 1)
}  // namespace detail

// The id of the trace being recorded (never 0), or 0.
inline uint32_t Recording() { return detail::g_recording.load(std::memory_order_relaxed); }

// Guest thread, at a Swap, once the render thread finished the frames before `frame`: starts a
// trace when one was asked for, and ends the one that has its frames. True while one is
// recorded.
bool AtSwap(uint64_t frame);
// Guest thread, while recording: the register writes since the last command (`regs`, `count`; for
// a trace's first command, the whole file `all`), then the command. Each returns the command's
// number + 1, for SetExecuting.
uint32_t Draw(const DrawState& state, const uint32_t* all, const RegWrite* regs, size_t count);
uint32_t Clear(const ClearCall& c, const uint32_t* all, const RegWrite* regs, size_t count);
uint32_t Resolve(const ResolveParams& p, const uint32_t* all, const RegWrite* regs, size_t count);
uint32_t Swap(const SwapCall& s, const uint32_t* all, const RegWrite* regs, size_t count);
// Guest thread, while recording: register writes that no command of the trace followed yet.
void Regs(const RegWrite* regs, size_t count);
// Guest thread, while recording: guest physical memory the command about to be recorded reads.
void Mem(uint32_t physical, uint32_t size);
// Render thread, while recording: texture memory the command it runs reads.
void TextureMem(uint32_t physical, uint32_t size);
inline void SetExecuting(uint32_t command) {
  detail::g_executing.store(command, std::memory_order_relaxed);
}

// --rexnative_trace_play; empty: the game runs.
const std::string& PlayPath();

}  // namespace rexnative::trace
