// ReXNative: the scene surface and its detection (scene.h).
#include "scene.h"

#include "frame_height.h"
#include "game_layer.h"
#include "resolve.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <array>

// Layers written before scene detection existed have no REXNATIVE_SCENE_AUTO: their scene is the
// measured constants, as before.
#ifndef REXNATIVE_SCENE_AUTO
#define REXNATIVE_SCENE_AUTO 0
#endif

REXCVAR_DEFINE_INT32(rexnative_scene_auto, -1, "ReXNative",
                     "Learn the 3D scene's surface (for DLSS / DLAA / FSR and the MSAA option) from "
                     "the frames' resolves: 1 on, 0 off (game_layer.h's constants), -1 the game "
                     "layer's choice (REXNATIVE_SCENE_AUTO)");
REXCVAR_DEFINE_BOOL(rexnative_scene_late_draws, false, "ReXNative",
                    "Scene detection: an 8-bit surface without MSAA is the scene when 3D draws "
                    "went into it before any of its full-size resolves, not only before its first "
                    "(Iron Brigade resolves its scene's surface once after the depth prepass, "
                    "before the scene's colour draws)");

namespace rexnative {
namespace {

// A candidate in this many frames in a row is adopted (about half a second).
constexpr uint32_t kStableFrames = 30;
// Of the frame's largest colour resolve, the share of its area the scene covers at least (a scene
// rendered smaller and scaled up by the game is still found; bloom and shadow targets are not).
constexpr double kMinShare = 0.5;
constexpr uint32_t kMinWidth = 320;
constexpr uint32_t kSceneMask = 0x33FFF;  // RB_SURFACE_INFO surface_pitch and msaa_samples
// Draws with the depth test on before its first resolve that make an 8-bit surface without
// MSAA a scene (SceneNoteDraw).
constexpr uint32_t kMinDepthDraws = 16;

// RB_COLOR_INFO color_format (xenos::ColorRenderTargetFormat) with more range than 8 bits.
bool IsHdrFormat(uint32_t f) {
  switch (f) {
    case 3:   // k_2_10_10_10_FLOAT (7e3)
    case 5:   // k_16_16_16_16
    case 6:   // k_16_16_FLOAT
    case 7:   // k_16_16_16_16_FLOAT
    case 12:  // k_2_10_10_10_FLOAT_AS_16_16_16_16
    case 14:  // k_32_FLOAT
    case 15:  // k_32_32_FLOAT
      return true;
    default:
      return false;
  }
}

const char* FormatName(uint32_t f) {
  static constexpr const char* kNames[16] = {
      "k_8_8_8_8",       "k_8_8_8_8_GAMMA", "k_2_10_10_10",        "k_2_10_10_10_FLOAT",
      "k_16_16",         "k_16_16_16_16",   "k_16_16_FLOAT",       "k_16_16_16_16_FLOAT",
      "8",               "9",               "k_2_10_10_10_AS_10_10_10_10", "11",
      "k_2_10_10_10_FLOAT_AS_16_16_16_16", "13", "k_32_FLOAT", "k_32_32_FLOAT"};
  return kNames[f & 15];
}

// One colour surface resolved in the frame.
struct Seen {
  uint32_t key = 0;     // surface_info & kSceneMask
  uint32_t width = 0;   // the widest resolve that starts at x 0 (or its tiles side by side)
  uint32_t height = 0;  // the lowest resolve bottom (tiles cover the frame in bands)
  uint32_t format = 0;
  uint32_t order = 0;   // its first resolve's place in the frame
  uint32_t depth_draws = 0;  // depth-tested draws into it before that resolve
};

struct State {
  bool init = false;
  SceneSurface scene;
  std::array<Seen, 32> seen{};
  uint32_t seen_count = 0, resolves = 0;
  std::array<std::pair<uint32_t, uint32_t>, 16> depth_draws{};  // surface key, draws this frame
  uint32_t depth_draw_keys = 0;
  Seen pending{};
  uint32_t pending_frames = 0;
} g;

void Init() {
  if (g.init) return;
  g.init = true;
  if (SceneAuto()) {
    g.scene = {};  // nothing until learned
    REXLOG_INFO("ReXNative scene: learned at run time (game layer: {}, --rexnative_scene_auto={})",
                REXNATIVE_SCENE_AUTO ? "auto" : "measured", REXCVAR_GET(rexnative_scene_auto));
  } else {
    g.scene = {rexnative::game::kSceneSurfaceMask, rexnative::game::kSceneSurface,
               rexnative::game::kSceneWidth, 0, rexnative::game::kScenePitch, false};
  }
}

// The depth-tested draws into the surface so far this frame (SceneNoteDraw).
uint32_t DepthDraws(uint32_t key) {
  for (uint32_t i = 0; i < g.depth_draw_keys; ++i)
    if (g.depth_draws[i].first == key) return g.depth_draws[i].second;
  return 0;
}

bool Same(const Seen& a, const Seen& b) {
  return a.key == b.key && a.width == b.width && a.height == b.height;
}

}  // namespace

bool SceneAuto() {
  const int32_t v = REXCVAR_GET(rexnative_scene_auto);
  return v < 0 ? REXNATIVE_SCENE_AUTO != 0 : v != 0;
}

const SceneSurface& Scene() {
  Init();
  return g.scene;
}

uint32_t SceneWidth() {
  const SceneSurface& s = Scene();
  return s.learned ? s.width : uint32_t(rexnative::game::kSceneWidth);
}

uint32_t SceneHeight() {
  const SceneSurface& s = Scene();
  return s.learned ? s.height : FrameHeight();
}

uint32_t ScenePitch() {
  const SceneSurface& s = Scene();
  return s.learned ? s.pitch : uint32_t(rexnative::game::kScenePitch);
}

void SceneNoteResolve(const ResolveParams& p) {
  Init();
  if (!SceneAuto() || p.source >= 4) return;  // colour surfaces only
  const uint32_t order = g.resolves++;
  if (p.x1 <= 0 || p.y1 <= 0) return;
  // What the resolve copies: the rectangle, within its destination.
  const uint32_t w = uint32_t(std::min<int64_t>(p.x1, int64_t(p.dest_x) + p.dest_width));
  const uint32_t h = uint32_t(std::min<int64_t>(p.y1, int64_t(p.dest_y) + p.dest_height));
  const uint32_t key = p.regs.surface_info & kSceneMask;
  for (uint32_t i = 0; i < g.seen_count; ++i) {
    Seen& s = g.seen[i];
    if (s.key != key) continue;
    if (p.x0 > 0) {
      // Predicated tiles side by side (Tetris Splash's 1280x720 2x MSAA in two tiles 640
      // wide, of a surface of pitch 640): the next tile, from the top, reaching past the
      // surface's pitch, makes the frame wider. Any other resolve not at x 0 is a corner
      // (a rear-view mirror), not the frame.
      if (p.y0 <= 0 && uint32_t(p.x0) <= s.width && uint32_t(p.x1) > (key & 0x3FFF))
        s.width = std::max(s.width, w);
      return;
    }
    s.width = std::max(s.width, w);
    s.height = std::max(s.height, h);
    // --rexnative_scene_late_draws: the 3D draws before a later resolve of the surface count too.
    if (REXCVAR_GET(rexnative_scene_late_draws))
      s.depth_draws = std::max(s.depth_draws, DepthDraws(key));
    return;
  }
  if (p.x0 > 0) return;  // a corner of a surface: not the frame
  if (g.seen_count == g.seen.size()) return;
  g.seen[g.seen_count++] = {key, w, h, (p.regs.color_info[p.source & 3] >> 16) & 0xF, order,
                            DepthDraws(key)};
}

void SceneNoteDraw(uint32_t surface_info) {
  if (!SceneAuto()) return;
  const uint32_t key = surface_info & kSceneMask;
  for (uint32_t i = 0; i < g.depth_draw_keys; ++i) {
    if (g.depth_draws[i].first == key) {
      ++g.depth_draws[i].second;
      return;
    }
  }
  if (g.depth_draw_keys < g.depth_draws.size()) g.depth_draws[g.depth_draw_keys++] = {key, 1};
}

bool SceneEndFrame(uint64_t frame) {
  Init();
  if (!SceneAuto()) return false;
  uint64_t largest = 0;
  for (uint32_t i = 0; i < g.seen_count; ++i)
    largest = std::max(largest, uint64_t(g.seen[i].width) * g.seen[i].height);
  // The best candidate: MSAA first, then an HDR format, then 3D draws (SceneNoteDraw), then the
  // earliest in the frame.
  const Seen* best = nullptr;
  auto rank = [](const Seen& s) {
    const bool msaa = ((s.key >> 16) & 3) != 0;
    return std::array<int64_t, 4>{msaa ? 1 : 0, IsHdrFormat(s.format) ? 1 : 0,
                                  s.depth_draws >= kMinDepthDraws ? 1 : 0, -int64_t(s.order)};
  };
  for (uint32_t i = 0; i < g.seen_count; ++i) {
    const Seen& s = g.seen[i];
    const bool msaa = ((s.key >> 16) & 3) != 0;
    // The HUD's and composites' surfaces: 8-bit, single-sampled, no 3D draws before their resolve.
    if (!msaa && !IsHdrFormat(s.format) && s.depth_draws < kMinDepthDraws) continue;
    if (s.width < kMinWidth || double(s.width) * s.height < kMinShare * double(largest)) continue;
    if (!best || rank(s) > rank(*best)) best = &s;
  }
  g.seen_count = 0;
  g.resolves = 0;
  g.depth_draw_keys = 0;
  if (!best) {
    g.pending_frames = 0;  // no 3D scene this frame (a menu): keep the scene, start over
    return false;
  }
  if (g.pending_frames && Same(*best, g.pending)) {
    ++g.pending_frames;
  } else {
    g.pending = *best;
    g.pending_frames = 1;
  }
  const SceneSurface& now = g.scene;
  if (g.pending_frames < kStableFrames ||
      (now.learned && now.value == g.pending.key && now.width == g.pending.width &&
       now.height == g.pending.height))
    return false;
  g.scene = {kSceneMask, g.pending.key, g.pending.width, g.pending.height,
             g.pending.key & 0x3FFF, true};
  REXLOG_INFO("ReXNative scene: learned at frame {}: RB_SURFACE_INFO {:05X} (pitch {}, {}x MSAA), "
              "{}x{}, {}{}",
              frame, g.scene.value, g.scene.pitch,
              ((g.scene.value >> 16) & 3) == 0 ? 1 : ((g.scene.value >> 16) & 3) == 1 ? 2 : 4,
              g.scene.width, g.scene.height, FormatName(g.pending.format),
              IsHdrFormat(g.pending.format) ? " (HDR)" : "");
  return true;
}

}  // namespace rexnative
