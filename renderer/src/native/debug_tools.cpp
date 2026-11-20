#include "debug_tools.h"

#include "command_stream.h"
#include "log.h"
#include "native/sdk.h"
#include "render_targets.h"
#include "resolve.h"
#include "../compat/renderdoc_app.h"
#include "../host.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <d3d12.h>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

REXCVAR_DEFINE_STRING(
    rexnative_renderdoc, "", "ReXNative",
    "Debug: load RenderDoc into the game so that the renderer takes captures itself (no "
    "launcher): 'auto' (REXNATIVE_RENDERDOC, then the newest RenderDoc* folder in Tools "
    "beside the game's drive roots and in Program Files), RenderDoc's folder, or "
    "renderdoc.dll's path. Empty: only when the game was started from RenderDoc")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(rexnative_renderdoc_dir, "", "ReXNative",
                      "Debug: the folder of the RenderDoc captures (<game>-f<frame>.rdc and "
                      "captures.txt); empty: rdoc beside the executable");
REXCVAR_DEFINE_INT32(rexnative_renderdoc_frames, 0, "ReXNative",
                     "Debug: RenderDoc captures the next this many frames, a capture each "
                     "(live; the key bind_renderdoc asks for --rexnative_renderdoc_burst)");
REXCVAR_DEFINE_INT32(rexnative_renderdoc_burst, 1, "ReXNative",
                     "Debug: frames the RenderDoc key and a mark capture (1-16)");
REXCVAR_DEFINE_INT32(rexnative_renderdoc_frame, -1, "ReXNative",
                     "Debug: RenderDoc captures this guest frame (-1: none)");
REXCVAR_DEFINE_BOOL(rexnative_renderdoc_on_mark, false, "ReXNative",
                    "Debug: each mark (F12, capture.request) also takes a RenderDoc capture "
                    "of the frames after it");
