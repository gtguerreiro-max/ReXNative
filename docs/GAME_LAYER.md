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
| `kArgs<Name>[8]`, `kRemap<Name>` | the same for the hooked draws `DrawIndexedVertices`, `DrawVertices`, `DrawVerticesUP`, `BeginVertices`: LTCG builds drop and move their arguments (Iron Brigade's DrawIndexedVertices has no BaseVertexIndex and takes start and count in r5, r6; read in the wrong registers they made draws large enough to hang the GPU). The hooks' `PublicRegs` puts them in the public order for the renderer and the state tracker |
| `kFollowSwapDeviceOnly`, `kConfigureAtFirstDevice` | follow only the device the game presents with (a game that drives a second device while loading); configure the trace at the first device seen when CreateDevice is not hooked |
| `kSceneSurfaceMask`, `kSceneSurface`, `kSceneWidth`, `kScenePitch`, `kGameFrameHeight` | the scene the upscaler, the MSAA option, DLSS and DLSS 5 work on (RB_SURFACE_INFO pitch and MSAA, size), when it is measured (`rexnative scene`); the fallback otherwise |
| `REXNATIVE_SCENE_AUTO` | 1: the renderer learns the scene at run time (`renderer/src/native/scene.h`): the colour surface with MSAA or an HDR format resolved at nearly the frame's full size, earliest in the frame, adopted after 30 frames in a row and logged as `ReXNative scene: learned at frame ...`; the upscaler and the scene's targets are then made again for it. `adapt` writes 1 unless `overrides.json` gives a measured scene. Absent (older layers) = 0. `--rexnative_scene_auto=1` / `0` forces either at run time |
| `kDefaultEnabled` | whether ReXNative draws and presents the game without any option (the `rexnative` cvar's default; `--rexnative=false` / `true` overrides it) |
| `kDefaultRenderHeight`, `kDefaultOcclusion`, `kDefaultDisplayAspect` | defaults of `rexnative_render_height`, `rexnative_occlusion`, `rexnative_display_aspect` |

## overrides.json

```json
{
  "functions": {"CreateDevice": null, "RunCommandBuffer": "8264EF50"},
  "devices": {"follow_swap_device_only": true},
  "scene": {"auto": false, "kSceneSurfaceMask": 212991, "kSceneSurface": 66576, "width": 1024,
            "scaled_pitch": 1040, "height": 600},
  "clear_color": {"address": "82007F50", "vector": true},
  "resolve_args": [3, 4, 5, 6, 7, 8, 9, 10],
  "defaults": {"enabled": true, "render_height": 600, "occlusion": false}
}
```

## Game-side functions

Besides the layer, the renderer calls four functions the game may define (build with
`rexnative_add(... NO_DEFAULT_GLUE)`); `renderer/src/glue_defaults.cpp` has the values for a game
with nothing special: `RexNativeGame60Fps()` (the game's own 60 FPS option), `RexNativeGameStepNs()`
(its last simulation step, for DLSS 5's timing), `RexNativeGameWorldStepped()` and
`rex_xam_take_frame_pad()`.

## The HUD

`renderer/src/hud/hud.h`: the game's app calls `rexnative::hud::SetConfigPath(config)` in
`OnConfigurePaths`, `Initialize(window())` in `OnCreateDialogs` and `Shutdown()` in `OnShutdown`.
A game that hooks `XInputGetState` can pass each pad read through `FilterPad` for the
Back + Start chord; F1 always works. Settings are saved into the configuration file, keeping its
other lines and comments.

## The scene at run time

With `REXNATIVE_SCENE_AUTO 1` (or `--rexnative_scene_auto=1`) nothing about the scene needs
measuring: the renderer watches the game's resolves. Until it has seen the scene in 30 frames in
a row, no surface is treated as the scene (the upscaler and the MSAA option wait); menus and
loading screens without a 3D scene keep the last one. A game whose 3D scene has neither MSAA nor
an HDR format is not found this way: measure it (`rexnative scene`) and give it in
`overrides.json`. Still fixed when the renderer starts, from the layer's constants: frame
generation's largest render size (`frame_generation.cpp`), and with
`--rexnative_resolution_scale` above 1 the targets made before the scene is learned keep the
scale they were made with. The `rexnative_render_height` / `kGameFrameHeight` machinery (PGR4's Steam Deck frame height)
(`frame_height.h`, `d3d_state.cpp`) is unchanged.
