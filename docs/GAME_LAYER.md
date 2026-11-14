# The game layer

Everything ReXNative's renderer knows about a game is in two generated files, found through the
include path (`rexnative_add(... LAYER_DIR ...)`):

- `game_layer.h`: constants in `namespace rexnative::game`;
- `d3d_surface.inc`: `D3D_FN(address, name, category)`, the guest D3D functions hooked for the
  frame trace and the profiler. Every function the renderer itself hooks is in it.

`python -m rexnative adapt` writes both, with a comment on each value saying how it was found
and how sure that is, and `report.md` beside them. Fix a value by hand in `overrides.json` in
the same folder and run `adapt` again, rather than editing the files.

## game_layer.h

| Name | What |
|---|---|
| `kGameName`, `kTitleId` | the game's name (the HUD's title) and title id |
| `kFn<Name>` | guest address of each XDK D3D function the renderer uses, 0 when the game has none: `Swap`, `Resolve` (the function hooked, public or inner), `ResolveInner`, `CreateDevice`, `StartSegment` / `SwitchSegment` / `MakeSpace` / `RestoreSegment*` / `RestorePush` (the push buffer), `BeginVertices`, `DrawVertices*`, `DrawIndexedVertices`, `DrawIndx2Inline` / `Split` (the library's internal draws), `Clear`, `ClearF`, `BeginTiling` / `EndTiling`, `QueryIssue` / `QueryGetData` (occlusion), `RunCommandBuffer`, `ShaderLoad`, `ImmediateVs` |
| `kHook<Name>` | the values of `HookFn` (hook_args.h): the address, or a distinct number below 0x100 for a function the game lacks |
| `REXNATIVE_SHADER_LOAD`, `REXNATIVE_IMMEDIATE_VS` (+ `_FOUND`), `REXNATIVE_SUB()`, `REXNATIVE_IMP()` | the two functions hooked by symbol (`sub_XXXXXXXX` tokens) |
| `dev::`, `shadow::`, `rt::`, `occ::` | offsets of the guest D3D device's fields: push buffer, register shadow, shaders, index buffer, render target surfaces, predicated tiling, fences |
| `kResolveDefaultClearColor`, `kResolveDefaultClearIsVector` | the constant Resolve clears with when `pClearColor` is null: one float splatted (XDK 6132) or a 16-byte vector (XDK 20353) |
| `kResolveArgRegs[8]`, `kResolveRemapArgs` | for each public Resolve argument r3..r10, the register it is in at the hooked function's entry (0: a constant 0); remapped only when they differ |
| `kFollowSwapDeviceOnly`, `kConfigureAtFirstDevice` | follow only the device the game presents with (a game that drives a second device while loading); configure the trace at the first device seen when CreateDevice is not hooked |
| `kSceneSurfaceMask`, `kSceneSurface`, `kSceneWidth`, `kScenePitch`, `kGameFrameHeight` | the scene the upscaler, DLSS and DLSS 5 work on (RB_SURFACE_INFO pitch and MSAA, size): measured with `rexnative scene` |
| `kDefaultRenderHeight`, `kDefaultOcclusion`, `kDefaultDisplayAspect` | defaults of `pgr4_render_height`, `pgr4_native_occlusion`, `pgr4_native_display_aspect` |

## overrides.json

```json
{
  "functions": {"CreateDevice": null, "RunCommandBuffer": "8264EF50"},
  "devices": {"follow_swap_device_only": true},
  "scene": {"kSceneSurfaceMask": 212991, "kSceneSurface": 66576, "width": 1024,
            "scaled_pitch": 1040, "height": 600},
  "clear_color": {"address": "82007F50", "vector": true},
  "resolve_args": [3, 4, 5, 6, 7, 8, 9, 10],
  "defaults": {"pgr4_render_height": 600, "pgr4_native_occlusion": false}
}
```

## Game-side functions

Besides the layer, the renderer calls four functions the game may define (build with
`rexnative_add(... NO_DEFAULT_GLUE)`); `renderer/src/glue_defaults.cpp` has the values for a game
with nothing special: `Pgr4Enable60Fps()` (the game's own 60 FPS option), `Pgr4LastCarStepNs()`
(its last simulation step, for DLSS 5's timing), `FromDustWorldStepped()` and
`rex_xam_take_frame_pad()`.

## The HUD

`renderer/src/hud/hud.h`: the game's app calls `rexnative::hud::SetConfigPath(config)` in
`OnConfigurePaths`, `Initialize(window())` in `OnCreateDialogs` and `Shutdown()` in `OnShutdown`.
A game that hooks `XInputGetState` can pass each pad read through `FilterPad` for the
Back + Start chord; F1 always works. Settings are saved into the configuration file, keeping its
other lines and comments.
