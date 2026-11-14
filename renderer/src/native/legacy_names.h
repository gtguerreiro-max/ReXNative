// ReXNative: the options' old names (legacy_names.cpp).
#pragma once

#include <string_view>

namespace rexnative {

// The name an option had before ReXNative renamed it, or nullptr.
const char* OldNameOf(std::string_view new_name);

}  // namespace rexnative
