// ReXNative: the recompiled code's spin wait hint, db16cyc (simde_mm_pause() in the code ReXGlue's
// codegen writes), made to sleep when a wait goes on. The XDK's D3D waits for the GPU and for the
// next frame in loops of db16cyc and a check (Jet Set Radio's Swap); spun out, a 30 FPS game keeps
// a core busy between its frames and the Steam Deck's battery pays for it. The project adds this
// header to the generated code's precompiled header, after ReXGlue's own (rexnative/project.py).
#pragma once

extern "C" void rexnative_spin_pause(void);

#define simde_mm_pause() rexnative_spin_pause()
