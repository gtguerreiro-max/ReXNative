# Roadmap

## Next

- Deus Ex: Human Revolution's disc 2: XamSwapDisc can't insert another disc yet (SDK patch
  0039 answers only for the running one).
- Fable II on the Steam Deck past its title screen, and Diablo III's replayed command buffers
  with the shader tracking of 2026-11-18 (neither tried yet).
- Midnight Club's races on the Steam Deck at frame latency 2: they fell to 22-29 FPS at 1.
- Find the HDR pass before the game's tone mapping, for DLSS 5.
- The open problems in [GAMES.md](GAMES.md#open-problems): Perfect Dark's broken levels,
  Spelunky's missing text.
- A reactive mask for the upscalers (FSR's own, made from the scene before and after its
  transparent draws), where motion vectors cannot be right: particles, and the draws whose mesh
  the previous frame did not have.
- Memexport: a game whose picture needs it, and no GPU wait when nothing on the CPU reads its
  results.
- Build a game end to end from `scripts\build-sdk.ps1` on a clean machine.
- Vulkan and native Linux (experimental, `vulkan-experiment`): a game builds and runs on the
  Steam Deck without Proton on ReXGlue's renderer; a Vulkan backend for ReXNative itself, its
  cost and what it would gain are in [LINUX.md](LINUX.md).
- Every new game becomes a reference and makes the next one easier.

## Done

- 2026-11-14: the renderer built from this repository for From Dust and Iron Brigade, and the
  scene surface learned at run time.
- 2026-11-15 and 16: recorded command buffers, memexport, pipelines compiled ahead, and thirteen
  more games end to end.
- 2026-11-16: the move to ReXGlue v0.11.0 and the Steam Deck on it.
- 2026-11-17: Midnight Club's cars and Dark Souls II, Jet Set Radio's motion vectors and camera
  cuts, inline draws found at run time; Overlord and Overlord II (their tiled G-buffers),
  tessellated patches, DLSS 5 in every game the tool prepares with a look per kind of game, one
  shared folder for the DLSS and FSR files, and the settings menu on a pad (Back + Start).
- 2026-11-18: the registers a function takes from its caller (`share_registers`), which drew
  Fable II's world black and garbled the XMV videos of Jet Set Radio, Syndicate and Dark Souls II.
  Fable II on the native renderer, now its default on the PC and the Steam Deck: its tone
  mapping, its DXT3A terrain, resolves into 1:5:5:5, the Resolve's level, and the shaders its own
  code loads between the hooks. The settings menu saves again, the Steam Deck runs at frame
  latency 2, and the README's details moved into `docs/`. Deus Ex: Human Revolution on the
  native renderer: the GPU-owned shader constants its own code loads (found by a probe of the
  push buffer between the hooks), and its Resolve's arguments from where the clear colour is.
