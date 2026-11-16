// ReXNative's connection to the ReXGlue v0.11 app: what the renderer and the HUD reached through
// rex::Runtime::instance() and the UI window before v0.11 (the game window, the UI thread, key
// and mouse input, guest memory, the XAM dialogs).
//
// The game's app (rexnative project writes it) calls Configure from OnConfigure, before the
// command line applies, and Attach from OnInit, once the window and the modules are up.
#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

#include <rex/input_keys.h>

namespace rex::app {
class App;
}
namespace rex::kernel {
class GuestAddressSpace;
}
namespace rex::ui {
class IUISystem;
}

namespace rexnative::host {

// OnConfigure: registers the renderer's settings and their old names, and, with
// rexnative.enabled, the SDK settings the native renderer needs (the Xenos driver keeps the GPU's
// fences and interrupts but skips its draws, its occlusion queries and its presents).
void Configure();
// OnInit: keeps the app's UI system and starts listening to the window's input.
void Attach(rex::app::App& app);
// OnShutdown.
void Detach();

rex::ui::IUISystem* UI();
// The game window's HWND, null before it exists.
void* Window();
// The game window's client size in pixels, 0 x 0 before it exists.
void WindowSize(uint32_t* width, uint32_t* height);

// Posts a function to the UI thread; false when there is no UI thread.
bool PostToUIThread(std::function<void()> fn);
// Runs a function on the UI thread and waits for it (at once on the UI thread).
bool RunOnUIThread(std::function<void()> fn);

// The guest's address space: the first guest thread that reaches a hook notes it (and the app's
// launch, when it can).
rex::kernel::GuestAddressSpace* Memory();
void NoteGuestThread();

// Whether an XAM dialog is on screen (the SDK's presenter draws it).
bool XamUIActive();

// Input from the game window, on the UI thread. A handler returns true when it consumed the event
// (the game does not see it). Higher `order` handlers see an event first.
using InputHandler = std::function<bool(rex::InputEvent&)>;
void AddInputHandler(const void* owner, int order, InputHandler handler);
void RemoveInputHandler(const void* owner);

// A key binding: `name` is the binding's setting (rexnative.<name without "bind_">, its old
// --bind_<...> name an alias), `default_key` a key name such as "F1". `pressed` runs on the UI
// thread once per press.
void RegisterBind(std::string_view name, std::string_view default_key, std::string_view help,
                  std::function<void()> pressed);
void UnregisterBind(std::string_view name);

}  // namespace rexnative::host
