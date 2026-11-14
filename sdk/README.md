# The ReXGlue SDK for ReXNative

ReXNative's renderer builds against the ReXGlue SDK with a fixed set of patches: upstream ReXGlue
at the commit in `rexglue.lock`, then `patches/pgr4native/` and `patches/rexnative/` in file name
order. `scripts/build-sdk.ps1` fetches, patches, builds and installs it, and checks out the AMD
FidelityFX SDK at `fidelityfx.lock`; it prints the paths to give `rexnative_add()`.

This is the SDK From Dust ([FromDustDeck](https://github.com/gtguerreiro-max/FromDustDeck)) and
Iron Brigade run with: the patched tree is identical, file for file, to the one both ports were
built and tested with (checked 2026-11-14: all 39 patches apply in order to `bd833a2`, and the
result equals FromDustDeck's `build/rexglue-sdk`, submodules aside).

| Part | What | From |
|---|---|---|
| `rexglue.lock` | ReXGlue `development` at `bd833a2` | [rexglue-sdk](https://github.com/rexglue/rexglue-sdk) |
| `patches/pgr4native/` (37) | The generic patches of pgr4-native's set: codegen, kernel, threading, memory and audio fixes, Steam Deck pacing and wake-ups, and the SDK side of a host renderer: `gpu_skip_draws` (0031), the gamma ramp accessors (0030), `occlusion_query_external` (0049), motion-vector shader modifications (0050), and more | [pgr4-native](https://github.com/shaunsingh/pgr4-native) `patches/`, byte for byte (its PGR4-only patches are left out) |
| `patches/rexnative/0001-renderer-support.patch` | From Dust's port's SDK changes (30 files, 38 KB): the per-draw pixel constant hook a host renderer calls (`rex/graphics/draw_hook.h`), integer texture fetches folded into `exp_adjust` (`rex/graphics/integer_fetch.h`), headers-only FidelityFX for FSR 1 and clang-cl warnings in its build, codegen and memory fixes | FromDustDeck `patches/fromdust/0001-fromdust-sdk-changes.patch`, the source files |
| `patches/rexnative/0002-resolve-shaders-honour-the-destination-number-format.patch` | The 20 prebuilt `resolve_full_*` compute shaders (D3D12 DXBC and Vulkan SPIR-V headers, 16 MB of hex text, ~1.6 MB compressed) rebuilt from a newer Xenia's sources, which honour the destination number format: signed resolves (From Dust's height maps) kept their negative values | the same patch, the bytecode files; ReXGlue ships these shaders prebuilt and has no sources for them, so they cannot be rebuilt here |

The two `rexnative/` patches are FromDustDeck's single patch split by file (disjoint files, the
same result). `xam_keyboard_accept_default` (pgr4-native's 0024) is not in the set: the renderer
logs that it could not set it, which is harmless.

## Licenses

ReXGlue is BSD-3-Clause, with code from Xenia (BSD-3-Clause). Several pgr4-native patches port
code from Xenia Canary (BSD-3-Clause; each names the Canary commit and files), and the resolve
shaders of `0002` are built from Xenia's sources; pgr4-native's patches are MIT
([THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)). No game data is in any of them.

## Updating

To move to another ReXGlue commit: change `rexglue.lock`, run `scripts\build-sdk.ps1 -NoBuild`
into a fresh `-Dest`, and fix the patches that no longer apply. From Dust's port regenerates its
own patch from a working SDK tree with its `tools/make_sdk_patch.sh`.
