// Forza Horizon, title update 4 (title 4D5309C9, module 336A44DC03EC8782): each frame shows one
// step of the game's simulation. Written into the game's src/title_hooks.cpp by ReXNative
// (rexnative/title_hooks.py); the hooks' addresses are in hooks.toml beside this file.
//
// The game steps its simulation once per raise of an event, which its vblank callback raises
// every second vblank whatever its render thread is doing; the render thread draws the newest step
// finished. A frame that takes longer than two vblanks (the Steam Deck's GPU near the 33 ms of a
// 30 fps frame) lets the simulation run ahead: one frame shows two steps of motion and the next
// none, at a flat 30 fps. The motion stutters while the frame rate and the frames' times on the
// display stay even (Forza Horizon on the Deck: every frame shown two refreshes after the last).
// The gate holds a raise while the simulation is two steps ahead of the frames swapped: the
// callback's vblank count stays as it is, so its next vblank raises the step once the frame is in.
// Steps that keep up stay on their vblank.
//
// After Pinyon Shift's step backpressure (issue #381: PinyonShiftGateSimulationStep in
// src/pinyon_shift_runtime_hooks.cpp, https://github.com/arcanite24/pinyon-shift), which raises a
// held step at the render thread's next swap; here the next vblank raises it, with no call into the
// kernel. Pinyon Shift's notice (BSD 3-Clause License, THIRD_PARTY_NOTICES.md):
//   Copyright (c) 2026, Pinyon Shift contributors

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform/sync.h>
#include <rex/ppc/context.h>

REXCVAR_DEFINE_INT32(rexnative_render_poll_us, 300, "ReXNative",
                     "Forza Horizon: how long the render thread sleeps (us) each time it finds no "
                     "new simulation step to draw, before its SleepEx(0) and next look; 0 polls "
                     "as on the console. Where the platform sleeps that short with no system "
                     "object (Wine); elsewhere nothing changes (src/title_hooks.cpp)");
REXCVAR_DEFINE_BOOL(rexnative_step_gate, true, "ReXNative",
                    "Forza Horizon: hold a simulation step while the frames are two behind, so "
                    "each frame shows one step of motion (src/title_hooks.cpp)");
REXCVAR_DEFINE_BOOL(rexnative_step_log, false, "ReXNative",
                    "Forza Horizon: log every 10 s how many simulation steps each frame showed "
                    "and how often the gate held one (src/title_hooks.cpp)");

// The renderer's (rexnative_d3d_hooks.cpp).
uint64_t RexNativeTitleSwaps();
void RexNativeSetTitleSwapHook(void (*hook)());

namespace {

using Clock = std::chrono::steady_clock;

// Steps the main loop began (the step hook), and the gate's counts for the log.
std::atomic<uint64_t> g_steps{0};
std::atomic<uint32_t> g_holds{0}, g_timeouts{0};
// The longest a step waits for a frame: a render thread that takes no steps (a load) does not
// stop the game.
constexpr auto kMaxHold = std::chrono::milliseconds(50);

// At each Swap, on the game's render thread: the steps each frame showed, logged with
// rexnative.step_log.
void OnSwap() {
  if (!REXCVAR_GET(rexnative_step_log)) return;
  static uint64_t last_steps = 0;
  static uint32_t shown[4] = {};
  static Clock::time_point report = Clock::now();
  const uint64_t steps = g_steps.load(std::memory_order_acquire);
  ++shown[std::min<uint64_t>(steps - last_steps, 3)];
  last_steps = steps;
  const Clock::time_point now = Clock::now();
  if (now - report < std::chrono::seconds(10)) return;
  report = now;
  REXLOG_INFO("ReXNative step gate {}: frames showing 0 steps {}, 1 step {}, 2 steps {}, 3 or more "
              "{}; steps held {}, held too long {}",
              REXCVAR_GET(rexnative_step_gate) ? "on" : "off", shown[0], shown[1], shown[2],
              shown[3], g_holds.exchange(0), g_timeouts.exchange(0));
  std::fill(std::begin(shown), std::end(shown), 0u);
}

const bool g_registered = [] {
  RexNativeSetTitleSwapHook(&OnSwap);
  return true;
}();

}  // namespace

// 0x8241DC44: the main loop has its step's time in f31: a step begins.
void rexnative_fh_step_begun(PPCRegister& f31) {
  (void)f31;
  g_steps.fetch_add(1, std::memory_order_release);
}

// 0x824ABC50, on the vblank callback (one thread): r11 not 0 raises the step event. Steps begun
// less frames swapped is 1 while the frames keep up (the step drawn next is under way); at 2 the
// frames fell behind, and the raise waits.
void rexnative_fh_step_raise(PPCRegister& r11) {
  static int64_t base = 0;  // steps less swaps when they last lined up
  static Clock::time_point held_since{};
  if ((r11.u32 & 0xFF) == 0 || !REXCVAR_GET(rexnative_step_gate)) return;
  int64_t lead = int64_t(g_steps.load(std::memory_order_acquire)) -
                 int64_t(RexNativeTitleSwaps()) - base;
  if (lead < 0) {
    // Frames swapped with no step (a loading screen, the pause menu): level again.
    base += lead;
    lead = 0;
  }
  if (lead >= 2) {
    const Clock::time_point now = Clock::now();
    if (held_since == Clock::time_point{}) {
      held_since = now;
      g_holds.fetch_add(1, std::memory_order_relaxed);
    }
    if (now - held_since < kMaxHold) {
      r11.u64 = 0;  // no raise now; the vblank count stays, the next vblank decides again
      return;
    }
    // The render thread takes no steps: this one runs, counted from the usual one ahead.
    base += lead - 1;
    g_timeouts.fetch_add(1, std::memory_order_relaxed);
  }
  held_since = {};
}

// 0x82616CD0, on the render thread: no newer simulation step to draw, SleepEx(0, TRUE) next, then
// its main loop looks again. That poll ran about 12,000 times a second on the Steam Deck, a whole
// core of four, beside the simulation thread it waits for. A sleep of rexnative.render_poll_us
// first (a futex in the process under Wine: rex::platform::SleepOnWord) lets it look about 3,000
// times a second, seeing a new step at most that late; nothing wakes it early. On a platform with
// no such sleep (Windows rounds it up to its timer period) it polls as before.
void rexnative_fh_render_idle(PPCRegister& r3) {
  (void)r3;
#ifdef REX_PLATFORM_HAS_SLEEP_ON_WORD
  const int32_t us = REXCVAR_GET(rexnative_render_poll_us);
  if (us <= 0) return;
  static const std::atomic<uint32_t> never{0};
  (void)rex::platform::SleepOnWord(never, 0, std::chrono::microseconds(std::min(us, 2000)));
#endif
}
