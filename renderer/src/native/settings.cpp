// ReXNative on ReXGlue v0.11: the renderer's cvars as settings (compat/rex/cvar.h).
#include <rex/cvar.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <rex/logging/log_category.h>
#include <rex/settings_registry.h>

#include "legacy_names.h"

namespace rexnative::settings {
namespace {

constexpr std::string_view kScope = "rexnative";
constexpr std::string_view kPrefix = "rexnative_";

// The old SDK's names for the settings the renderer and the HUD read or set.
constexpr std::pair<std::string_view, std::string_view> kSdkNames[] = {
    {"gpu_skip_draws", "gpu.xenos.skip_draws"},
    {"occlusion_query_external", "gpu.xenos.occlusion_query_external"},
    {"readback_resolve", "gpu.xenos.readback_resolve"},
    {"readback_memexport", "gpu.xenos.readback_memexport"},
    {"present_external", "gpu.xenos.present_external"},
    {"log_level", "log.level"},
    {"license_mask", "kernel.license_mask"},
    {"fullscreen", "ui.fullscreen"},
    {"window_width", "ui.window_width"},
    {"window_height", "ui.window_height"},
    {"vsync", "gpu.vsync"},
};

struct State {
  std::mutex mutex;
  std::vector<std::string> defined;  // old names of the renderer's settings
  std::vector<std::string> restart;  // full names of the .lifecycle(kRequiresRestart) ones
  bool registered = false;
  rex::cvar::FlagEntry info;  // GetFlagInfo's answer
};
State& GetState() {
  static State* state = new State;  // never destroyed: settings outlive static destructors
  return *state;
}

}  // namespace

rex::SettingOwner& Owner() {
  static rex::SettingOwner* owner = new rex::SettingOwner(kScope);
  return *owner;
}

std::string_view Define(std::string_view old_name) {
  {
    State& s = GetState();
    std::lock_guard lock(s.mutex);
    s.defined.emplace_back(old_name);
  }
  if (old_name == kScope) return "enabled";
  if (old_name.starts_with(kPrefix)) return old_name.substr(kPrefix.size());
  return old_name;
}

std::string FullName(std::string_view name) {
  if (name.find('.') != std::string_view::npos) return std::string(name);
  for (const auto& [old_name, full] : kSdkNames)
    if (name == old_name) return std::string(full);
  if (name == kScope) return std::string(kScope) + ".enabled";
  if (name.starts_with(kPrefix))
    return std::string(kScope) + "." + std::string(name.substr(kPrefix.size()));
  // A renderer setting defined without the prefix (see Define).
  State& s = GetState();
  std::lock_guard lock(s.mutex);
  if (std::ranges::find(s.defined, name) != s.defined.end())
    return std::string(kScope) + "." + std::string(name);
  return std::string(name);
}

void Register() {
  State& s = GetState();
  std::vector<std::string> defined;
  {
    std::lock_guard lock(s.mutex);
    if (s.registered) return;
    s.registered = true;
    defined = s.defined;
  }
  rex::SettingsRegistry& registry = rex::GetSettingsRegistry();
  if (auto r = registry.Register(Owner()); !r)
    REXLOG_ERROR("ReXNative: its settings did not register: {}", r.error().message);
  // The old names stay usable on the command line and in configuration files.
  for (const std::string& old_name : defined) {
    const std::string full = FullName(old_name);
    if (old_name != full) registry.AddAlias(old_name, full);
  }
  ForEachOldName([](std::string_view old_name, std::string_view new_name) {
    rex::GetSettingsRegistry().AddAlias(old_name, FullName(new_name));
  });
}

Modifier Modifier::lifecycle(Lifecycle l) const {
  if (l != Lifecycle::kHotReload && setting) {
    State& s = GetState();
    std::lock_guard lock(s.mutex);
    // Settings are defined at static initialisation, before their owner registers: the full
    // name is the scope and the name the setting was given.
    s.restart.push_back(setting->GetName());
  }
  return *this;
}

Modifier Modify(rex::SettingBase& setting) { return Modifier{&setting}; }

std::filesystem::path Path(std::string_view name) {
  auto found = rex::GetSettingsRegistry().Find(FullName(name));
  if (!found) return {};
  if (auto* path = dynamic_cast<rex::Setting<std::filesystem::path>*>(*found)) return path->Get();
  return std::filesystem::path((*found)->GetText());
}

bool RequiresRestart(std::string_view name) {
  const std::string full = FullName(name);
  State& s = GetState();
  std::lock_guard lock(s.mutex);
  return std::ranges::find(s.restart, full) != s.restart.end();
}

}  // namespace rexnative::settings

namespace rex::cvar {

std::string GetFlagByName(std::string_view name) {
  auto found = rex::GetSettingsRegistry().Find(rexnative::settings::FullName(name));
  return found ? (*found)->GetText() : std::string();
}

bool SetFlagByName(std::string_view name, std::string_view value) {
  return rex::GetSettingsRegistry()
      .SetText(rexnative::settings::FullName(name), value)
      .has_value();
}

const FlagEntry* GetFlagInfo(std::string_view name) {
  auto found = rex::GetSettingsRegistry().Find(rexnative::settings::FullName(name));
  if (!found) return nullptr;
  const rex::SettingBase& setting = **found;
  auto& s = rexnative::settings::GetState();
  std::lock_guard lock(s.mutex);
  FlagEntry& e = s.info;
  e.name = setting.GetName();
  e.description = setting.GetHelp();
  e.default_value = setting.GetDefaultText();
  e.category = "ReXNative";
  switch (setting.GetType()) {
    case rex::SettingType::kBool:
      e.type = FlagType::Boolean;
      break;
    case rex::SettingType::kI32:
    case rex::SettingType::kU32:
      e.type = FlagType::Int32;
      break;
    case rex::SettingType::kI64:
    case rex::SettingType::kU64:
      e.type = FlagType::Int64;
      break;
    case rex::SettingType::kF64:
      e.type = FlagType::Double;
      break;
    default:
      e.type = FlagType::String;
      break;
  }
  const bool restart =
      std::ranges::find(s.restart, e.name) != s.restart.end() ||
      setting.GetConstraints().life != rex::SettingLife::kLive;
  e.lifecycle = restart ? Lifecycle::kRequiresRestart : Lifecycle::kHotReload;
  return &e;
}

}  // namespace rex::cvar
