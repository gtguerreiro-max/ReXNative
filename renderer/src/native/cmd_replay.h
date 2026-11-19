// ReXNative renderer: recorded command buffers (XDK D3D RunCommandBuffer) replayed as native
// draws.
//
// A game can record draws into a command buffer object on a second D3D device and run it on the
// presenting one later (Iron Brigade's static scenery: its G-buffer pass). RunCommandBuffer
// flushes the dirty state the object inherits (d3d_state.h, CommandBufferHeader), emits one
// INDIRECT_BUFFER packet per entry of the object's chunk list, re-emits the window scissor and
// re-dirties everything the object does not keep. The recorded PM4 behind those packets never
// passes a hooked D3D call, so without this module the renderer draws none of it.
//
// The hook dispatch (rexnative_d3d_hooks.cpp) calls AfterRun once the guest RunCommandBuffer has
// returned (after the tracker's AfterCall copied the inherited flush). Depending on the mode,
// latched at each Swap on the presenting device so that a frame is never half replayed:
//   0 off     the tracker's exact entry flush and Run's scissor only; nothing is decoded;
//   1 count   plus a validate pass over the buffers (structure, opcode histogram, counters,
//             detail log lines, dumps): no effect on the tracker or the renderer;
//   2 replay  plus an apply pass: register writes go into the tracker (GuestStateTracker::
//             ReplayPut, in stream order through its register log), shader loads set its
//             shaders and draws reach NativeRenderer::OnDraw with fn = kRunCommandBuffer
//             (GuestStateTracker::ReadDrawState decodes them from r[1..3]).
// The validate pass runs first: a buffer it rejects has no effect at all. Mode 2 is lowered to 1
// when there is no live renderer, inside a renderer call, or when the game does not follow only
// the presenting device (game_layer.h kFollowSwapDeviceOnly: the recording device's own calls
// would reach the renderer too, and its recorded draws would be drawn twice).
//
// --rexnative_cmdbuf selects the mode (-1: the layer's kDefaultCmdBuf); the control command
// `cmdbuf 0|1|2` changes it at run time, `cmdbuf log N` logs the next N calls in detail and
// `cmdbuf dump [tag]` writes the next two calls packet by packet to the trace directory.
//
// Threading: everything here runs on the thread that last called Swap on the presenting device
// (the one the tracker and the renderer's guest side belong to). A RunCommandBuffer on any other
// thread only gets the tracker's EndCommandBuffer (AfterRunOtherThread) and one atomic count.
#pragma once

#include <cstdint>
#include <string>

namespace rexnative {
class NativeRenderer;
}

namespace rexnative::cmdbuf {

// One RunCommandBuffer call, filled by the hook dispatch.
struct RunContext {
  uint32_t device = 0, cb = 0;         // r3, r4 at RunCommandBuffer's entry
  NativeRenderer* renderer = nullptr;  // NativeRenderer::Live(); null: count at most
  uint32_t main_device = 0;            // the presenting device (0: not known yet)
  bool follow_swap_device_only = false, in_native_call = false;
  bool gap = false;  // ReplayGap's: the main stream, whose writes stay by design
  // ReplayGap: validate only, counting the shader constants the stream changes from the
  // tracker's values (rexnative.inline_draws_auto's probe).
  bool probe = false;
  std::string* trace = nullptr;  // the tap's frame trace while it traces this device
  void (*tap_write)(uint32_t reg, uint32_t value) = nullptr;  // tap register mirror (tap on)
  uint64_t frame = 0;                                          // for dump names
};

// Each Swap on the presenting device: records its thread (the one AfterRun accepts), latches the
// requested mode and logs the counters every `stats_every` frames (0: never).
void OnSwap(int32_t stats_every);
// Whether the calling thread is the one that called Swap last on the presenting device.
bool OnSwapThread();
// After the guest RunCommandBuffer returned, on the Swap thread (OnSwapThread), whenever the
// tracker's AfterCall ran for it.
void AfterRun(const uint8_t* base, const RunContext& c);
// rexnative.inline_draws: the push buffer the game wrote between two hooked calls, [first, end)
// (guest virtual addresses), walked as a buffer is: its register writes into the tracker, its
// shader loads, its draws to the renderer. A game whose build inlined some draws past the hooked
// D3D functions (Orcs Must Die!'s title and menus, DRAW_INDX packets no hook saw) has them drawn.
// On the Swap thread, on the presenting device.
void ReplayGap(const uint8_t* base, const RunContext& c, uint32_t first, uint32_t end);
// The draws ReplayGap issued so far (rexnative.inline_draws_auto's trial).
uint64_t GapDraws();
// The shader constant registers the probed gaps set to another value than the tracker held, so
// far: constants the game's own code gives the GPU, which no hooked call shows the tracker
// (rexnative.inline_draws_auto's probe).
uint64_t GapConstants();
// The resolves (copy draws) the probed gaps held, so far: resolves the game's own code writes,
// which no hooked Resolve makes (rexnative.inline_draws_auto's probe, rexnative.inline_resolves).
uint64_t GapResolves();
// A probe starts: its shader constant registers counted once each again (GapConstants).
void ProbeReset();
// At each Swap with inline draws on: their counts every `stats_every` frames.
void OnSwapGaps(int32_t stats_every);
// The same on any other thread: the tracker's EndCommandBuffer, and a count. Touches no state of
// this module but one atomic.
void AfterRunOtherThread(const uint8_t* base, uint32_t device, uint32_t cb);
// The control command `cmdbuf <arg> [arg2]` (rexnative_d3d_hooks.cpp PollControl).
void Control(const std::string& arg, const std::string& arg2, const std::string& trace_dir);

// For a replayed draw's vertex and index ranges (draw.cpp RequestVertices, inside OnDraw on the
// Swap thread): whether [start, start + size) lies inside the data a recorded BeginVertices or
// BeginIndexedVertices allocated for the draw being issued. The recording's allocator hands it
// out apart from the PM4 and the game records again every pass: copied for every draw like
// inline data rather than watched. False outside a replayed draw.
bool InlineData(uint32_t start, uint32_t size);

}  // namespace rexnative::cmdbuf
