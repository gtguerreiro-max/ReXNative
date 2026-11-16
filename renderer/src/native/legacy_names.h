// ReXNative: the options' old names (legacy_names.cpp).
#pragma once

#include <string_view>

namespace rexnative {

// The name an option had before ReXNative renamed it, or nullptr.
const char* OldNameOf(std::string_view new_name);

// Each old name with the (old style) name of the option that replaced it.
void ForEachOldName(void (*fn)(std::string_view old_name, std::string_view new_name));

// Nothing since ReXGlue v0.11: the settings registry notes each old name used.
void LogOldNames();

}  // namespace rexnative
