# Roadmap

## Next

- Find the HDR pass before the game's tone mapping, for DLSS 5.
- The open problems in [GAMES.md](GAMES.md#open-problems): Perfect Dark's broken levels,
  Spelunky's missing text, Halo's dark and striped surfaces, the WMV videos of Dark Souls II and
  Jet Set Radio.
- A reactive mask for the upscalers (FSR's own, made from the scene before and after its
  transparent draws), where motion vectors cannot be right: particles, and the draws whose mesh
  the previous frame did not have.
- Memexport: a game whose picture needs it, and no GPU wait when nothing on the CPU reads its
  results.
- Build a game end to end from `scripts\build-sdk.ps1` on a clean machine.
- Every new game becomes a reference and makes the next one easier.

## Done

- 2026-11-14: the renderer built from this repository for From Dust and Iron Brigade, and the
  scene surface learned at run time.
- 2026-11-15 and 16: recorded command buffers, memexport, pipelines compiled ahead, and thirteen
  more games end to end.
- 2026-11-16: the move to ReXGlue v0.11.0 and the Steam Deck on it.
- 2026-11-17: Midnight Club's cars and Dark Souls II, Jet Set Radio's motion vectors and camera
  cuts, inline draws found at run time.
