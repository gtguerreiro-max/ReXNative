// ReXNative renderer: frame pacing on the display's clock (primary mode).
//
// Why. The game paces itself on guest vblanks: KickOffEx (sub_826CACA0) hands its presentation
// interval to the GPU, and ReXGlue raises the vblanks from a timer at --video_mode_refresh_rate,
// a clock beside the display's. The two drift apart: a frame that misses a tick by a little waits
// a whole tick (a doubled frame at any frame rate), and under a frame limit (Steam's, which is
// gamescope's) the game's time steps no longer match the intervals its frames are shown at. While
// the pacer runs the guest presents immediately (RexNativePresentationInterval) and the display is the
// only clock.
//
// The gate. Frame F + 1 begins once the display has shown frame F + 1 - L
// (--rexnative_frame_latency). Frames then begin in step with the display whatever its refresh
// rate, frame limit or VRR, and the game's time steps follow the intervals shown. L frames let the
// game thread, the render thread and the GPU each work on one; fewer leave the GPU idle between
// frames, and the Deck's GPU then lowers its clock.
//
// The delay (--rexnative_low_latency). The gate alone puts L display intervals between the game
// reading its input and the display showing the result, however short the pipeline: 67 ms under
// a 45 fps limit at L = 3 (td-111). So frame F + 1 begins a delay D after its gate opened, D being
// the frames' headroom less a margin (--rexnative_latency_margin_us): 31 ms then, with no frame
// shown late (td-118). This is DXVK's latency sleep (dxvk_latency_builtin.cpp) measured against
// the display, as the game cannot know the frame interval under gamescope's limiter.
// - A shown frame's headroom is the time from the GPU finishing it (the frame fence) to the
//   display showing it (its first present: a generated frame's, which the GPU finished with
//   it), plus the delay it began with. A frame shown n intervals late counts
//   against the slot it missed, n intervals earlier, and none counts below zero. A frame shown
//   more than kStallSlots slots late is a stall (a loading screen), not the pipeline's time: the
//   estimate starts over.
// - The estimate is held: the least headroom of the last kHeadroomWindowS seconds (in one-second
//   buckets), since the slowest frames come seconds apart (in the race at 90 fps, single frames
//   with twice the usual GPU time every 3-7 s, td-114). Released at a fixed rate instead (1 ms/s,
//   td-113 to td-119), the estimate took over a minute to recover from one hitch at 30 fps, where
//   the headroom spans three 33 ms intervals (td-121).
// - D follows it by at most kDelayUpNs per frame up and kDelayDownNs down, and only down for
//   kHeadroomFrames frames after the estimate started over, so the game's time steps stay within
//   a fraction of a millisecond of the intervals shown.
// - The interval is the shortest the display showed recently: the least median of three of the
//   last kIntervalFrames. Frames begun just late enough to fill the display's slots cannot tell
//   a frame limit from a pace they set themselves, so D probes: it eases down by half an interval,
//   kProbeStepNs per frame, and holds there for kProbeHoldFrames frames. Under a limit only the
//   latency changes, for a moment (20 ms for 2 s at 30 fps, td-122); without one the frames are
//   shown at the faster pace, and the interval follows it. D probes kProbePeriodNs after the pace
//   was set, then at twice the period before, up to kProbePeriodMaxNs; and at once when a frame
//   was shown sooner than the pace allows, as the first frames are once a limit is lifted.
//
// Automatic latency (SetFrameLatency's `automatic`, --rexnative_frame_latency_auto). L = 1 gives
// the least delay, but the game, the render thread and the GPU then work on one frame after the
// other: where their times add up to more than a display interval, frames miss their slot. The
// Steam Deck's player set heavy games to 2 by hand (Blur: 51 % of its 5 s windows slow at 1, 3 %
// at 2; Lazy Raiders 23.5 ms frames at 1 against 16.7 at 2), and wants a game that needs 2 to
// keep it: one that needed it will need it again, and trying 1 now and then only stutters. With
// L = 1 and `automatic`:
// - Trying 2: a game too slow at 1 misses every slot the same way, so the shown pace is its own
//   and nothing looks late (a frame limit looks the same). So the pacer tries: kAutoProbeFirstNs
//   after the estimate settled, then every kAutoProbePeriodNs (twice as late each time it did
//   not help, up to kAutoProbePeriodMaxNs), it sets 2 for kAutoProbeSettleNs +
//   kAutoProbeMeasureNs and compares the mean interval shown then with the kAutoProbeMeasureNs
//   before. A try costs one frame of latency for a few seconds, no stutter. kAutoMisses frames
//   the GPU finished after their slot, shown late within kAutoMissWindowNs (not a stall, nor the
//   first kHeadroomFrames after a start or a new pace), bring the next try forward to at once.
// - kAutoProbeGain faster at 2: 2, and once, kAutoConfirmNs later, 1 again to confirm it (a try
//   during a loading screen can look faster): kAutoProbeEarlyLoss slower at 1 after
//   kAutoProbeEarlyNs ends the try, and kAutoProbeSure slower over all it measured makes 2 the
//   game's for good (a second alone is noise: one 10 % slower ended a try 4.8 % slower), saved
//   in its settings file as the HUD would (TakeAutoDecision: the renderer writes it), so it
//   starts at 2 from then on; no later at 1 and it stays at 1, trying again as before.
//   Between the two (more than kAutoProbeKeep, less than kAutoProbeSure slower at 1) it stays 2
//   for this run only and confirms again later (twice as late each time, up to
//   kAutoProbePeriodMaxNs): a title screen or a menu a little slower at 1 decided for good what
//   the game itself does (Lazy Raiders' title 4 % slower at 1 one start and not the next, its
//   levels 50 %; Blur's menu 4.5 %, its races more).
//   The confirmation is the one stutter a game that needs 2 ever gets (about a second; a few
//   more where it was not sure).
// Neither the headroom nor a frame's own time from beginning to finished tells whether 1 would
// do: a game that waits inside its frame for its own pace (most 30 fps ones) leaves no slack.
// Only from 1 to 2: a setting of 2 or 3 is kept as it is.
//
// "Shown" is the swapchain's frame latency waitable object: vkd3d-proton releases it once per
// present when vkWaitForPresentKHR returns, but only in FIFO (with other present modes once the
// frame's blit finished, which is why the renderer presents with vsync while the pacer paces);
// DXGI when the frame leaves the present queue. A thread here waits on it and on the frame fence.
// A frame with a generated one before it (frame_generation.h) is shown with the second of its
// presents, and the display's interval is then that of every other refresh.
// A frame the render thread does not present (held for pipelines, or while ReXGlue's presenter
// has the window: a XAM dialog, the Xenos plugin) counts as shown at once (RetireUnshown).
//
// The swapchain comes and goes with the game window's owner (renderer.h, OwnGameWindow): the
// pacer counts across swapchains, each one's waitable bound from Attach to Detach.
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

