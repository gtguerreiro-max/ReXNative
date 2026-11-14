// ReXNative renderer: guest D3D functions replaced in primary mode
// (--rexnative_replace_draws).
//
// With --rexnative nothing draws the guest's PM4 stream (the Xenos plugin skips all
// draws), so the guest D3D's packet building for draws is wasted work. These replacements keep
// the guest device in the state the original leaves it in for everything that is read later
// (the register shadow, the dirty masks, the shader load that produces the patched vertex
// shader copy for the native draw) and skip the packets. Ring kicks, fences, Swap, Clear and
// Resolve stay on the original path.
#pragma once

#include <rex/cvar.h>
#include <rex/hook.h>

#include <cstdint>

REXCVAR_DECLARE(bool, rexnative_replace_draws);

namespace rexnative::guest_d3d {

// DrawIndexedVertices (DrawIndexedVertices): runs the replacement and returns true, or returns false
// when the original must run (flag off, not primary, no native renderer drawing).
bool DrawIndexed(PPCContext& ctx, uint8_t* base);

}  // namespace rexnative::guest_d3d
