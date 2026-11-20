// ReXNative renderer: native work that may read a bad guest address, run so that the fault does
// not end the game.
//
// The hooks read the guest D3D's objects at the offsets of the game's layer (rexnative adapt). A
// layer whose offsets do not fit the game's D3D (one older than every reference, an LTCG build
// that moved a field) reads a garbage pointer and dereferences it: Geometry Wars: Retro Evolved's
// first vertex shader object, read with PGR4's layout, crashed the game at its first draw. Guarded
// runs such work under a structured exception handler (Windows, and Wine on the Steam Deck); on an
// access violation the hooks stop the native path and hand the game to the Xenos plugin at its
// next Swap (NativeRenderer::NoteCrash), as the renderer switch does, instead of the crash report.
#pragma once

#include <cstdint>

namespace rexnative {

// Runs fn(ctx). False when it raised an access violation (its frames are left without their C++
// cleanups: the caller turns the native path off for good).
bool RunGuarded(void (*fn)(void*), void* ctx);
// The last fault RunGuarded caught: the address read or written, and where the code that did it
// is in the executable (an offset to look up in its PDB or map).
struct Fault {
  uint64_t address = 0, code_offset = 0;
  bool write = false;
};
Fault LastFault();

template <class F>
bool Guarded(F& f) {
  return RunGuarded([](void* p) { (*static_cast<F*>(p))(); }, &f);
}
template <class F>
bool Guarded(F&& f) {
  return Guarded(f);
}

}  // namespace rexnative
