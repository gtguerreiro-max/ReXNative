// DLSS 5 looks: named sets of its settings, one per kind of game, chosen in the settings HUD
// (--rexnative_dlss5_look) instead of a dozen sliders, with the game's own pick as the default
// (python -m rexnative project writes it into the game's settings: rexnative/dlss5.py).
//
// The modding community's findings they follow (2026; the community forks' sliders, not presets):
// the Cinematic style over Natural, and two passes, as the best balance on older console games
// (XDA, PS2 games; racing games gained most); cel-shaded art only sharpened and saturated, its
// faces kept (the same test, Budokai Tenkaichi 3); a colour strength of 0 keeps the game's hues
// (the cure for the washed-out look most complain of), high colour flickers; intensity under 1.5;
// light transfer that suits bright exteriors overdoes dark interiors.
//   cinematic   the default for realistic games: Cinematic, 1 pass, the model's light and colour.
//   cinematic2  realistic games and racing, two passes at 0.8 of the size (the PS2 sweet spot).
//   natural     Natural style, two thirds of the model's colour: closer to the game's grade.
//   stylized    stylized 3D (cartoon, painterly, fantasy): weaker, the game's own hues kept.
//   cel         cel-shaded, anime and 2D art: gentle detail and light, flat colours kept, steadier.
//   strong      the most change: Default style, two passes.
//   custom      the separate --rexnative_dlss5_* settings as they are.
// --rexnative_dlss5_intensity scales a look's intensity (1: the look's own).
#pragma once

#include <cstdint>
#include <string_view>

namespace rexnative {

struct Dlss5Look {
  const char* id;
  const char* label;  // the HUD's
  int32_t style;      // 0 Default, 1 Natural, 2 Cinematic
  float intensity, structure, tone;
  float transfer, colour, stability;  // the scene stage's (neural_scene.cpp)
  int32_t passes;
  float scale;  // the model's working scale
};

inline constexpr Dlss5Look kDlss5Looks[] = {
    {"cinematic", "Cinematic", 2, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.5f, 1, 1.0f},
    {"cinematic2", "Cinematic, 2 passes", 2, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.5f, 2, 0.8f},
    {"natural", "Natural", 1, 1.0f, 1.0f, 1.0f, 1.0f, 0.65f, 0.5f, 1, 1.0f},
    {"stylized", "Stylized", 1, 0.75f, 0.8f, 0.7f, 0.6f, 0.0f, 0.6f, 1, 1.0f},
    {"cel", "Cel-shaded / 2D", 1, 0.65f, 0.8f, 0.4f, 0.4f, 0.0f, 0.75f, 1, 1.0f},
    {"strong", "Strong", 0, 1.25f, 1.25f, 1.25f, 1.25f, 1.0f, 0.5f, 2, 0.8f},
};

// The look named id; nullptr for "custom" or a name it does not know.
inline const Dlss5Look* FindDlss5Look(std::string_view id) {
  for (const Dlss5Look& l : kDlss5Looks)
    if (id == l.id) return &l;
  return nullptr;
}

// The settings DLSS 5 runs with now: the chosen look's (its intensity scaled by
// --rexnative_dlss5_intensity) or, for custom, the separate settings (neural.cpp).
struct Dlss5Values {
  int32_t style, passes;
  float intensity, structure, tone, transfer, colour, stability, scale;
};
Dlss5Values CurrentDlss5Values();

}  // namespace rexnative
