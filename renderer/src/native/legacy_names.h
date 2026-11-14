// ReXNative: the options' old names (legacy_names.cpp).
#pragma once

#include <string_view>

namespace rexnative {

// The name an option had before ReXNative renamed it, or nullptr.
const char* OldNameOf(std::string_view new_name);

// Logs each old option name used in this run, with the new name and the value it set.
void LogOldNames();

}  // namespace rexnative
