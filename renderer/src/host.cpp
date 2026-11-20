// ReXNative's connection to the ReXGlue v0.11 app (host.h).
#include "host.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <rex/app/app.h>
#include <rex/cvar.h>
#include <rex/kernel/heap.h>
#include <rex/kernel/thread_state.h>
#include <rex/logging.h>
#include <rex/logging/log_category.h>
#include <rex/platform/native_window.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/ui_system.h>
#include <rex/xam/ui.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

REXCVAR_DEFINE_STRING(bind_rexnative_hud, "F1", "ReXNative",
                      "Key of the settings menu (the pad: Back+Start)");
REXCVAR_DEFINE_STRING(bind_renderer_switch, "F9", "ReXNative",
                      "Key that switches between the native renderer and ReXGlue's");
REXCVAR_DEFINE_STRING(bind_upscaler, "F7", "ReXNative", "Key that turns the upscaler on and off");
REXCVAR_DEFINE_STRING(bind_upscaler_debug, "F8", "ReXNative",
                      "Key that shows the upscaler's inputs");
REXCVAR_DECLARE(bool, rexnative);

namespace rexnative::host {
namespace {

struct Handler {
  const void* owner;
  int order;
  InputHandler fn;
};

struct Bind {
  std::string name;
  rex::VirtualKey key = rex::VirtualKey::kNone;
  rex::KeyModifiers modifiers = rex::KeyModifiers::kNone;
  std::function<void()> pressed;
};

struct State : rex::ui::IUIEventListener {
  std::mutex mutex;
  rex::app::App* app = nullptr;
  rex::ui::IUISystem* ui = nullptr;
  rex::ScopeGuard listening;
  std::vector<Handler> handlers;  // highest order first
  std::vector<Bind> binds;
  std::atomic<rex::kernel::GuestAddressSpace*> memory{nullptr};
  HWND window = nullptr;

