#include "watch.h"

#include "host.h"
#include "log.h"

#include <rex/kernel/heap.h>
#include <rex/logging.h>
#include <rex/ppc/recompiled_code.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace rexnative::watch {
namespace {

// Host function -> guest address, from the generated code's table, sorted by host address.
struct HostFunction {
  uintptr_t host;
  uint32_t guest;
};
std::vector<HostFunction>& Functions() {
  static std::vector<HostFunction> table = [] {
    std::vector<HostFunction> t;
    for (const PPCFuncMapping* m = kPPCFuncMappings; m->host; ++m)
      t.push_back({reinterpret_cast<uintptr_t>(m->host), uint32_t(m->address)});
    std::sort(t.begin(), t.end(), [](const HostFunction& a, const HostFunction& b) {
      return a.host < b.host;
    });
    return t;
  }();
  return table;
}

// "sub_82345678+0x1C4", or empty outside the recompiled code (more than 1 MB past a function).
std::string Name(uintptr_t pc) {
  const auto& t = Functions();
  auto it = std::upper_bound(t.begin(), t.end(), pc,
                             [](uintptr_t v, const HostFunction& f) { return v < f.host; });
  if (it == t.begin()) return {};
  --it;
  const uintptr_t off = pc - it->host;
  if (off > (1u << 20)) return {};
  char buf[48];
  std::snprintf(buf, sizeof(buf), "sub_%08X+0x%zX", it->guest, size_t(off));
  return buf;
}

std::atomic<uintptr_t> g_host{0};
std::atomic<uint32_t> g_guest{0};
std::atomic<bool> g_nan_only{true};
std::atomic<int32_t> g_left{0};
// `read` mode: reads and writes of a physical address through each of its CPU views
// (0xA0000000, 0xC0000000, 0xE0000000), debug registers 0-2; the guest code's only.
std::atomic<bool> g_reads{false};
uintptr_t g_view_host[3] = {};
uint32_t g_view_guest[3] = {};

bool IsNan(uint32_t bits) { return (bits & 0x7F800000u) == 0x7F800000u && (bits & 0x7FFFFFu); }

#ifdef _WIN32
PVOID g_handler = nullptr;

LONG CALLBACK OnException(EXCEPTION_POINTERS* e) {
  if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
  CONTEXT* c = e->ContextRecord;
  if (g_reads.load(std::memory_order_relaxed)) {
    const DWORD64 hit = c->Dr6 & 7;
    if (!hit) return EXCEPTION_CONTINUE_SEARCH;
    c->Dr6 = 0;
    const int view = (hit & 1) ? 0 : (hit & 2) ? 1 : 2;
    // The guest code's access, or host code's on its behalf (a memcpy the guest called): the
    // recompiled functions on the stack; none, the host's own (a writeback): skipped.
    std::string where = Name(uintptr_t(c->Rip));
    void* frames[48];
    const USHORT n = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
    bool guest = !where.empty();
    for (USHORT i = 0; i < n && !guest; ++i)
      guest = !Name(reinterpret_cast<uintptr_t>(frames[i])).empty();
    if (!guest) return EXCEPTION_CONTINUE_EXECUTION;
    if (where.empty()) where = "host code";
    if (g_left.fetch_sub(1, std::memory_order_relaxed) <= 0) return EXCEPTION_CONTINUE_EXECUTION;
    uint32_t raw;
    std::memcpy(&raw, reinterpret_cast<const void*>(g_view_host[view]), 4);
    std::string last = where;
    int added = 0;
    for (USHORT i = 0; i < n && added < 8; ++i) {
      std::string f = Name(reinterpret_cast<uintptr_t>(frames[i]));
      if (f.empty()) continue;
      const std::string fn = f.substr(0, f.find('+'));
      if (fn == last.substr(0, last.find('+'))) continue;
      where += " <- " + f;
      last = f;
      ++added;
    }
    REXLOG_INFO("ReXNative watch: {:08X} accessed (holds {:08X}) by {}", g_view_guest[view],
                _byteswap_ulong(raw), where);
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (!(c->Dr6 & 1)) return EXCEPTION_CONTINUE_SEARCH;
  c->Dr6 = 0;
  const uintptr_t host = g_host.load(std::memory_order_relaxed);
  if (!host) return EXCEPTION_CONTINUE_EXECUTION;
  uint32_t raw;
  std::memcpy(&raw, reinterpret_cast<const void*>(host), 4);
  const uint32_t value = _byteswap_ulong(raw);  // guest memory is big-endian
  if (g_nan_only.load(std::memory_order_relaxed) && !IsNan(value))
    return EXCEPTION_CONTINUE_EXECUTION;
  if (g_left.fetch_sub(1, std::memory_order_relaxed) <= 0) return EXCEPTION_CONTINUE_EXECUTION;
  // The writer, then the recompiled functions further up the host stack.
  std::string chain = Name(uintptr_t(c->Rip));
  if (chain.empty()) chain = "host code";
  void* frames[48];
  const USHORT n = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
  std::string last = chain;
  int added = 0;
  for (USHORT i = 0; i < n && added < 10; ++i) {
    std::string f = Name(reinterpret_cast<uintptr_t>(frames[i]));
    if (f.empty()) continue;
    const std::string fn = f.substr(0, f.find('+'));
    if (fn == last.substr(0, last.find('+'))) continue;
    chain += " <- " + f;
    last = f;
    ++added;
  }
  REXLOG_INFO("ReXNative watch: {:08X} <- {:08X} by {}", g_guest.load(), value, chain);
  return EXCEPTION_CONTINUE_EXECUTION;
}

// Debug register 0 (and its enable bits in 7) on every other thread of the process; with
// g_reads, registers 0-2 on the views (read or write).
void Apply(uintptr_t host) {
  const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  THREADENTRY32 te{sizeof(te)};
  uint32_t set = 0, failed = 0;
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
    HANDLE h = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE,
                          te.th32ThreadID);
    if (!h) {
      ++failed;
      continue;
    }
    if (SuspendThread(h) != DWORD(-1)) {
      CONTEXT c{};
      c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
      if (GetThreadContext(h, &c)) {
        if (g_reads.load() && host) {
          // L0-L2, read or write (RW = 11), 4 bytes (LEN = 11) each.
          c.Dr0 = g_view_host[0];
          c.Dr1 = g_view_host[1];
          c.Dr2 = g_view_host[2];
          c.Dr7 = (c.Dr7 & ~0xFFF003Full) | 0x15ull | (0xFFFull << 16);
        } else {
          c.Dr0 = host;
          // L0, write only (RW0 = 01), 4 bytes (LEN0 = 11); cleared when host is 0.
          c.Dr7 = host ? (c.Dr7 & ~0xFFF003Full) | 1ull | (1ull << 16) | (3ull << 18)
                       : (c.Dr7 & ~0xFFF003Full);
        }
        c.Dr6 = 0;
        if (SetThreadContext(h, &c)) ++set; else ++failed;
      } else {
        ++failed;
      }
      ResumeThread(h);
    }
    CloseHandle(h);
  }
  CloseHandle(snap);
  REXLOG_INFO("ReXNative watch: debug register set on {} threads ({} failed)", set, failed);
}
#endif

}  // namespace

