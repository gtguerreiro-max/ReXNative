// ReXNative renderer: the arguments of a hooked guest D3D call.
//
// What the hook dispatch (rexnative_d3d_trace.cpp) hands the native path for each call: which guest
// function it is, its PPC arguments at entry and the tiling call it is nested in.
#pragma once

#include <cstdint>
#include "game_layer.h"  // this game's layer (rexnative adapt): addresses, offsets

// What a layer generated before a function was known lacks: its constants, "not found". Qualified
// lookup (rexnative::game::kX) searches a namespace named by a using-directive only when the
// layer itself declares no kX, so a layer's own value always wins.
namespace rexnative::game_defaults {
inline constexpr uint32_t kFnBeginIndexedVertices = 0;
inline constexpr uint32_t kFnDrawIndexedVerticesUP = 0;
// Distinct HookFn values below 0x100 (the generator numbers its own sentinels from 1).
inline constexpr uint32_t kHookBeginIndexedVertices = 0xF0;
inline constexpr uint32_t kHookDrawIndexedVerticesUP = 0xF1;
// --rexnative_cmdbuf's default (cmd_replay.h): 0 off, 1 count, 2 replay recorded command buffers.
inline constexpr int32_t kDefaultCmdBuf = 0;
// The device's bin mask, low then high word, which the D3D's segment start (Iron Brigade's
// sub_826430B0) emits as SET_BIN_MASK_LO/HI with SET_BIN_SELECT = 0x00000000FFFFFFFF and which
// RunCommandBuffer restores after the buffer: the bin state a replay starts from. D3D 2.0.20353
// (Iron Brigade, From Dust); PGR4's 2.0.6132 has it at 12700.
inline constexpr uint32_t kCmdBufBinMaskOffset = 12956;
// Where a D3DBaseTexture keeps its fetch constant (6 dwords): +28 from D3D 2.0.4314 on (every
// layer before this constant); +16 in 2.0.2135, Geometry Wars: Retro Evolved's, whose Swap hands
// VdSwap texture + 16.
inline constexpr uint32_t kTextureFetchOffset = 28;
// The D3D's device and shader objects have a layout the layer cannot map (D3D 2.0.2135, older
// than any reference): the renderer takes the frames from the push buffer alone. No state is read
// from the device but its push pointer (dev::kPush); every hook but Swap, CreateDevice and the
// push buffer's own (they mark the stream's segments) runs the guest only, and the walk
// (rexnative.inline_draws, cmd_replay.h) draws, resolves and clears from the packets, as the
// GPU (and ReXGlue's Xenos plugin) would.
inline constexpr bool kStreamOnly = false;
}  // namespace rexnative::game_defaults
namespace rexnative::game {
using namespace rexnative::game_defaults;
}

namespace rexnative {

// Guest D3D functions the native path hears of (addresses in the base-disc default.xex; names
// from src/d3d_surface.inc). All of them flush the device's register shadow
// (d3d_state.h, IsStateFlushingFn); all but kRunCommandBuffer reach the renderer's On* calls
// themselves (kRunCommandBuffer's recorded draws do, through native/cmd_replay.h).
enum class HookFn : uint32_t {
  kBeginVertices = rexnative::game::kHookBeginVertices,        // draw; the game writes the vertices after it returns
  kDrawVerticesUP = rexnative::game::kHookDrawVerticesUP,       // draw
  // BeginIndexedVertices(dev, prim, base vertex, vertex count, index count, index format, stride,
  // ppIndexData; ppVertexData on the stack): reserves inline vertices and, after them, inline
  // indices in the push buffer; the game writes both after it returns. DrawIndexedVerticesUP
  // is BeginIndexedVertices + two copies + the push restore (Scaleform's draws in Iron Brigade:
  // the bitmap fills, its intro video among them).
  kBeginIndexedVertices = rexnative::game::kHookBeginIndexedVertices,  // draw
  kDrawIndexedVerticesUP = rexnative::game::kHookDrawIndexedVerticesUP,  // draw
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
  // RunCommandBuffer(dev, command buffer object) (PGR4's fn_826D6610, Iron Brigade's
  // sub_8264EF50; r5 is unused in Iron Brigade's LTCG build) [INFERENCE: the XDK's
  // D3DDevice_RunCommandBuffer]. Object layout in d3d_state.h (CommandBufferHeader). It flushes
  // dirty & inherit (+24..+56, one mask per dirty mask +0..+32; shader load included), emits one
  // INDIRECT_BUFFER per entry of the chunk list at +116 (into the push segment when flag 0x20 is
  // clear, straight into the ring when it is set), re-emits the window scissor unless keep
  // (+96) bits 46-48 are all set, and sets the dirty masks to ~keep (+64..+96). With
  // --rexnative_cmdbuf 2 the buffer's draws reach OnDraw with this fn and r[1..3] = the draw
  // initiator, VGT_DMA_BASE and VGT_DMA_SIZE (native/cmd_replay.h). From Dust has no copy
  // (tools/match_d3d.py): never matched.
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
// kInside: a call made between BeginTiling and EndTiling, which the D3D records once and the GPU
// runs for every tile (the device's tile count stays set after EndTiling, so it cannot tell).
enum class TilingCall : uint8_t { kNone, kBegin, kEnd, kInside };

// Raw PPC arguments of one hooked call. Integer arguments in r3..r10 and float arguments in
// f1..f4 are captured at entry, before the guest function runs (it clobbers them).
// Argument meaning follows the XDK D3DDevice_* signatures [INFERENCE, check against the
// tap traces' `C` lines before relying on one]:
//   DrawIndexedVertices(dev, prim, base_vertex, start_index, index_count)
//   DrawVertices(dev, prim, start_vertex, vertex_count)
//   DrawVerticesUP(dev, prim, vertex_count, data_ptr, stride)
//   BeginVertices(dev, prim, vertex_count, stride) -> r3 = pointer the game writes into
//   BeginIndexedVertices(dev, prim, base_vertex, vertex_count, index_count, index_format,
//                        stride, ppIndexData; ppVertexData at sp+84) -> r3 = 0 (S_OK)
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
  // Resolve: its ClearStencil when an LTCG build passes it in a register (game_layer.h
  // REXNATIVE_RESOLVE_STENCIL_REG: Overlord II's r10); -1: on the stack, where the public call has
  // it (sp + 92).
  int32_t stencil = -1;
};

}  // namespace rexnative