REXCVAR_DEFINE_BOOL(rexnative_markers, false, "ReXNative",
                    "Debug: record an event per pass, draw, clear and resolve and name the "
                    "resources, for PIX and the debug layer (always with RenderDoc loaded)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DECLARE(bool, rexnative_debug_keys);

namespace rexnative::dbg {
namespace detail {
std::atomic<bool> g_markers{false}, g_names{false};

void SetName(ID3D12Object* object, std::string_view name) {
  wchar_t wide[160];
  const size_t n = std::min(name.size(), std::size(wide) - 1);
  for (size_t i = 0; i < n; ++i) wide[i] = wchar_t(static_cast<unsigned char>(name[i]));
  wide[n] = 0;
  object->SetName(wide);
}
}  // namespace detail

namespace {

RENDERDOC_API_1_6_0* g_api = nullptr;
std::atomic<int32_t> g_key_frames{0};  // frames the key asked for
int32_t g_left = 0;                    // frames still to capture (render thread)
std::filesystem::path g_dir;
std::string g_game;

// Events open on the list (render thread): the pass's, then a draw's or a resolve's inside it.
uint32_t g_depth = 0;
bool g_pass_open = false;
SurfaceRegs g_pass;
uint32_t g_pass_index = 0;

std::filesystem::path ExePath() {
  wchar_t path[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
  return std::filesystem::path(std::wstring(path, n));
}

// renderdoc.dll in `dir`, or the newest RenderDoc* folder of `dir` that has one.
std::filesystem::path FindIn(const std::filesystem::path& dir) {
  std::error_code ec;
  if (std::filesystem::exists(dir / "renderdoc.dll", ec)) return dir / "renderdoc.dll";
  std::filesystem::path best;
  for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
    const std::wstring name = e.path().filename().wstring();
    if (!e.is_directory(ec) || _wcsnicmp(name.c_str(), L"RenderDoc", 9) != 0) continue;
    if (!std::filesystem::exists(e.path() / "renderdoc.dll", ec)) continue;
    if (best.empty() || e.path().filename() > best.parent_path().filename())
      best = e.path() / "renderdoc.dll";
  }
  return best;
}

std::filesystem::path FindRenderDoc(const std::string& setting) {
  std::error_code ec;
  if (setting != "auto") {
    const std::filesystem::path p(setting);
    return std::filesystem::is_directory(p, ec) ? FindIn(p) : p;
  }
  wchar_t env[MAX_PATH];
  if (const DWORD n = GetEnvironmentVariableW(L"REXNATIVE_RENDERDOC", env, MAX_PATH);
      n && n < MAX_PATH)
    if (auto p = FindIn(env); !p.empty()) return p;
  // Tools folders: at the root of each fixed drive (F:\Tools\RenderDoc_1.47_64), then where the
  // installer puts it.
  wchar_t drives[256];
  const DWORD n = GetLogicalDriveStringsW(255, drives);
  for (const wchar_t* d = drives; d < drives + n && *d; d += wcslen(d) + 1) {
    if (GetDriveTypeW(d) != DRIVE_FIXED) continue;
    if (auto p = FindIn(std::filesystem::path(d) / "Tools"); !p.empty()) return p;
  }
  if (const DWORD m = GetEnvironmentVariableW(L"ProgramFiles", env, MAX_PATH); m && m < MAX_PATH)
    if (auto p = FindIn(std::filesystem::path(env) / "RenderDoc"); !p.empty()) return p;
  return {};
}

const char* ColorName(uint32_t info) {
  return rex::xenos::GetColorRenderTargetFormatName(
      rex::xenos::ColorRenderTargetFormat((info >> 16) & 0xF));
}

}  // namespace

void Initialize() {
  static std::once_flag once;
  std::call_once(once, [] {
    HMODULE dll = GetModuleHandleW(L"renderdoc.dll");
    const std::string setting = REXCVAR_GET(rexnative_renderdoc);
    if (!dll && !setting.empty()) {
      const std::filesystem::path path = FindRenderDoc(setting);
      if (path.empty() || !(dll = LoadLibraryW(path.c_str())))
        REXLOG_ERROR("ReXNative RenderDoc: --rexnative_renderdoc={}: renderdoc.dll not found{}",
                     setting, path.empty() ? "" : " there");
    }
    if (dll) {
      const auto get = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(dll, "RENDERDOC_GetAPI"));
      if (!get || get(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void**>(&g_api)) != 1)
        g_api = nullptr;
    }
    if (g_api) {
      const std::filesystem::path exe = ExePath();
      g_game = exe.stem().string();
      const std::string dir = REXCVAR_GET(rexnative_renderdoc_dir);
      g_dir = dir.empty() ? exe.parent_path() / "rdoc" : std::filesystem::path(dir);
      std::error_code ec;
      std::filesystem::create_directories(g_dir, ec);
      // The renderer's own keys and overlay: F12 is the mark, and the overlay would be in the
      // frames the renderer's captures read.
      g_api->SetCaptureKeys(nullptr, 0);
      g_api->SetFocusToggleKeys(nullptr, 0);
      g_api->MaskOverlayBits(eRENDERDOC_Overlay_None, eRENDERDOC_Overlay_None);
      g_api->SetCaptureOptionU32(eRENDERDOC_Option_DebugOutputMute, 1);
      int major = 0, minor = 0, patch = 0;
      g_api->GetAPIVersion(&major, &minor, &patch);
      REXLOG_INFO("ReXNative RenderDoc: loaded (API {}.{}.{}), captures in {}", major, minor,
                  patch, g_dir.string());
      host::RegisterBind("bind_renderdoc", "F6",
                         "ReXNative: a RenderDoc capture of the next frames "
                         "(--rexnative_renderdoc_burst)",
                         [] {
                           if (REXCVAR_GET(rexnative_debug_keys))
                             g_key_frames.store(
                                 std::clamp(REXCVAR_GET(rexnative_renderdoc_burst), 1, 16),
                                 std::memory_order_relaxed);
                         });
    }
    detail::g_markers.store(REXCVAR_GET(rexnative_markers), std::memory_order_relaxed);
    detail::g_names.store(g_api != nullptr || REXCVAR_GET(rexnative_markers),
                          std::memory_order_relaxed);
  });
}

bool RenderDocLoaded() { return g_api != nullptr; }

void BeginEvent(CommandStream* list, std::string_view name) {
  list->BeginEvent(name);
  ++g_depth;
}

void EndEvent(CommandStream* list) {
  if (!g_depth) return;  // CloseEvents ended it
  --g_depth;
  list->EndEvent();
}

void CloseEvents(CommandStream* list) {
  for (; g_depth; --g_depth) list->EndEvent();
  g_pass_open = false;
}

void EndPass(CommandStream* list) {
  if (!g_pass_open) return;
  g_pass_open = false;
  // A pass has nothing open inside it between two calls of the renderer.
  for (; g_depth; --g_depth) list->EndEvent();
}

void Pass(CommandStream* list, const SurfaceRegs& regs) {
  if (!Markers()) [[likely]]
    return;
  if (g_pass_open && !std::memcmp(&regs, &g_pass, sizeof(regs))) return;
  EndPass(list);
  g_pass = regs;
  g_pass_open = true;
  const uint32_t pitch = regs.surface_info & 0x3FFF;
  std::string name = std::format("pass {}: pitch {} x{}", g_pass_index++, pitch,
                                 SurfaceSamples(regs.surface_info));
  for (uint32_t i = 0; i < 4; ++i) {
    // Targets 1-3 at EDRAM base 0 with target 0's there too are unused (the D3D leaves them).
    if (i && !regs.color_info[i]) continue;
    name += std::format(" c{} @{} {}", i, regs.color_info[i] & 0xFFF, ColorName(regs.color_info[i]));
  }
  name += std::format(" d @{} {}", regs.depth_info & 0xFFF,
                      (regs.depth_info >> 16) & 1 ? "D24FS8" : "D24S8");
  BeginEvent(list, name);
}

std::string DescribeResolve(const ResolveParams& p) {
  const rex::xenos::FormatInfo* info =
      rex::xenos::FormatInfo::Get(rex::xenos::TextureFormat(p.dest_format));
  const std::string source =
      p.source < 4 ? std::format("c{} @{} {}", p.source, p.regs.color_info[p.source] & 0xFFF,
                                 ColorName(p.regs.color_info[p.source]))
                   : std::format("depth @{}", p.regs.depth_info & 0xFFF);
  return std::format("resolve {} ({},{})-({},{}) -> {:08X} {} {}x{}{}{}", source, p.x0, p.y0, p.x1,
                     p.y1, p.dest_base, info ? info->name : "?", p.dest_width, p.dest_height,
                     p.clear_flags ? " +clear" : "", p.from_registers ? " (inline)" : "");
}

bool CaptureNext(uint64_t frame, bool marked) {
  g_pass_index = 0;  // passes are numbered within their frame
  if (!g_api) return false;
  const int32_t burst = std::clamp(REXCVAR_GET(rexnative_renderdoc_burst), 1, 16);
  if (const int32_t asked = REXCVAR_GET(rexnative_renderdoc_frames); asked > 0) {
    g_left = std::max(g_left, std::min(asked, 64));
    rex::cvar::SetFlagByName("rexnative_renderdoc_frames", "0");
  }
  if (const int32_t key = g_key_frames.exchange(0, std::memory_order_relaxed))
    g_left = std::max(g_left, key);
  if (marked && REXCVAR_GET(rexnative_renderdoc_on_mark)) g_left = std::max(g_left, burst);
  if (const int32_t at = REXCVAR_GET(rexnative_renderdoc_frame);
      at >= 0 && uint64_t(at) == frame + 1)
    g_left = std::max(g_left, 1);
  const bool capture = g_left > 0;
  if (capture) --g_left;
  // The captured frame's events.
  detail::g_markers.store(capture || REXCVAR_GET(rexnative_markers), std::memory_order_relaxed);
  return capture;
}

void StartCapture(ID3D12Device* device, uint64_t frame) {
  if (!g_api) return;
  g_api->SetCaptureFilePathTemplate((g_dir / std::format("{}-f{}", g_game, frame)).string().c_str());
  g_api->StartFrameCapture(device, nullptr);
  g_api->SetCaptureTitle(std::format("{} frame {}", g_game, frame).c_str());
}

void EndCapture(ID3D12Device* device, uint64_t frame) {
  if (!g_api) return;
  const uint32_t before = g_api->GetNumCaptures();
  if (g_api->EndFrameCapture(device, nullptr) != 1 || g_api->GetNumCaptures() <= before) {
    REXLOG_ERROR("ReXNative RenderDoc: the capture of frame {} failed", frame);
    return;
  }
  char path[1024];
  uint32_t length = sizeof(path);
  uint64_t timestamp = 0;
  if (!g_api->GetCapture(g_api->GetNumCaptures() - 1, path, &length, &timestamp)) return;
  // RenderDoc's name ends in its own frame number: the guest frame's alone instead.
  std::filesystem::path file(path);
  const std::filesystem::path named = g_dir / std::format("{}-f{}.rdc", g_game, frame);
  std::error_code ec;
  std::filesystem::rename(file, named, ec);
  if (!ec) file = named;
  REXLOG_INFO("ReXNative RenderDoc: frame {} captured: {}", frame, file.string());
  std::ofstream(g_dir / "captures.txt", std::ios::app) << frame << ' ' << file.string() << '\n';
}

}  // namespace rexnative::dbg
