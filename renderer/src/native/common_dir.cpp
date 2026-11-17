#include "common_dir.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <filesystem>
#include <iterator>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

REXCVAR_DEFINE_STRING(rexnative_common_dir, "", "ReXNative",
                      "The folder of the shared runtime DLLs (DLSS, DLSS 5, FidelityFX) when they "
                      "are not beside the executable; empty: ..\\common, then ..\\..\\common");

namespace rexnative {
namespace {

constexpr const wchar_t* kMarkers[] = {L"amd_fidelityfx_loader_dx12.dll", L"nvngx_dlss.dll",
                                       L"nvngx_dlssnr.dll"};

bool HoldsShared(const std::filesystem::path& dir) {
  std::error_code ec;
  for (const wchar_t* name : kMarkers)
    if (std::filesystem::exists(dir / name, ec)) return true;
  return false;
}

}  // namespace

const std::wstring& CommonDllDir() {
  static const std::wstring dir = [] {
    std::wstring found;
#ifdef _WIN32
    wchar_t buffer[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buffer, DWORD(std::size(buffer)));
    const std::filesystem::path exe_dir = std::filesystem::path(std::wstring(buffer, n)).parent_path();
    if (HoldsShared(exe_dir)) return found;  // beside the executable: the old layout
    std::vector<std::filesystem::path> candidates;
    const std::string flag = REXCVAR_GET(rexnative_common_dir);
    if (!flag.empty()) candidates.push_back(std::filesystem::u8path(flag));
    candidates.push_back(exe_dir.parent_path() / L"common");
    candidates.push_back(exe_dir.parent_path().parent_path() / L"common");
    for (const auto& c : candidates) {
      if (!HoldsShared(c)) continue;
      std::error_code ec;
      const std::filesystem::path full = std::filesystem::weakly_canonical(c, ec);
      found = (ec ? c : full).wstring();
      SetDllDirectoryW(found.c_str());
      REXLOG_INFO("ReXNative: shared DLLs (DLSS, FidelityFX) from {}",
                  (ec ? c : full).u8string().c_str());
      break;
    }
#endif
    return found;
  }();
  return dir;
}

}  // namespace rexnative
