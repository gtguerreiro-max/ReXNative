// The Linux build's guest memory watch (native/watch.h): none. The Windows one sets the debug
// registers of every thread through Win32's thread snapshots and contexts.
#include "native/watch.h"

#include <rex/logging.h>

namespace rexnative::watch {

void Set(uint32_t guest_address, const std::string&, uint32_t) {
  REXLOG_WARN("ReXNative: watch {:08X}: hardware breakpoints are Windows only", guest_address);
}

void Clear() {}

}  // namespace rexnative::watch