void Set(uint32_t guest_address, const std::string& mode, uint32_t count) {
#ifdef _WIN32
  guest_address &= ~3u;
  auto* mem = rexnative::host::Memory();
  // `read`: a physical address, watched through its 0xA0000000 view (and the others).
  const uint32_t watched = mode == "read" ? (0xA0000000u | (guest_address & 0x1FFFFFFCu))
                                          : guest_address;
  const uintptr_t host = mem ? reinterpret_cast<uintptr_t>(
                                   mem->TranslateVirtual<uint8_t*>(watched))
                             : 0;
  if (!host) {
    REXLOG_WARN("ReXNative watch: {:08X} has no host address", guest_address);
    return;
  }
  Functions();  // built here, not in the exception handler
  if (!g_handler) g_handler = AddVectoredExceptionHandler(1, OnException);
  g_reads = mode == "read";
  if (g_reads) {
    // `guest_address` physical: its CPU views.
    const uint32_t phys = guest_address & 0x1FFFFFFCu;
    const uint32_t views[3] = {0xA0000000u | phys, 0xC0000000u | phys, 0xE0000000u | phys};
    for (int i = 0; i < 3; ++i) {
      g_view_guest[i] = views[i];
      g_view_host[i] = reinterpret_cast<uintptr_t>(mem->TranslateVirtual<uint8_t*>(views[i]));
    }
  }
  g_guest = guest_address;
  g_nan_only = mode != "any";
  g_left = int32_t(count ? count : 16);
  g_host = host;
  REXLOG_INFO("ReXNative watch: {:08X} (host {:016X}), {}, at most {}", guest_address, host,
              g_reads ? "reads and writes through its physical views"
              : g_nan_only ? "NaN writes" : "all writes",
              g_left.load());
  // Not from this thread: SetThreadContext on the caller itself is not allowed.
  std::thread([host] { Apply(host); }).join();
#else
  (void)guest_address;
  (void)mode;
  (void)count;
  REXLOG_WARN("ReXNative watch: only on Windows");
#endif
}

void Clear() {
#ifdef _WIN32
  g_host = 0;
  std::thread([] { Apply(0); }).join();
#endif
}

}  // namespace rexnative::watch
