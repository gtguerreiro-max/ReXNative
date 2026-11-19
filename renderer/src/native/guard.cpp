#include "guard.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace rexnative {

#ifdef _WIN32
namespace {
int Filter(unsigned long code) {
  // ReXGlue's vectored handler has seen it first (MMIO pages): what reaches here is a real fault.
  return code == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                            : EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace

// No C++ object in this frame: __try needs none to unwind.
bool RunGuarded(void (*fn)(void*), void* ctx) {
  __try {
    fn(ctx);
    return true;
  } __except (Filter(GetExceptionCode())) {
    return false;
  }
}
#else
bool RunGuarded(void (*fn)(void*), void* ctx) {
  fn(ctx);
  return true;
}
#endif

}  // namespace rexnative
