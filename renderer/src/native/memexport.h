// ReXNative: memexport (Xenos shaders that write to memory through eA / eM).
//
// The draws run in draw.cpp (DrawRenderer, --rexnative_memexport). The ranges they write are
// also noted here (the first ones logged), and MemexportRangeOf says whether an address is in
// one, so the texture cache can log a texture read from memory a memexport draw writes.
#pragma once

#include <cstdint>

namespace rex::graphics {
class RegisterFile;
class Shader;
}  // namespace rex::graphics

namespace rexnative {

void NoteMemexportDraw(const rex::graphics::RegisterFile& regs, const rex::graphics::Shader& vs,
                       const rex::graphics::Shader* ps);
bool MemexportRangeOf(uint32_t address, uint32_t* base, uint32_t* bytes);

}  // namespace rexnative
