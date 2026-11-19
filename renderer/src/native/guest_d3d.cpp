// ReXNative renderer: guest D3D functions replaced in primary mode (guest_d3d.h).
#include "guest_d3d.h"
#include "game_layer.h"  // this game's layer (rexnative adapt): addresses, offsets

#include "d3d_state.h"
#include "guest_memory.h"
#include "renderer.h"

#include <cstring>

REXCVAR_DEFINE_BOOL(rexnative_replace_draws, true, "ReXNative",
                    "With --rexnative, replace the guest D3D's DrawIndexedVertices "
                    "(DrawIndexedVertices): keep the device state, skip its PM4 packets");
REXCVAR_DECLARE(bool, rexnative);

// Without ShaderLoad in the layer (REXNATIVE_SHADER_LOAD_FOUND 0) there is nothing to call and
// the replacement stays off (sub_00000000 would not link). Layers before the macro have it.
#if !defined(REXNATIVE_SHADER_LOAD_FOUND) || REXNATIVE_SHADER_LOAD_FOUND
#define REXNATIVE_HAS_SHADER_LOAD 1
REX_EXTERN(REXNATIVE_SUB(REXNATIVE_SHADER_LOAD));
#else
#define REXNATIVE_HAS_SHADER_LOAD 0
#endif

namespace rexnative::guest_d3d {

// The original (DrawIndexedVertices(device, primitive, base vertex, start index, index count)):
//   1. flushes the dirty float constants (masks +0, +8, sub_826DE030), clears both masks;
//   2. mask +16 & kShaderLoadBits (d3d_state.h): ShaderLoad (shader load; the immediate,
//      patched VS copy comes from its call to ImmediateVs), which returns the new mask +16;
//      that mask & mask +40: sub_826DD8F8 (stream-only registers derived from it);
//   3. flushes the register blocks of masks +16, +24, +32 (sub_826DDC90, the fetch constants
//      sub_826DDED0, clip planes sub_826DDD50), clears the three masks;
//   4. writes VGT_INDX_OFFSET and DRAW_INDX (in pieces of at most 65535 indices), MakeSpace
//      (MakeSpace) when the push buffer is full.
// Steps 1, 3 and 4 and sub_826DD8F8 write only the push buffer and its cursor (device +48); the
// shadow is unchanged and the state tracker (d3d_state.cpp) reads the shadow and which dirty
// bits the call cleared. So the replacement runs step 2's shader load and clears the masks.
// With predicated tiling on (device byte +10940, bit 0) the original writes the draw packet in
// its predicated form and records its address in the tile list at +13232 for the per-tile
// replay; without a packet there is nothing to record, so the replacement covers both forms.
// The hook dispatch calls it only for the presenting device (rexnative_d3d_hooks.cpp,
// MayDropPackets): on a device that records a command buffer the packets it skips would be
// missing from the buffer RunCommandBuffer runs later (cmd_replay.h).
bool DrawIndexed(PPCContext& ctx, uint8_t* base) {
#if !REXNATIVE_HAS_SHADER_LOAD
  (void)ctx;
  (void)base;
  return false;
#else
  // Start-up flags: the renderer switch changes NativeRenderer::Live, not these.
  // The replacement reads the public signature's registers: not for a game whose
  // DrawIndexedVertices takes them elsewhere (game_layer.h kRemapDrawIndexedVertices).
  static const bool kReplace = !rexnative::game::kRemapDrawIndexedVertices &&
                               REXCVAR_GET(rexnative_replace_draws) && REXCVAR_GET(rexnative);
  const uint32_t device = ctx.r3.u32;
  if (!kReplace || !device || !NativeRenderer::Live()) return false;
  uint8_t* masks = HostPtr(base, device + dev::kDirtyMasks);
  // Same order as the original: the constant masks are cleared before the shader load (which
  // may dirty constants again), the register masks after it.
  std::memset(masks, 0, 16);
  const uint64_t shader = Load64(base, device + dev::kDirtyMasks + 16);
  if (shader & kShaderLoadBits) {
    // Volatile registers are the callee's, as in the original; r1 gets a frame below ours.
    const uint32_t sp = ctx.r1.u32;
    ctx.r1.u64 = sp - 256;
    ctx.r3.u64 = device;
    ctx.r4.u64 = shader;
    REXNATIVE_SUB(REXNATIVE_SHADER_LOAD)(ctx, base);
    ctx.r1.u64 = sp;
  }
  std::memset(masks + 16, 0, 24);  // masks +16, +24, +32
  return true;
#endif
}

}  // namespace rexnative::guest_d3d
