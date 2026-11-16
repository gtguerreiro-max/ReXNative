// ReXNative: memexport (Xenos shaders that write to memory through eA / eM).
//
// The draws run in draw.cpp (DrawRenderer, --rexnative_memexport). The ranges they write are
// also noted here (the first ones logged), and MemexportRangeOf says whether an address is in
// one, so the texture cache can log a texture read from memory a memexport draw writes.
#pragma once

#include <cstdint>

#include "native/sdk.h"

namespace rex::gpu {
class Shader;
}  // namespace rex::gpu

namespace rexnative {

void NoteMemexportDraw(const rex::graphics::RegisterFile& regs, const rex::gpu::Shader& vs,
                       const rex::gpu::Shader* ps);
bool MemexportRangeOf(uint32_t address, uint32_t* base, uint32_t* bytes);

}  // namespace rexnative
