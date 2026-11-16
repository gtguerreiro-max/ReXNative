// ReXNative on ReXGlue v0.11: the SDK's Xenos definitions the renderer uses, under the names it
// used before v0.11 (rex::graphics::xenos, reg, texture_util, XE_GPU_REG_*...).
//
// v0.11 moved the Xenos formats, registers, fetch constants, texture layouts and ucode into
// rex/xenos (namespace rex::xenos), and the GPU internals (the shader translator, the draw state
// helpers) into the SDK's private src/gpu/xenos, which the renderer builds from the SDK's source
// tree (native/shader_xlate.cmake).
#pragma once

#include <cstdint>
#include <utility>

#include <rex/bit.h>
#include <rex/math.h>
#include <rex/memory/endian.h>
#include <rex/memory/mapped_ptr.h>
#include <rex/types.h>
#include <rex/xenos/draw_hook.h>
#include <rex/xenos/fetch.h>
#include <rex/xenos/format.h>
#include <rex/xenos/integer_fetch.h>
#include <rex/xenos/register.h>
#include <rex/xenos/state.h>
#include <rex/xenos/texture.h>
#include <rex/xenos/ucode.h>

namespace rex::graphics {
// Every rex::xenos name as rex::graphics::<name> too (FormatInfo, GetBaseFormat...).
using namespace ::rex::xenos;
namespace xenos = ::rex::xenos;
namespace reg = ::rex::xenos::reg;
namespace texture_util = ::rex::xenos;
namespace ucode = ::rex::xenos::ucode;
using ::rex::xenos::AdjustIntegerTextureFetchConstants;
using ::rex::xenos::GetPixelConstantHook;
using ::rex::xenos::PixelConstantHook;
using ::rex::xenos::Register;
using ::rex::xenos::RegisterFile;
}  // namespace rex::graphics

// The register indices, as the old SDK's XE_GPU_REG_<name> constants.
#define REXGPU_REGISTER(index, type, name) inline constexpr uint32_t XE_GPU_REG_##name = index;
#include <rex/xenos/register_table.inc>
#undef REXGPU_REGISTER
