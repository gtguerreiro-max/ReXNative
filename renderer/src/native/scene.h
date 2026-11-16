// ReXNative: the 3D scene's surface, which the upscaler (DLSS / DLAA / FSR), the MSAA override
// (--rexnative_msaa) and DLSS 5 work on, and its detection at run time.
//
// A game's scene is the colour surface it draws its 3D frame into before post-processing and the
// HUD: RB_SURFACE_INFO's pitch and MSAA identify it (the guest's value, whatever the host
// targets' sample count), and its size is the frame's. game_layer.h gives it as constants
// (kSceneSurface, kSceneWidth, kGameFrameHeight, kScenePitch), measured per game. With
// REXNATIVE_SCENE_AUTO in the layer (python -m rexnative adapt writes it when the scene was not
// measured), or --rexnative_scene_auto=1, the renderer learns it instead, from the frames'
// resolves: the colour surface with MSAA or an HDR format that is resolved at (nearly) the full
// size of the largest colour resolve of the frame, the earliest such in the frame (the
// post-processing and HUD composites come after it, single-sampled and 8-bit), the same choice as
// the tool's `rexnative scene` makes from a resolve dump. A candidate seen in kStableFrames frames
// in a row is adopted (menus and loading screens without a 3D scene keep the last one), logged,
// and the renderer then makes its upscaler and scene targets again for it
// (NativeRenderer::BeginFrameModules).
//
// Until a scene is learned, auto mode matches no surface: the upscaler and the MSAA override
// wait for it rather than act on a guess.
//
// All on the thread that records the frames.
#pragma once

#include <cstdint>

namespace rexnative {

struct ResolveParams;

struct SceneSurface {
  uint32_t mask = 0;     // RB_SURFACE_INFO bits that identify the scene (pitch and MSAA)
  uint32_t value = 0;    // their value; mask 0: no scene (auto mode, nothing learned yet)
  uint32_t width = 0;    // guest pixels
  uint32_t height = 0;
  uint32_t pitch = 0;    // RB_SURFACE_INFO surface pitch (the screen-sized surfaces')
  bool learned = false;  // detected at run time, not the layer's constants
};

// Whether the scene is learned at run time: --rexnative_scene_auto (1 on, 0 off), or the layer's
// REXNATIVE_SCENE_AUTO when the cvar is -1 (the default).
bool SceneAuto();

// The scene now: the layer's constants, or the learned one in auto mode (nothing before).
const SceneSurface& Scene();
inline bool IsSceneSurface(uint32_t surface_info) {
  const SceneSurface& s = Scene();
  return s.mask != 0 && (surface_info & s.mask) == s.value;
}
uint32_t SceneWidth();   // the layer's kSceneWidth until a scene is learned
uint32_t SceneHeight();  // FrameHeight() until a scene is learned
uint32_t ScenePitch();   // the layer's kScenePitch until a scene is learned

// Detection (auto mode only; calls are no-ops otherwise). NoteResolve for each guest resolve the
// renderer records; EndFrame between frames, true when a new scene was adopted.
void SceneNoteResolve(const ResolveParams& p);
// Each draw that writes colour with the depth test on, by its surface (RB_SURFACE_INFO): a
// surface without MSAA or an HDR format is still the scene when 3D draws went into it before its
// full-size resolve (Minecraft's 8-bit 1280x720), where the HUD's and menus' draws test no depth.
void SceneNoteDraw(uint32_t surface_info);
bool SceneEndFrame(uint64_t frame);

}  // namespace rexnative
