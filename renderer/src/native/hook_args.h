// ReXNative renderer: the arguments of a hooked guest D3D call.
//
// What the hook dispatch (rexnative_d3d_trace.cpp) hands the native path for each call: which guest
// function it is, its PPC arguments at entry and the tiling call it is nested in.
#pragma once

#include <cstdint>
#include "game_layer.h"  // this game's layer (rexnative adapt): addresses, offsets

namespace rexnative {

// Guest D3D functions the native path hears of (addresses in the base-disc default.xex; names
// from src/d3d_surface.inc). All of them flush the device's register shadow
// (d3d_state.h, IsStateFlushingFn); all but kRunCommandBuffer reach the renderer's On* calls.
enum class HookFn : uint32_t {
  kBeginVertices = rexnative::game::kHookBeginVertices,        // draw; the game writes the vertices after it returns
  kDrawVerticesUP = rexnative::game::kHookDrawVerticesUP,       // draw
  kDrawVertices = rexnative::game::kHookDrawVertices,         // draw
  kDrawIndexedVertices = rexnative::game::kHookDrawIndexedVertices,  // draw
  kDrawIndx2Inline = rexnative::game::kHookDrawIndx2Inline,      // draw, called only from inside the D3D library
  kDrawIndx2Split = rexnative::game::kHookDrawIndx2Split,       // draw, called only from inside the D3D library
  kClearF = rexnative::game::kHookClearF,
  kClear = rexnative::game::kHookClear,
  // Iron Brigade: the D3D's inner Resolve, called by the game directly (no public wrapper in its
  // LTCG D3D). EntryArgs (ib_d3d_trace.cpp) rearranges its arguments into the public signature.
  kResolve = rexnative::game::kHookResolve,
  kSwap = rexnative::game::kHookSwap,
  // fn_826D6610(dev, object, r5): flushes the dirty state the object's masks select (+24..+56,
  // one per dirty mask +0..+32; shader load included), then writes the packets of the object's
  // chunk list (+116) into the push buffer. [INFERENCE: the XDK's D3DDevice_RunCommandBuffer.]
  // PGR4's sub_826D6610 has no From Dust copy (tools/match_d3d.py): never matched.
  kRunCommandBuffer = rexnative::game::kHookRunCommandBuffer,
  // QueryIssue(query, flags) and QueryGetData(query, data, size, flags): the guest D3D's
  // Direct3DQuery Issue and GetData (occlusion.h).
  // Iron Brigade: not found in its LTCG D3D (tools/match_d3d_fd.py); never matched.
  kQueryIssue = rexnative::game::kHookQueryIssue,
  kQueryGetData = rexnative::game::kHookQueryGetData,
};

// The guest D3D's predicated tiling calls a hooked call can be nested in. BeginTiling
// (fn_820EC610) clears through ClearF with the largest tile's extent at the EDRAM base, meaning
// every tile; EndTiling (fn_820EC980) resolves each tile, its rectangle in surface pixels and
// the EDRAM holding the tile from its origin (the tile list at device+12740, render_targets.h).
enum class TilingCall : uint8_t { kNone, kBegin, kEnd };

// Raw PPC arguments of one hooked call. Integer arguments in r3..r10 and float arguments in
// f1..f4 are captured at entry, before the guest function runs (it clobbers them).
// Argument meaning follows the XDK D3DDevice_* signatures [INFERENCE, check against the
// tap traces' `C` lines before relying on one]:
//   DrawIndexedVertices(dev, prim, base_vertex, start_index, index_count)
//   DrawVertices(dev, prim, start_vertex, vertex_count)
//   DrawVerticesUP(dev, prim, vertex_count, data_ptr, stride)
//   BeginVertices(dev, prim, vertex_count, stride) -> r3 = pointer the game writes into
//   Clear(dev, count, rects_ptr, flags, color; z in f1, stencil in r9)
//   ClearF(dev, flags, rect_ptr, color_ptr; z in f1, stencil in r8)
//   (a float argument takes the next integer register's slot)
//   Resolve(dev, flags, src_rect_ptr, dest_texture, dest_point_ptr, level, slice,
//           clear_color_ptr; clear_z in f1, clear_stencil, params_ptr)
//   Swap(dev, front_buffer_texture, scaler_params_ptr)
struct HookArgs {
  HookFn fn;
  uint32_t device;  // r3 at entry: the guest D3D device (a guest virtual address)
  uint32_t r[8];    // r3..r10 at entry; r[0] == device
  uint32_t sp;      // r1 at entry: arguments past r10 are on the guest stack (Resolve's
                    // ClearStencil and pParameters; offsets in resolve.h)
  double f[4];      // f1..f4 at entry
  uint64_t ret;     // r3 after the guest function returned
  TilingCall tiling = TilingCall::kNone;  // the tiling call this one is nested in
};

}  // namespace rexnative
