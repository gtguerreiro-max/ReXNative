// The Linux build's Win32 functions (compat/linux/rexnative_win32.h): events on eventfd, waitable
// timers on timerfd, threads on pthreads, modules on dlopen, window sizes from SDL3.
#include "rexnative_win32.h"

#include <SDL3/SDL_video.h>

#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

enum class Kind { kAutoEvent, kManualEvent, kTimer };

// The kind of each handle made here; a handle from elsewhere (vkd3d-proton's frame latency
// eventfd) is waited on as an auto-reset event.
std::mutex g_mutex;
std::unordered_map<int, Kind>& Kinds() {
  static std::unordered_map<int, Kind> kinds;
  return kinds;
}

thread_local DWORD g_last_error = 0;

int Fd(HANDLE h) { return int(intptr_t(h)); }
HANDLE Handle(int fd) { return reinterpret_cast<HANDLE>(intptr_t(fd)); }

// FD 0 means no handle to vkd3d-proton (vkd3d_native_sync_handle_wrap): never hand it out.
int AvoidZero(int fd) {
  if (fd != 0) return fd;
  const int other = fcntl(fd, F_DUPFD_CLOEXEC, 1);
  close(fd);
  return other;
}

HANDLE Register(int fd, Kind kind) {
  if (fd < 0) {
    g_last_error = DWORD(errno);
    return nullptr;
  }
  fd = AvoidZero(fd);
  std::lock_guard lock(g_mutex);
  Kinds()[fd] = kind;
  return Handle(fd);
}

Kind KindOf(int fd) {
  std::lock_guard lock(g_mutex);
  const auto it = Kinds().find(fd);
  return it == Kinds().end() ? Kind::kAutoEvent : it->second;
}

// Takes the signal: an auto-reset event or a semaphore eventfd gives one, a timer its expiries;
// a manual-reset event stays set. False when another waiter took it first.
bool Consume(int fd) {
  if (KindOf(fd) == Kind::kManualEvent) return true;
  uint64_t value = 0;
  return read(fd, &value, sizeof(value)) == ssize_t(sizeof(value));
}

const HANDLE kCurrentThread = reinterpret_cast<HANDLE>(intptr_t(-2));

std::string Narrow(LPCWSTR w, int count = -1) {
  std::string s;
  if (!w) return s;
  for (int i = 0; count < 0 ? w[i] != 0 : i < count; ++i) {
    const uint32_t c = uint32_t(w[i]);
    if (c < 0x80) {
      s += char(c);
    } else if (c < 0x800) {
      s += char(0xC0 | (c >> 6));
      s += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      s += char(0xE0 | (c >> 12));
      s += char(0x80 | ((c >> 6) & 0x3F));
      s += char(0x80 | (c & 0x3F));
    } else {
      s += char(0xF0 | (c >> 18));
      s += char(0x80 | ((c >> 12) & 0x3F));
      s += char(0x80 | ((c >> 6) & 0x3F));
      s += char(0x80 | (c & 0x3F));
    }
  }
  return s;
}

}  // namespace

HANDLE CreateEventW(LPSECURITY_ATTRIBUTES, BOOL manual_reset, BOOL initial_state, LPCWSTR) {
  return Register(eventfd(initial_state ? 1 : 0, EFD_CLOEXEC | EFD_NONBLOCK),
                  manual_reset ? Kind::kManualEvent : Kind::kAutoEvent);
}

HANDLE CreateEventA(LPSECURITY_ATTRIBUTES a, BOOL manual_reset, BOOL initial_state, LPCSTR) {
  return CreateEventW(a, manual_reset, initial_state, nullptr);
}

BOOL SetEvent(HANDLE event) {
  const int fd = Fd(event);
  if (KindOf(fd) == Kind::kManualEvent) {
    // Set stays set: one count is enough, a second would need two resets.
    pollfd p = {fd, POLLIN, 0};
    if (poll(&p, 1, 0) == 1) return TRUE;
  }
  const uint64_t one = 1;
  return write(fd, &one, sizeof(one)) == ssize_t(sizeof(one));
}

BOOL ResetEvent(HANDLE event) {
  uint64_t value = 0;
  while (read(Fd(event), &value, sizeof(value)) == ssize_t(sizeof(value))) {
  }
  return TRUE;
}

BOOL CloseHandle(HANDLE handle) {
  if (!handle || handle == kCurrentThread) return TRUE;
  const intptr_t value = intptr_t(handle);
  if (value < 0) return TRUE;  // a thread's pseudo handle (CreateThread)
  const int fd = int(value);
  {
    std::lock_guard lock(g_mutex);
    Kinds().erase(fd);
  }
  return close(fd) == 0;
}

DWORD WaitForSingleObject(HANDLE handle, DWORD milliseconds) {
  return WaitForMultipleObjects(1, &handle, FALSE, milliseconds);
}