#include "async_file.h"
#include "win_handle.h"  // windows.h

struct ID3D12Fence;

namespace rexnative {

class FramePacer {
 public:
  static constexpr uint32_t kMaxFrameLatency = 3;

  struct Options {
    // L above, at most kMaxFrameLatency; 0 only observes (the log), and the game keeps its
    // vblank pacing.
    uint32_t frame_latency = kMaxFrameLatency;
    bool low_latency = true;  // the delay D above
    uint64_t latency_margin_ns = 4'000'000;
    // When not null: one "present,frame,ready_ms,shown_ms,delay_ms" row per frame the display
    // showed (NowNs clock), `present` the number of its last present.
    std::unique_ptr<AsyncFile> log;
  };

  explicit FramePacer(Options options);
  ~FramePacer();
  FramePacer(const FramePacer&) = delete;
  FramePacer& operator=(const FramePacer&) = delete;

  uint32_t frame_latency() const { return latency_.load(std::memory_order_relaxed); }
  // Whether frames are paced: a swapchain is attached and L > 0.
  bool pacing() const { return waitable_ && frame_latency() > 0; }
  // Game thread, before WaitToBegin: L from now on (1-3; the settings HUD's Frame latency, live),
  // and with 1 whether the pacer may go to 2 while frames miss their slot (automatic latency
  // above). A change starts the delay estimate over, as for a new swapchain.
  void SetFrameLatency(uint32_t frames, bool automatic = false);
  // Game thread: true once when the automatic latency settled on 2 for this game; the caller
  // makes it the setting (rexnative_frame_latency = 2, saved in the game's settings file).
  bool TakeAutoDecision() { return auto_decided_.exchange(false, std::memory_order_acq_rel); }
  // The mean time from a frame beginning (the game reading its input) to the display showing it,
  // over the frames shown since the last call, in ms (0: none): the stats line's latency.
  double TakeShownLatencyMs();

  // Render thread. Attach: the swapchain the frames are presented to from here on, by its frame
  // latency waitable (D3D12Device::TakeFrameLatencyWaitable before its first Present; owned by
  // the pacer from here), and the fence the frames signal. Detach, once that swapchain was
  // released: its presents not counted yet are shown, and the waitable is closed.
  void Attach(HANDLE waitable, ID3D12Fence* fence);
  void Detach();
  // Render thread, once per Swap: the frame was presented, in `presents` presents (a generated
  // frame's before it, frame_generation.h) and signals `fence_value` once the GPU finished it, or
  // will not be presented (RetireUnshown). It is shown with its last present.
  void Presented(uint64_t fence_value, uint32_t presents = 1);
  void RetireUnshown();

