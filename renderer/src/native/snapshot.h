// A debug snapshot for a report, taken by the player where something looks wrong: the settings
// menu's Y (View + Menu, then Y on the Steam Deck), its Snapshot button, or `snapshot` in the
// control file. A few frames after the request (the menu gone), the frame as presented (a PNG)
// and, on the native renderer, a frame trace of the next frames (trace.h) go into <executable's folder>\snapshots\<date and
// time>\, with snapshot.txt (the frame, the time, the renderer that drew it). The newest
// kSnapshotsKept folders are kept.
//
// The PNG is what the player saw; the trace is what the game handed the renderer there, which
// `live.ps1 play` draws again on another machine with the same executable (a trace taken on the
// Steam Deck plays on the PC), without reaching the place. The ReXNative Manager sends both with
// a report.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace rexnative::snapshot {

constexpr int kSnapshotsKept = 5;

// Any thread. Ignored while one is being taken.
void Request(const char* why);

// The presenter, at each Swap (CaptureRequested): true once, for the frame to capture; it then
// starts the trace of the next frames. A snapshot that got no picture gives up after 4 s.
bool Due(uint64_t frame);
// A snapshot is waiting for its frame or being taken (its capture takes no Xenos copy and does
// not hold the guest thread, and the classic renderer's frames are captured too).
bool Pending();
bool Capturing();
// The snapshot's folder (while Capturing).
std::filesystem::path Dir();
// The presenter recorded the snapshot's frame (its PNG is written once the frame completes).
void Taken(uint64_t frame);

// For the HUD: the message about the last snapshot and the steady_clock ticks it was set at
// (0: none yet).
std::string Message(int64_t* at);

}  // namespace rexnative::snapshot
