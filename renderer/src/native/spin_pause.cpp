// ReXNative: rexnative_spin_pause (spin_pause.h), the recompiled code's db16cyc. A pause, as
// before; once one thread's run of pauses has gone on for kSpinLimit with no gap (a wait, not a
// short spin lock), it sleeps kSleep every kEvery pauses, so the loop still checks its condition
// often and wakes within about that long of it. REXNATIVE_SPIN_SLEEP=0 in the environment keeps
// the plain pause (to compare).
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <time.h>
#endif

#include <immintrin.h>

namespace {

using Clock = std::chrono::steady_clock;

// Pauses closer than this together are one wait.
constexpr auto kGap = std::chrono::microseconds(50);
// A wait longer than this sleeps.
constexpr auto kSpinLimit = std::chrono::microseconds(200);
constexpr auto kSleep = std::chrono::microseconds(100);
constexpr uint32_t kEvery = 32;

bool Enabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("REXNATIVE_SPIN_SLEEP");
    return !(v && v[0] == '0');
  }();
  return enabled;
}

void SleepBriefly() {
#if defined(_WIN32)
  // A high resolution waitable timer per thread: Sleep(1) would take a millisecond or more.
  thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
                                                     CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                     TIMER_ALL_ACCESS);
  if (timer) {
    LARGE_INTEGER due;
    due.QuadPart = -int64_t(kSleep.count() * 10);
    if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
      WaitForSingleObject(timer, INFINITE);
      return;
    }
  }
  SwitchToThread();
#else
  timespec ts = {0, long(std::chrono::nanoseconds(kSleep).count())};
  nanosleep(&ts, nullptr);
#endif
}

}  // namespace

extern "C" void rexnative_spin_pause(void) {
  _mm_pause();
  if (!Enabled()) return;
  thread_local Clock::time_point start, last;
  thread_local uint32_t count = 0;
  const Clock::time_point now = Clock::now();
  if (now - last > kGap) {
    start = now;
    count = 0;
  }
  last = now;
  if (now - start > kSpinLimit && ++count % kEvery == 0) {
    SleepBriefly();
    last = Clock::now();
  }
}