  void OnUIEvent(const rex::ui::UIEvent&) override {}
  void OnInputEvent(rex::InputEvent& event) override;
};
State& GetState() {
  static State* state = new State;  // the UI thread may outlive static destructors
  return *state;
}

// "F1", "Ctrl+F9", "A", "1": the key and its modifiers; kNone for an unknown name.
bool ParseKey(std::string_view text, rex::VirtualKey* key, rex::KeyModifiers* modifiers) {
  *key = rex::VirtualKey::kNone;
  *modifiers = rex::KeyModifiers::kNone;
  std::string rest(text);
  auto take = [&](std::string_view prefix, rex::KeyModifiers m) {
    if (rest.size() > prefix.size() &&
        std::equal(prefix.begin(), prefix.end(), rest.begin(),
                   [](char a, char b) { return std::tolower(a) == std::tolower(b); })) {
      rest.erase(0, prefix.size());
      *modifiers = *modifiers | m;
      return true;
    }
    return false;
  };
  while (take("ctrl+", rex::KeyModifiers::kCtrl) || take("shift+", rex::KeyModifiers::kShift) ||
         take("alt+", rex::KeyModifiers::kAlt)) {
  }
  if (rest.size() >= 2 && (rest[0] == 'F' || rest[0] == 'f')) {
    const int n = std::atoi(rest.c_str() + 1);
    if (n >= 1 && n <= 24) {
      *key = rex::VirtualKey(uint16_t(VK_F1 + n - 1));
      return true;
    }
  }
  if (rest.size() == 1 && std::isalnum(static_cast<unsigned char>(rest[0]))) {
    *key = rex::VirtualKey(uint16_t(std::toupper(static_cast<unsigned char>(rest[0]))));
    return true;
  }
  return false;
}

void State::OnInputEvent(rex::InputEvent& event) {
  std::vector<Handler> current;
  std::vector<std::function<void()>> pressed;
  {
    std::lock_guard lock(mutex);
    current = handlers;
    if (event.type == rex::InputEvent::Type::kKeyDown && !event.repeat) {
      for (const Bind& b : binds)
        if (b.key == event.key && b.modifiers == event.modifiers) pressed.push_back(b.pressed);
    }
  }
  for (Handler& h : current) {
    if (h.fn(event)) {
      event.handled = true;
      return;
    }
  }
  for (auto& fn : pressed) {
    fn();
    event.handled = true;
  }
}

}  // namespace

void Configure() { settings::Register(); }

void Attach(rex::app::App& app) {
  // ReXNative presents from the first frame: the Xenos driver's present thread then never puts
  // its swap chain on the window, so the window has one swap chain from the start. Taken over
  // at the first D3D call instead, the driver released its swap chain while ReXNative's already
  // presented into the window, and under vkd3d-proton that release could block the driver's GPU
  // thread: Double Dragon Neon and Minecraft stopped at their first frames on the Steam Deck,
  // now and then, on a first start.
  if (REXCVAR_GET(rexnative) && !rex::cvar::SetFlagByName("present_external", "true"))
    REXLOG_ERROR("ReXNative: could not set gpu.xenos.present_external (sdk/patches missing?)");
  State& s = GetState();
  rex::ui::IUISystem* ui = &app.GetUI();
  {
    std::lock_guard lock(s.mutex);
    s.app = &app;
    s.ui = ui;
  }
  if (ui->IsHeadless()) return;
  s.listening = ui->AddEventListener(s);
}

void Detach() {
  State& s = GetState();
  rex::ScopeGuard listening;
  {
    std::lock_guard lock(s.mutex);
    listening = std::move(s.listening);
    s.ui = nullptr;
    s.app = nullptr;
    s.window = nullptr;
  }
}

rex::ui::IUISystem* UI() {
  State& s = GetState();
  std::lock_guard lock(s.mutex);
  return s.ui;
}

void* Window() {
  State& s = GetState();
  rex::ui::IUISystem* ui;
  {
    std::lock_guard lock(s.mutex);
    if (s.window) return s.window;
    ui = s.ui;
  }
  if (!ui || ui->IsHeadless() || !ui->HasWindow()) return nullptr;
  auto native = ui->GetNativeWindow();
#if defined(REXNATIVE_LINUX)
  // Linux: DXVK Native's DXGI takes the game's SDL_Window as an HWND.
  if (!native || !native->sdl) return nullptr;
  std::lock_guard lock(s.mutex);
  s.window = static_cast<HWND>(native->sdl);
#else
  if (!native || native->type != rex::NativeWindow::Type::kWin32) return nullptr;
  std::lock_guard lock(s.mutex);
  s.window = static_cast<HWND>(native->window);
#endif
  return s.window;
}

void WindowSize(uint32_t* width, uint32_t* height) {
  *width = *height = 0;
  RECT r;
  if (HWND w = static_cast<HWND>(Window()); w && GetClientRect(w, &r)) {
    *width = uint32_t(std::max<LONG>(0, r.right - r.left));
    *height = uint32_t(std::max<LONG>(0, r.bottom - r.top));
  }
}

bool PostToUIThread(std::function<void()> fn) {
  rex::ui::IUISystem* ui = UI();
  if (!ui) return false;
  ui->PostTask([fn = std::move(fn)]() mutable { fn(); });
  return true;
}

bool RunOnUIThread(std::function<void()> fn) {
  rex::ui::IUISystem* ui = UI();
  if (!ui) return false;
  if (ui->IsUIThread()) {
    fn();
    return true;
  }
  return ui->RunSynchronous([fn = std::move(fn)]() mutable { fn(); });
}

rex::kernel::GuestAddressSpace* Memory() {
  State& s = GetState();
  if (auto* m = s.memory.load(std::memory_order_acquire)) return m;
  NoteGuestThread();
  return s.memory.load(std::memory_order_acquire);
}

void NoteGuestThread() {
  State& s = GetState();
  if (s.memory.load(std::memory_order_relaxed)) return;
  if (auto* thread = rex::kernel::ThreadState::Get(); thread && thread->GetAddressSpace())
    s.memory.store(thread->GetAddressSpace(), std::memory_order_release);
}

bool XamUIActive() { return rex::xam::XAMIsUIActive(); }

void AddInputHandler(const void* owner, int order, InputHandler handler) {
  State& s = GetState();
  std::lock_guard lock(s.mutex);
  s.handlers.push_back({owner, order, std::move(handler)});
  std::stable_sort(s.handlers.begin(), s.handlers.end(),
                   [](const Handler& a, const Handler& b) { return a.order > b.order; });
}

void RemoveInputHandler(const void* owner) {
  State& s = GetState();
  std::lock_guard lock(s.mutex);
  std::erase_if(s.handlers, [owner](const Handler& h) { return h.owner == owner; });
}

void RegisterBind(std::string_view name, std::string_view default_key, std::string_view help,
                  std::function<void()> pressed) {
  (void)help;
  std::string text = rex::cvar::GetFlagByName(name);
  if (text.empty()) text = std::string(default_key);
  Bind b;
  b.name = std::string(name);
  b.pressed = std::move(pressed);
  if (!ParseKey(text, &b.key, &b.modifiers)) {
    REXLOG_WARN("ReXNative: --{}={} is not a key (F1...F24, A...Z, 0...9, with Ctrl+ / Shift+ / "
                "Alt+); {} stays unbound",
                name, text, name);
    return;
  }
  State& s = GetState();
  std::lock_guard lock(s.mutex);
  std::erase_if(s.binds, [&](const Bind& x) { return x.name == name; });
  s.binds.push_back(std::move(b));
}

void UnregisterBind(std::string_view name) {
  State& s = GetState();
  std::lock_guard lock(s.mutex);
  std::erase_if(s.binds, [&](const Bind& x) { return x.name == name; });
}

}  // namespace rexnative::host
