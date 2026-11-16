// ReXNative on ReXGlue v0.11: the old SDK's cvar macros over v0.11 settings.
//
// ReXGlue v0.11 replaced its cvars with settings (rex/setting.h): typed members of a setting
// owner, named <scope>.<name>, set from the config file, the environment or --scope.name. The
// renderer keeps its REXCVAR_* definitions and reads; this header turns each into a
// rex::Setting of the one "rexnative" owner (native/settings.cpp): rexnative_<name> becomes
// rexnative.<name> (the master switch rexnative itself rexnative.enabled), and the old name stays
// an alias, so --rexnative_<name>=... and the configuration files keep working.
//
// rex::cvar::GetFlagByName / SetFlagByName take either name, and the old SDK names the renderer
// sets (gpu_skip_draws, occlusion_query_external, readback_resolve...) map to the v0.11 settings
// (native/settings.cpp, kSdkNames).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include <rex/setting.h>
#include <rex/types.h>

namespace rexnative::settings {

// The owner of every ReXNative setting (scope "rexnative"); never destroyed.
rex::SettingOwner& Owner();
// The setting name for an old cvar name ("rexnative_x" -> "x", "rexnative" -> "enabled"); also
// records the old name as an alias of rexnative.<name>. Static initialisation only.
std::string_view Define(std::string_view old_name);
// From the game's app (OnConfigure): registers the owner and the aliases with the registry.
void Register();
// The v0.11 full name for an old cvar name or a full name.
std::string FullName(std::string_view name);
// A path setting's value with its {scope.name} references resolved, empty when unknown.
std::filesystem::path Path(std::string_view name);

enum class Lifecycle : uint8_t { kHotReload, kRequiresRestart, kInitOnly };

// What `.lifecycle(...)` and `.range(...)` after a definition record (the settings HUD reads it).
struct Modifier {
  rex::SettingBase* setting;
  Modifier lifecycle(Lifecycle l) const;
  Modifier range(double, double) const { return *this; }
  Modifier allowed(std::initializer_list<const char*>) const { return *this; }
};
Modifier Modify(rex::SettingBase& setting);
// Whether a change applies only at the next start (a `.lifecycle(kRequiresRestart)` setting).
bool RequiresRestart(std::string_view name);

}  // namespace rexnative::settings

namespace rex::cvar {
using Lifecycle = ::rexnative::settings::Lifecycle;
enum class FlagType : uint8_t { Boolean, Int32, Int64, Double, String, Command };
struct FlagEntry {
  std::string name;
  FlagType type = FlagType::String;
  std::string description;
  std::string category;
  std::string default_value;
  Lifecycle lifecycle = Lifecycle::kHotReload;
};
// The value as text, empty for an unknown name.
std::string GetFlagByName(std::string_view name);
// Sets a value at the runtime layer; false for an unknown name or a refused value.
bool SetFlagByName(std::string_view name, std::string_view value);
// The flag's description, null for an unknown name. Valid until the next call.
const FlagEntry* GetFlagInfo(std::string_view name);
}  // namespace rex::cvar

#define REXNATIVE_DETAIL_SETTING(T, name, default_value, description)                     \
  ::rex::Setting<T> FLAGS_##name{&::rexnative::settings::Owner(),                         \
                                 ::rexnative::settings::Define(#name), (default_value),   \
                                 (description)};                                          \
  [[maybe_unused]] static const ::rexnative::settings::Modifier rexnative_modifier_##name = \
      ::rexnative::settings::Modify(FLAGS_##name)

#define REXCVAR_DEFINE_BOOL(name, default_value, category, description) \
  REXNATIVE_DETAIL_SETTING(bool, name, default_value, description)
#define REXCVAR_DEFINE_INT32(name, default_value, category, description) \
  REXNATIVE_DETAIL_SETTING(::rex::i32, name, default_value, description)
#define REXCVAR_DEFINE_INT64(name, default_value, category, description) \
  REXNATIVE_DETAIL_SETTING(::rex::i64, name, default_value, description)
#define REXCVAR_DEFINE_UINT32(name, default_value, category, description) \
  REXNATIVE_DETAIL_SETTING(::rex::u32, name, default_value, description)
#define REXCVAR_DEFINE_UINT64(name, default_value, category, description) \
  REXNATIVE_DETAIL_SETTING(::rex::u64, name, default_value, description)
#define REXCVAR_DEFINE_DOUBLE(name, default_value, category, description) \
  REXNATIVE_DETAIL_SETTING(::rex::f64, name, default_value, description)
#define REXCVAR_DEFINE_STRING(name, default_value, category, description) \
  REXNATIVE_DETAIL_SETTING(std::string, name, std::string(default_value), description)

#define REXCVAR_DECLARE(type, name) extern ::rex::Setting<type> FLAGS_##name
#define REXCVAR_GET(name) (FLAGS_##name.Get())
