#include "snapshot.h"

#include "log.h"
#include "trace.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <vector>

REXCVAR_DECLARE(bool, rexnative_classic);

namespace rexnative::snapshot {
namespace {

// Frames between the request and the picture: the settings menu that asked is gone by then.
constexpr uint32_t kDelayFrames = 8;
// Frames of the trace after the picture (3: the first one reads what earlier frames left).
constexpr int kTraceFrames = 3;
constexpr auto kGiveUp = std::chrono::seconds(4);

enum State : int { kIdle, kWaiting, kCapturing };
std::atomic<int> g_state{kIdle};
std::mutex g_mutex;
std::filesystem::path g_dir;  // g_mutex
uint32_t g_delay = 0;         // g_mutex
std::chrono::steady_clock::time_point g_started;
std::string g_message;          // g_mutex
std::atomic<int64_t> g_message_at{0};

std::filesystem::path ExeDir() {
  wchar_t path[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
  return std::filesystem::path(std::wstring(path, n)).parent_path();
}

void SetMessage(std::string text) {
  g_message = std::move(text);
  g_message_at.store(std::chrono::steady_clock::now().time_since_epoch().count(),
                     std::memory_order_relaxed);
}

// The oldest folders beyond kSnapshotsKept go (each holds a trace of tens of MB).
void Prune(const std::filesystem::path& root) {
  std::error_code ec;
  std::vector<std::filesystem::path> dirs;
  for (const auto& e : std::filesystem::directory_iterator(root, ec))
    if (e.is_directory(ec)) dirs.push_back(e.path());
  if (int(dirs.size()) <= kSnapshotsKept) return;
  std::sort(dirs.begin(), dirs.end());  // named by date and time
  for (size_t i = 0; i + kSnapshotsKept < dirs.size(); ++i) std::filesystem::remove_all(dirs[i], ec);
}

}  // namespace

void Request(const char* why) {
  std::lock_guard lock(g_mutex);
  if (g_state.load(std::memory_order_relaxed) != kIdle) return;
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
  localtime_s(&tm, &now);
  char name[32];
  std::strftime(name, sizeof(name), "%Y%m%d-%H%M%S", &tm);
  const std::filesystem::path root = ExeDir() / "snapshots";
  std::error_code ec;
  std::filesystem::create_directories(root / name, ec);
  if (ec) {
    REXLOG_WARN("ReXNative snapshot: cannot create {}: {}", (root / name).string(), ec.message());
    SetMessage("Snapshot failed: cannot write its folder");
    return;
  }
  g_dir = root / name;
  g_delay = kDelayFrames;
  g_state.store(kWaiting, std::memory_order_release);
  REXLOG_INFO("ReXNative snapshot: asked ({}), into {}", why ? why : "?", g_dir.string());
  Prune(root);
}

bool Due(uint64_t frame) {
  const int state = g_state.load(std::memory_order_acquire);
  if (state == kIdle) return false;
  std::lock_guard lock(g_mutex);
  if (state == kCapturing) {
    if (std::chrono::steady_clock::now() - g_started > kGiveUp) {
      g_state.store(kIdle, std::memory_order_release);
      REXLOG_WARN("ReXNative snapshot: no picture of the frame within 4 s ({})", g_dir.string());
      SetMessage("Snapshot: the trace was saved, the picture was not");
    }
    return false;
  }
  if (g_delay) {
    --g_delay;
    return false;
  }
  g_started = std::chrono::steady_clock::now();
  g_state.store(kCapturing, std::memory_order_release);
  // The trace holds what the native renderer is handed: on the classic renderer there is none (it
  // would wait for the switch back), so the picture alone.
  const bool classic = REXCVAR_GET(rexnative_classic);
  if (!classic) trace::RecordNext(kTraceFrames, g_dir);
  if (std::FILE* f = _wfopen((g_dir / "snapshot.txt").c_str(), L"w")) {
    std::fprintf(f, "frame %llu\nrenderer %s\ntrace frames %d\n", (unsigned long long)frame,
                 classic ? "classic" : "native", classic ? 0 : kTraceFrames);
    std::fclose(f);
  }
  return true;
}

bool Pending() { return g_state.load(std::memory_order_acquire) != kIdle; }
bool Capturing() { return g_state.load(std::memory_order_acquire) == kCapturing; }

std::filesystem::path Dir() {
  std::lock_guard lock(g_mutex);
  return g_dir;
}

void Taken(uint64_t frame) {
  std::lock_guard lock(g_mutex);
  if (g_state.load(std::memory_order_relaxed) != kCapturing) return;
  g_state.store(kIdle, std::memory_order_release);
  REXLOG_INFO("ReXNative snapshot: frame {} into {}", frame, g_dir.string());
  SetMessage("Snapshot saved. Send it with Report a problem in the ReXNative Manager");
}

std::string Message(int64_t* at) {
  std::lock_guard lock(g_mutex);
  *at = g_message_at.load(std::memory_order_relaxed);
  return g_message;
}

}  // namespace rexnative::snapshot
