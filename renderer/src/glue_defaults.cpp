// ReXNative: the game-side functions the renderer calls, with the values for a game that has
// nothing special to tell it. A game that does (PGR4's 60 FPS option and car physics clock,
// From Dust's world stepping) builds with rexnative_add(... NO_DEFAULT_GLUE) and defines them.

#include <cstdint>

// Whether the game's own 60 FPS option is on (PGR4: its guest presentation interval ONE).
bool RexNativeGame60Fps() { return false; }

// The time of the game's last simulation step in ns (PGR4: its car physics), which tells DLSS 5
// gameplay from the front end and lifts the menu frame limit while the player drives
// (neural.cpp); 0: not playing now; ~0: the game does not tell (DLSS 5 runs everywhere, the frame
// limit stays).
uint64_t RexNativeGameStepNs() { return ~0ull; }

// The guest's pad reads in the last frame, for the renderer's frame CSV (PGR4Native's SDK patch
// 0025); 0 without that patch.
extern "C" uint64_t rex_xam_take_frame_pad() { return 0; }

// Whether the game's world stepped this frame (From Dust steps every other frame at 60 FPS), for
// draw.cpp's camera trace; true: every frame.
bool RexNativeGameWorldStepped() { return true; }