  // Game thread, once per frame after the Swap queued it: blocks until the next frame may
  // begin. Returns the ns it waited.
  uint64_t WaitToBegin();
  // Opens the gate for good (the native path stopped); WaitToBegin returns at once from then on.
  void Stop();

 private:
  // A present the display does not complete (a minimized or occluded window, suspend) lets the
  // game go on at this pace instead of stopping it.
  static constexpr uint32_t kWaitTimeoutMs = 100;
  // Rings indexed by frame or present number: more than the frames begun but not retired (L) and
  // the presents not shown (at most L).
  static constexpr uint32_t kRing = 16;
  static constexpr uint32_t kIntervalFrames = 32;
  static constexpr uint32_t kIntervalMinFrames = 8;  // before IntervalLocked has an answer
  static constexpr uint32_t kHeadroomFrames = 64;
  static constexpr uint32_t kHeadroomWindowS = 10;
  static constexpr int64_t kStallSlots = 3;
  static constexpr int64_t kDelayUpNs = 500'000;
  static constexpr int64_t kDelayDownNs = 1'000'000;
  static constexpr uint64_t kProbePeriodNs = 10'000'000'000;
  static constexpr uint64_t kProbePeriodMaxNs = 80'000'000'000;
  static constexpr int64_t kProbeStepNs = 500'000;  // D's step down per frame while probing
  static constexpr uint64_t kProbeHoldFrames = 8;   // for the median of three to see a faster pace
  static constexpr uint64_t kNoLag = ~0ull;
  // Automatic latency (above).
  static constexpr uint32_t kAutoMisses = 4;
  static constexpr uint64_t kAutoMissWindowNs = 3'000'000'000;
  static constexpr uint64_t kAutoProbeSettleNs = 1'000'000'000;
  static constexpr uint64_t kAutoProbeMeasureNs = 3'000'000'000;
  static constexpr uint64_t kAutoProbeFirstNs = 5'000'000'000;  // the first try of 2, once settled
  static constexpr uint64_t kAutoProbePeriodNs = 15'000'000'000;
  static constexpr uint64_t kAutoProbePeriodMaxNs = 240'000'000'000;
  static constexpr double kAutoProbeGain = 0.95;  // the interval at 2 over the one at 1
  static constexpr uint64_t kAutoConfirmNs = 20'000'000'000;  // 2, then the try of 1 that confirms
  static constexpr uint64_t kAutoProbeEarlyNs = 1'000'000'000;  // that try: judged after
  static constexpr double kAutoProbeEarlyLoss = 1.10;  // that much slower ends it: 2 for good
  static constexpr double kAutoProbeKeep = 1.03;  // over the whole try, more than this: 2 for now
  static constexpr double kAutoProbeSure = 1.10;  // and more than this: 2 for good
  static constexpr uint32_t kShownRing = 512;       // shown times: 3 s at 144 fps and more

  struct Present {
    uint64_t frame = 0;   // the frame's number (Swaps since the pacer started, from 1)
    uint64_t number = 0;  // its last present's (presents since the pacer started, from 1)
    uint64_t fence = 0;
    uint64_t ready_ns = 0;  // when the pacer saw the fence reach `fence`; 0: not yet
    uint32_t presents = 1;  // the frame's: 2 with a generated frame before it
    uint32_t shown = 0;     // of them, shown
    uint64_t first_shown_ns = 0;
  };

  void Run();
  // mutex_ held. Frames `retired_` + 1 to `retired_` + n were shown or will not be.
  void RetireLocked(uint64_t n, uint64_t now_ns);
  void OnFenceLocked(uint64_t now_ns);
  // mutex_ held, per shown present: updates delay_ns_.
  void ControlLocked(const Present& p, uint64_t shown_ns);
  // mutex_ held, from ControlLocked: the automatic latency's step for a shown present that
  // `missed` slots of `interval`. True when it changed L (the control started over).
  bool AutoLatencyLocked(const Present& p, uint64_t shown_ns, int64_t missed, int64_t interval);
  // mutex_ held: the mean interval between the presents shown in the last `window_ns` (ms; 0
  // with fewer than 10).
  double MeanShownIntervalLocked(uint64_t now_ns, uint64_t window_ns) const;
  int64_t IntervalLocked() const;
  void ResetControlLocked();
  // Sleeps until `deadline_ns` (NowNs clock).
  void SleepUntil(uint64_t deadline_ns) const;