DWORD WaitForSingleObjectEx(HANDLE handle, DWORD milliseconds, BOOL) {
  return WaitForSingleObject(handle, milliseconds);
}

DWORD WaitForMultipleObjectsEx(DWORD count, const HANDLE* handles, BOOL wait_all,
                               DWORD milliseconds, BOOL) {
  return WaitForMultipleObjects(count, handles, wait_all, milliseconds);
}

DWORD WaitForMultipleObjects(DWORD count, const HANDLE* handles, BOOL wait_all, DWORD milliseconds) {
  using Clock = std::chrono::steady_clock;
  const auto deadline = milliseconds == INFINITE
                            ? Clock::time_point::max()
                            : Clock::now() + std::chrono::milliseconds(milliseconds);
  std::vector<bool> done(count, false);
  for (;;) {
    std::vector<pollfd> fds;
    std::vector<DWORD> index;
    for (DWORD i = 0; i < count; ++i) {
      if (done[i]) continue;
      fds.push_back({Fd(handles[i]), POLLIN, 0});
      index.push_back(i);
    }
    int timeout = -1;
    if (milliseconds != INFINITE) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
      timeout = int(std::max<int64_t>(0, left.count()));
    }
    const int n = poll(fds.data(), nfds_t(fds.size()), timeout);
    if (n < 0) {
      if (errno == EINTR) continue;
      g_last_error = DWORD(errno);
      return WAIT_FAILED;
    }
    for (size_t k = 0; k < fds.size(); ++k) {
      if (!(fds[k].revents & POLLIN) || !Consume(fds[k].fd)) continue;
      if (!wait_all) return WAIT_OBJECT_0 + index[k];
      done[index[k]] = true;
    }
    if (wait_all && std::all_of(done.begin(), done.end(), [](bool d) { return d; }))
      return WAIT_OBJECT_0;
    if (milliseconds != INFINITE && Clock::now() >= deadline) return WAIT_TIMEOUT;
  }
}

HANDLE CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD) {
  return Register(timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK), Kind::kTimer);
}

BOOL SetWaitableTimer(HANDLE timer, const LARGE_INTEGER* due, LONG period, PTIMERAPCROUTINE, void*,
                      BOOL) {
  // A negative due time is relative, in 100 ns units (the renderer's only use).
  int64_t ns = due ? -int64_t(due->QuadPart) * 100 : 0;
  ns = std::max<int64_t>(ns, 1);
  itimerspec spec = {};
  spec.it_value.tv_sec = time_t(ns / 1000000000);
  spec.it_value.tv_nsec = long(ns % 1000000000);
  if (period > 0) {
    spec.it_interval.tv_sec = time_t(period / 1000);
    spec.it_interval.tv_nsec = long(period % 1000) * 1000000;
  }
  return timerfd_settime(Fd(timer), 0, &spec, nullptr) == 0;
}

HANDLE GetCurrentThread() { return kCurrentThread; }

DWORD GetCurrentThreadId() { return DWORD(gettid()); }

HRESULT SetThreadDescription(HANDLE thread, LPCWSTR description) {
  if (thread != kCurrentThread) return E_NOTIMPL;
  std::string name = Narrow(description);
  if (name.size() > 15) name.resize(15);  // the kernel's limit
  pthread_setname_np(pthread_self(), name.c_str());
  return S_OK;
}

HANDLE CreateThread(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE start, void* parameter,
                    DWORD, DWORD* thread_id) {
  std::thread t([start, parameter] { start(parameter); });
  if (thread_id) *thread_id = 0;
  t.detach();
  return reinterpret_cast<HANDLE>(intptr_t(-3));  // CloseHandle ignores it
}

