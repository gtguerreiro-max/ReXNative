// ReXNative's HUD: the player settings menu over the running game, for any game the renderer
// runs (from From Dust's port, after PGR4Native's in-game Options page). It is a menu over the
// cvars that give each setting; F1 or the pad's Back+Start opens it and the native renderer
// draws it (native/overlay.h) with an ImGui context of its own. A setting the renderer can change
// while it runs is applied at once; one it takes at its start is kept until the next one. Both
// are saved into the game's configuration file, keeping its other lines and comments. The title
// is the game's name from game_layer.h.
//
// The game's app calls SetConfigPath (OnConfigurePaths), Initialize (OnCreateDialogs, UI thread)
// and Shutdown. The pad reaches it through FilterPad, from ReXNative's XInputGetState hook (the
// layer's REXNATIVE_XINPUT_GET_STATE, rexnative_d3d_hooks.cpp) or the port's own.
#pragma once

#include <cstdint>
#include <filesystem>

namespace rex::ui {
class Window;
}

namespace rexnative::hud {

// From the game's app: the configuration file (OnConfigurePaths) and the window
// (OnCreateDialogs, on the UI thread; registers the key and the input listener).
void SetConfigPath(const std::filesystem::path& config);
void Initialize(rex::ui::Window* window);
void Shutdown();

// Whether the menu is up. Any thread.
bool MenuOpen();

// From the game's XInputGetState hook, if it has one, for each pad read: `pad` is the
// XINPUT_GAMEPAD the guest gets (big-endian: buttons, triggers, sticks; 12 bytes). Watches for
// the Back+Start chord (a lone Back or Start reaches the game a moment late) and feeds the menu;
// while the menu is up the pad is cleared and true is returned.
bool FilterPad(uint32_t user, uint8_t* pad);

}  // namespace rexnative::hud