  std::atomic<uint32_t> latency_;  // L: written by the game thread (SetFrameLatency)
  // The setting (SetFrameLatency); L differs from it while the automatic latency tries 2 or 1.
  std::atomic<uint32_t> base_latency_;
  std::atomic<bool> automatic_{false};
  std::atomic<bool> auto_decided_{false};  // TakeAutoDecision
  enum class AutoState { kAt1, kTrying2, kAt2, kTrying1, kDone };
  AutoState auto_state_ = AutoState::kAt1;            // mutex_
  std::array<uint64_t, kAutoMisses> auto_miss_ns_{};  // mutex_: when the last misses were shown
  uint64_t auto_misses_ = 0;
  std::array<uint64_t, kShownRing> auto_shown_ns_{};  // mutex_: when presents were shown
  uint64_t auto_shown_count_ = 0;
  uint64_t auto_probe_end_ns_ = 0;   // while trying: when the try ends
  uint64_t auto_next_probe_ns_ = 0;  // the next try (0: not set yet)
  uint64_t auto_probe_period_ns_ = kAutoProbePeriodNs;
  uint64_t auto_confirm_period_ns_ = kAutoConfirmNs;  // the next confirmation of 2 when unsure
  double auto_probe_base_ms_ = 0;  // the mean interval before the try
  const bool low_latency_;
  const int64_t margin_ns_;
  const std::unique_ptr<AsyncFile> log_;
  UniqueHandle waitable_;           // render thread: the attached swapchain's
  ID3D12Fence* fence_ = nullptr;    // render thread: with waitable_
  const UniqueHandle thread_stop_;  // Detach ends the waiting thread with it
  const UniqueHandle fence_event_;  // set when the GPU finished the oldest present not seen ready
  std::thread thread_;              // waits on waitable_ and fence_event_ from Attach to Detach
  uint64_t swaps_ = 0;              // render thread: Presented and RetireUnshown calls
  uint64_t presents_count_ = 0;     // render thread: the presents of Presented calls
  uint64_t queued_ = 0;             // game thread: frames WaitToBegin was called for

  // Sleeps with sub-millisecond precision: Wine's NtDelayExecution sleeps in the process (a
  // waitable timer is a wineserver object there); on Windows it rounds up to the timer period
  // and a high-resolution waitable timer does not.
  using NtDelayExecutionFn = LONG(WINAPI*)(BOOLEAN alertable, const LARGE_INTEGER* timeout);
  NtDelayExecutionFn delay_execution_ = nullptr;
  UniqueHandle sleep_timer_;

  std::mutex mutex_;
  std::condition_variable retired_cv_;
  // Presents [shown_, presented_) are not shown yet, [ready_, presented_) not seen finished.
  std::array<Present, kRing> presents_{};
  uint64_t presented_ = 0, ready_ = 0, shown_ = 0;
  uint64_t armed_ = 0;    // the fence value fence_event_ was last set to wait for
  uint64_t retired_ = 0;  // frames shown or retired unshown
  std::array<uint64_t, kRing> retired_ns_{};  // by frame number: when retired_ reached it
  // By frame number: when it began (0: not yet), for TakeShownLatencyMs.
  std::array<uint64_t, kRing> begin_ns_{};
  uint64_t shown_latency_sum_ns_ = 0, shown_latency_count_ = 0;  // mutex_
  // By frame number: how long after its gate opened the frame began (kNoLag: no gate).
  std::array<uint64_t, kRing> lag_ns_{};
  bool stopped_ = false;
  bool timed_out_ = false;
  // Latency control, per shown present.
  int64_t delay_ns_ = 0;                         // D
  uint64_t last_frame_ = 0, last_shown_ns_ = 0;  // the last shown present's
  std::array<int64_t, kIntervalFrames> intervals_{};
  uint64_t interval_count_ = 0;
  int64_t interval_ns_ = 0;      // IntervalLocked when the headroom was last reset
  int64_t headroom_ns_ = 0;      // the held headroom
  uint64_t headroom_count_ = 0;  // frames since it was reset
  // The least headroom shown in each of the last kHeadroomWindowS seconds, by second.
  std::array<int64_t, kHeadroomWindowS> headroom_by_s_{};
  uint64_t headroom_s_ = 0;  // the second the last headroom was shown in
  uint64_t probe_at_ns_ = 0, probe_end_frame_ = 0;
  uint64_t probe_period_ns_ = kProbePeriodNs;
  int64_t probe_delay_ns_ = 0;  // D's ceiling until probe_end_frame_
};

}  // namespace rexnative