void Sleep(DWORD milliseconds) {
  std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

HMODULE LoadLibraryA(LPCSTR name) {
  if (!name) return nullptr;
  const std::string n = name;
  // Windows DLLs (DLSS, FidelityFX's DX12 runtime, d3dcompiler) have no Linux build here.
  if (n.size() > 4 && (n.ends_with(".dll") || n.ends_with(".DLL"))) {
    g_last_error = 126;  // ERROR_MOD_NOT_FOUND
    return nullptr;
  }
  void* h = dlopen(name, RTLD_NOW | RTLD_LOCAL);
  if (!h) g_last_error = 126;
  return h;
}

HMODULE LoadLibraryW(LPCWSTR name) { return LoadLibraryA(Narrow(name).c_str()); }

HMODULE GetModuleHandleW(LPCWSTR name) {
  if (!name) return dlopen(nullptr, RTLD_NOW);
  const std::string n = Narrow(name);
  if (n.ends_with(".dll") || n.ends_with(".DLL")) return nullptr;  // never Wine's ntdll
  return dlopen(n.c_str(), RTLD_NOW | RTLD_NOLOAD);
}

FARPROC GetProcAddress(HMODULE module, LPCSTR name) {
  return module ? reinterpret_cast<FARPROC>(dlsym(module, name)) : nullptr;
}

BOOL FreeLibrary(HMODULE module) { return module && dlclose(module) == 0; }

DWORD GetModuleFileNameA(HMODULE, LPSTR buffer, DWORD size) {
  std::error_code ec;
  const std::string path = std::filesystem::read_symlink("/proc/self/exe", ec).string();
  if (!size) return 0;
  const DWORD n = DWORD(std::min<size_t>(path.size(), size - 1));
  std::memcpy(buffer, path.data(), n);
  buffer[n] = 0;
  return n;
}

DWORD GetModuleFileNameW(HMODULE module, LPWSTR buffer, DWORD size) {
  char narrow[4096];
  const DWORD n = GetModuleFileNameA(module, narrow, sizeof(narrow));
  return DWORD(MultiByteToWideChar(CP_UTF8, 0, narrow, int(n), buffer, int(size)));
}

int WideCharToMultiByte(UINT, DWORD, LPCWSTR wide, int wide_count, LPSTR out, int out_size,
                        LPCSTR, BOOL* used_default) {
  if (used_default) *used_default = FALSE;
  std::string s = Narrow(wide, wide_count);
  if (wide_count < 0) s.push_back('\0');  // the terminator counts when the input has one
  if (!out || out_size <= 0) return int(s.size());
  const int n = std::min<int>(int(s.size()), out_size);
  std::memcpy(out, s.data(), size_t(n));
  return n;
}

int MultiByteToWideChar(UINT, DWORD, LPCSTR text, int count, LPWSTR out, int out_size) {
  if (!text) return 0;
  const size_t len = count < 0 ? std::strlen(text) + 1 : size_t(count);
  std::wstring w;
  for (size_t i = 0; i < len;) {
    const unsigned char c = (unsigned char)text[i];
    uint32_t cp = c;
    size_t extra = 0;
    if (c >= 0xF0) cp = c & 0x07, extra = 3;
    else if (c >= 0xE0) cp = c & 0x0F, extra = 2;
    else if (c >= 0xC0) cp = c & 0x1F, extra = 1;
    ++i;
    for (size_t k = 0; k < extra && i < len; ++k, ++i) cp = (cp << 6) | ((unsigned char)text[i] & 0x3F);
    w.push_back(wchar_t(cp));
  }
  if (!out || out_size <= 0) return int(w.size());
  const int n = std::min<int>(int(w.size()), out_size);
  std::memcpy(out, w.data(), size_t(n) * sizeof(wchar_t));
  return n;
}

DWORD GetLastError() { return g_last_error; }

DWORD GetTempPathW(DWORD size, LPWSTR buffer) {
  const char* t = std::getenv("TMPDIR");
  const std::string dir = std::string(t && *t ? t : "/tmp") + "/";
  return DWORD(MultiByteToWideChar(CP_UTF8, 0, dir.c_str(), -1, buffer, int(size)));
}

BOOL QueryPerformanceCounter(LARGE_INTEGER* count) {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  count->QuadPart = int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
  return TRUE;
}

BOOL QueryPerformanceFrequency(LARGE_INTEGER* frequency) {
  frequency->QuadPart = 1000000000;
  return TRUE;
}

USHORT RtlCaptureStackBackTrace(ULONG skip, ULONG count, void** frames, ULONG* hash) {
  void* all[128];
  const int n = backtrace(all, int(std::min<ULONG>(128, skip + count + 1)));
  int out = 0;
  for (int i = int(skip) + 1; i < n && out < int(count); ++i) frames[out++] = all[i];
  if (hash) *hash = 0;
  return USHORT(out);
}

std::FILE* _wfopen(const wchar_t* path, const wchar_t* mode) {
  return std::fopen(Narrow(path).c_str(), Narrow(mode).c_str());
}

std::FILE* _wfopen(const char* path, const wchar_t* mode) {
  return std::fopen(path, Narrow(mode).c_str());
}

BOOL SetEnvironmentVariableW(LPCWSTR name, LPCWSTR value) {
  const std::string n = Narrow(name);
  return value ? setenv(n.c_str(), Narrow(value).c_str(), 1) == 0 : unsetenv(n.c_str()) == 0;
}

BOOL GetClientRect(HWND window, RECT* rect) {
  int w = 0, h = 0;
  if (!window || !SDL_GetWindowSizeInPixels(static_cast<SDL_Window*>(window), &w, &h)) return FALSE;
  rect->left = rect->top = 0;
  rect->right = w;
  rect->bottom = h;
  return TRUE;
}
