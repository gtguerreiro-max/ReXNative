// The shared runtime DLLs (NVIDIA's DLSS and DLSS 5, AMD's FidelityFX loader, upscaler and frame
// generation: 413 MB) are kept once for every game, in a folder named common, instead of beside each
// game's executable, like Steam's shared redistributables:
//   <work>/common, for <work>/<game>/run/<game>.exe and <work>/<game>/build/<game>.exe;
//   ~/Games/ReXNative/common on the Steam Deck, for ~/Games/ReXNative/<game>/<game>.exe.
// A port that keeps them beside its executable (the old layout) goes on working: that is looked at
// first. --rexnative_common_dir names another folder.
#pragma once

#include <string>

namespace rexnative {

// The folder the shared DLLs are in; empty when they are beside the executable or not found.
// The first call also adds it to the DLL search path (SetDllDirectory), for the libraries that
// load the others by name (FidelityFX's loader loads its upscaler and frame generation).
const std::wstring& CommonDllDir();

}  // namespace rexnative
