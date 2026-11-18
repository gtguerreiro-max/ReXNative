# How it works

A [ReXGlue](https://github.com/rexglue/rexglue-sdk) port runs a game's own PowerPC code
recompiled to x64, and draws it through an emulation of the Xbox 360 GPU (Xenos: command
buffers, EDRAM, resolves). **ReXNative draws the game with Direct3D 12 instead.** It hooks the
game's calls into the XDK's Direct3D library (draws, clears, resolves, Swap), reads the state the
game's D3D device keeps, and draws each call natively. That leaves room for what the console
never had: modern upscalers, MSAA, HDR, a settings menu over the game.

## What changes from game to game

One renderer serves every game. What differs between games, a few dozen addresses and structure
offsets, is found in the game's own recompiled code by a tool:

| What | How ReXNative finds it |
|---|---|
| The addresses of the XDK D3D functions it hooks (Swap, Draw*, Clear, Resolve, push-buffer segments, shader load, about 100 more for tracing) | matches the game's ReXGlue-generated code against a reference's |
| The offsets of the fields of the game's D3D device | votes from the aligned instructions of the matched functions |
| Which Resolve function to hook and where its arguments are (LTCG builds inline the wrapper and drop arguments) | follows each argument's uses in both listings |
| Resolve's default clear colour | its code pattern: one float (XDK 6132) or a 16-byte vector (XDK 20353) |
| The pad the HUD reads | the game's XInputGetState (the XDK's three-instruction wrapper, found by its shape) |
| The scene surface the upscaler works on | learned at run time from the game's resolves; measured once (`rexnative scene`) for a scene with neither MSAA nor an HDR format |
| Engine behaviour (a second D3D device, setting defaults) | your choice, in `overrides.json` |

The tool writes all of it into a header, `game_layer.h`, and a list of hooks, `d3d_surface.inc`
([GAME_LAYER.md](GAME_LAYER.md)). The renderer reads nothing else about the game.

## Matching a game against a reference

The matching works on the C++ that ReXGlue generates (every PowerPC instruction is kept there as
a comment), so it needs no disassembler. For each function of a reference it tries, in order of
confidence:

1. the same listing;
2. the same with every number masked;
3. the only such one inside the D3D library;
4. the function a matched one calls at the same place (the XREF idea of Cxbx-Reloaded's
   XbSymbolDatabase);
5. the neighbour of a matched one (the library keeps its object files' order);
6. the one that calls the same matched functions;
7. a similar listing;
8. a public wrapper the compiler inlined away.

The reference is chosen from the game's `default.xex`: the same D3D library version first, then
LTCG or not, then the closest XDK build; the other references fill gaps and cross-check.
Regenerating From Dust and Iron Brigade without their own layers gave the same hooks and device
offsets as the hand-made ones, and found mistakes in them
([validation](NOTES.md#validating-the-adapter)).

## What codegen needs besides

A recompiled game also needs things ReXGlue's codegen does not find alone, which `prepare` and
`project` supply as codegen hints: function starts reached only through pointers, jump tables
read short, the addresses of setjmp and longjmp, XAPI fibers, missing imports, and the functions
that take non-volatile registers from their caller ([NOTES.md](NOTES.md)).

## Repository layout

```
renderer/src/native/                  the renderer (the same for every game)
renderer/src/rexnative_d3d_hooks.cpp  the guest D3D hooks and frame trace
renderer/src/hud/                     the settings HUD
renderer/src/host.{h,cpp}             the renderer's side of the ReXGlue app: window, input, memory
renderer/src/glue_defaults.cpp        game-side functions, for a game with nothing special
renderer/rexnative.cmake              rexnative_add()
rexnative/                            the tool (python -m rexnative)
deck/                                 a game's scripts on the Steam Deck (python -m rexnative deck)
sdk/                                  the ReXGlue SDK the renderer needs: lock and patches
scripts/build-sdk.ps1                 fetch, patch, build and install that SDK
scripts/live.ps1, snaps.py            a running game for experiments, snapshots as images
scripts/rdoc.ps1, rdoc.py             RenderDoc capture queries
scripts/rdoc_trigger.py               a RenderDoc capture of a running game, without focus
scripts/deck_drive.py                 a game on the Steam Deck on a schedule (keys, captures, power)
docs/                                 the documentation (the README links each file)
```
