// The Linux build's sampling profiler, for finding where a thread spends its time (a game's main
// thread spinning between frames): REXNATIVE_PROFILE=<thread name prefix>[,<seconds>[,<delay>]]
// sends that thread SIGPROF every millisecond for <seconds> (default 20) after <delay> seconds
// (default 60); the handler keeps its backtrace. Then logs/profile.txt (beside the executable)
// gets the most frequent stacks, each frame as <module>+<offset>; the executable's offsets match
// its link map (build/<name>.map), where the recompiled guest functions are sub_<address>.
// Nothing runs without the variable. No ptrace: the Steam Deck's Yama does not let another
// process read the thread.
#include "rexnative_win32.h"

#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kFrames = 8;
constexpr size_t kMaxSamples = 200000;

struct Sample {
  void* frames[kFrames];
  int count;
};

Sample* g_samples = nullptr;
std::atomic<size_t> g_next{0};

void OnProf(int, siginfo_t*, void*) {
  const size_t i = g_next.fetch_add(1, std::memory_order_relaxed);
  if (i >= kMaxSamples) return;
  Sample& s = g_samples[i];
  // backtrace() is not async-signal-safe in general; it is called once before the first signal
  // (libgcc's unwinder loaded), and only this thread is interrupted, for a diagnostic.
  s.count = backtrace(s.frames, kFrames);
}

std::string Where(void* address) {
  Dl_info info = {};
  if (!dladdr(address, &info) || !info.dli_fname) return "?";
  const std::string module = std::filesystem::path(info.dli_fname).filename().string();
  char text[256];
  if (info.dli_sname && info.dli_saddr) {
    std::snprintf(text, sizeof(text), "%s!%s+0x%lx", module.c_str(), info.dli_sname,
                  static_cast<unsigned long>(static_cast<char*>(address) -
                                             static_cast<char*>(info.dli_saddr)));
  } else {
    std::snprintf(text, sizeof(text), "%s+0x%lx", module.c_str(),
                  static_cast<unsigned long>(static_cast<char*>(address) -
                                             static_cast<char*>(info.dli_fbase)));
  }
  return text;
}

pid_t FindThread(const std::string& prefix) {
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator("/proc/self/task", ec)) {
    std::FILE* f = std::fopen((entry.path() / "comm").c_str(), "r");
    if (!f) continue;
    char name[64] = {};
    const bool read = std::fgets(name, sizeof(name), f) != nullptr;
    std::fclose(f);
    if (read && std::strncmp(name, prefix.c_str(), prefix.size()) == 0)
      return pid_t(std::atoi(entry.path().filename().c_str()));
  }
  return 0;
}

void Run(std::string prefix, int seconds, int delay) {
  std::this_thread::sleep_for(std::chrono::seconds(delay));
  pid_t tid = 0;
  for (int i = 0; i < 100 && !(tid = FindThread(prefix)); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  std::error_code ec;
  const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
  const auto out_path = exe.parent_path() / "logs" / "profile.txt";
  std::FILE* out = std::fopen(out_path.c_str(), "w");
  if (!out) return;
  if (!tid) {
    std::fprintf(out, "no thread named %s*\n", prefix.c_str());
    std::fclose(out);
    return;
  }
  g_samples = static_cast<Sample*>(std::calloc(kMaxSamples, sizeof(Sample)));
  void* warm[4];
  backtrace(warm, 4);  // loads the unwinder before the first signal
  struct sigaction sa = {};
  sa.sa_sigaction = OnProf;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigaction(SIGPROF, &sa, nullptr);
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  while (std::chrono::steady_clock::now() < end) {
    syscall(SYS_tgkill, getpid(), tid, SIGPROF);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  signal(SIGPROF, SIG_IGN);
  const size_t n = std::min(g_next.load(), kMaxSamples);
  // The stacks, by the frames from the interrupted one outwards (the handler's own frames, the
  // first two or three, are left out: from the first frame after the signal trampoline).
  std::map<std::string, int> stacks, leaves, exe_frames;
  for (size_t i = 0; i < n; ++i) {
    const Sample& s = g_samples[i];
    // The handler, then the signal trampoline (libc's __restore_rt), then the interrupted code
    // (which may be in libc too: sched_yield, nanosleep).
    int first = std::min(2, s.count - 1);
    for (int f = 0; f < s.count; ++f)
      if (Where(s.frames[f]).rfind("libc.so.6", 0) == 0) {
        first = f + 1;
        break;
      }
    std::string stack;
    for (int f = first; f < s.count; ++f) stack += Where(s.frames[f]) + " <- ";
    ++stacks[stack];
    if (first < s.count) ++leaves[Where(s.frames[first])];
    for (int f = first; f < s.count; ++f) {
      const std::string w = Where(s.frames[f]);
      if (w.rfind(exe.filename().string() + "+", 0) == 0) {
        ++exe_frames[w];
        break;
      }
    }
  }
  auto dump = [&](const char* title, const std::map<std::string, int>& m, size_t top) {
    std::vector<std::pair<int, std::string>> v;
    for (const auto& [k, c] : m) v.push_back({c, k});
    std::sort(v.rbegin(), v.rend());
    std::fprintf(out, "== %s (%zu samples)\n", title, n);
    for (size_t i = 0; i < v.size() && i < top; ++i)
      std::fprintf(out, "%6d %5.1f%%  %s\n", v[i].first, 100.0 * v[i].first / double(n),
                   v[i].second.c_str());
  };
  std::fprintf(out, "thread %s* (tid %d), %d s from %d s, every 1 ms\n", prefix.c_str(), int(tid),
               seconds, delay);
  dump("interrupted at", leaves, 40);
  dump("first frame in the executable", exe_frames, 40);
  dump("stacks", stacks, 30);
  std::fclose(out);
}

// Started before main when REXNATIVE_PROFILE is set.
[[maybe_unused]] const bool g_started = [] {
  const char* v = std::getenv("REXNATIVE_PROFILE");
  if (!v || !*v) return false;
  std::string spec = v, prefix = spec;
  int seconds = 20, delay = 60;
  if (const size_t c = spec.find(','); c != std::string::npos) {
    prefix = spec.substr(0, c);
    std::sscanf(spec.c_str() + c + 1, "%d,%d", &seconds, &delay);
  }
  std::thread(Run, prefix, seconds, delay).detach();
  return true;
}();

}  // namespace
