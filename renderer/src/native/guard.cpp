#include "guard.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace rexnative {

#ifdef _WIN32
namespace {
Fault g_fault;
int Filter(EXCEPTION_POINTERS* info) {
  // ReXGlue's vectored handler has seen it first (MMIO pages): what reaches here is a real fault.
  const EXCEPTION_RECORD& r = *info->ExceptionRecord;
  if (r.ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
  g_fault.address = r.NumberParameters >= 2 ? uint64_t(r.ExceptionInformation[1]) : 0;
  g_fault.write = r.NumberParameters >= 1 && r.ExceptionInformation[0] == 1;
  g_fault.code_offset = uint64_t(reinterpret_cast<uintptr_t>(r.ExceptionAddress)) -
                        uint64_t(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)));
  return EXCEPTION_EXECUTE_HANDLER;
}
}  // namespace

// No C++ object in this frame: __try needs none to unwind.
bool RunGuarded(void (*fn)(void*), void* ctx) {
  __try {
    fn(ctx);
    return true;
  } __except (Filter(GetExceptionInformation())) {
    return false;
  }
}
#else
bool RunGuarded(void (*fn)(void*), void* ctx) {
  fn(ctx);
  return true;
}
#endif

#ifdef _WIN32
Fault LastFault() { return g_fault; }
#else
Fault LastFault() { return {}; }
#endif

}  // namespace rexnative
