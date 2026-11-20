// ReXNative renderer: what a graphics debugger needs from the renderer (docs/DEBUGGING.md).
//
// RenderDoc. --rexnative_renderdoc=auto (or RenderDoc's folder) loads renderdoc.dll before the
// renderer's D3D12 device is made, so a game needs no launcher: only the renderer's device is
// captured, not the Xenos plugin's. A game started from RenderDoc has the library already, and
// it is used as it is. Captures are whole guest frames, from the Swap before to this one's last
// Present, each in its own file <dir>\<game>-f<frame>.rdc, listed in <dir>\captures.txt:
//   --rexnative_renderdoc_frames=N   the next N frames (live: `set rexnative_renderdoc_frames N`
//                                    in the control file, or the key bind_renderdoc, F6)
//   --rexnative_renderdoc_frame=N    guest frame N (a scripted run)
//   --rexnative_renderdoc_on_mark    each mark (F12, capture.request) also takes the frame after
//
// Events and names (the events in the frames RenderDoc captures, the names while it is loaded;
// both always with --rexnative_markers, for PIX and the debug layer): each frame is a tree of passes (the guest's render targets: EDRAM base, format, MSAA), with
// one event per guest draw named by its index in the frame (the draw log's and draw-debug.txt's),
// primitive, count and shader hashes, and one per clear, resolve (source and destination) and
// Swap. Textures, render targets and resolve images are named by guest address, format and size.
//
// Threading: Pass, BeginEvent, EndEvent and CloseEvents on the thread that records (the render
// thread); StartCapture and EndCapture on the command stream's thread; the rest on any.
#pragma once

#include <atomic>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

struct ID3D12Device;
struct ID3D12Object;

namespace rexnative {
class CommandStream;
struct ResolveParams;
struct SurfaceRegs;
}  // namespace rexnative

namespace rexnative::dbg {

// Before the D3D12 device is created: loads RenderDoc when asked to and takes its API.
void Initialize();
// Whether RenderDoc's API is there (captures can be taken).
bool RenderDocLoaded();

namespace detail {
extern std::atomic<bool> g_markers, g_names;
void SetName(ID3D12Object* object, std::string_view name);
}  // namespace detail

// Resources are named (RenderDoc loaded, or --rexnative_markers).
inline bool Names() { return detail::g_names.load(std::memory_order_relaxed); }
// Events are recorded: in the frames RenderDoc captures, or in all with --rexnative_markers (a
// string and two calls a draw are not free in a frame of thousands).
inline bool Markers() { return detail::g_markers.load(std::memory_order_relaxed); }

// Names a resource (ID3D12Object::SetName) while Names(); the arguments are not evaluated into
// a string otherwise.
template <class... Args>
void Name(ID3D12Object* object, std::format_string<Args...> format, Args&&... args) {
  if (object && Names()) [[unlikely]]
    detail::SetName(object, std::format(format, std::forward<Args>(args)...));
}

// Events on the frame's list. A list is submitted with none open: CloseEvents ends them all
// (D3D12Device, before each submission), and an EndEvent after it does nothing.
void BeginEvent(CommandStream* list, std::string_view name);
void EndEvent(CommandStream* list);
void CloseEvents(CommandStream* list);
// The draw's pass: a new event when the render targets differ from the pass open (or none is).
void Pass(CommandStream* list, const SurfaceRegs& regs);
// Ends the pass open, if any (a resolve, a Swap).
void EndPass(CommandStream* list);
// Scope of an event; with Markers() off, nothing.
class Event {
 public:
  template <class... Args>
  Event(CommandStream* list, std::format_string<Args...> format, Args&&... args) {
    if (Markers()) [[unlikely]] {
      list_ = list;
      BeginEvent(list, std::format(format, std::forward<Args>(args)...));
    }
  }
  ~Event() {
    if (list_) EndEvent(list_);
  }
  Event(const Event&) = delete;
  Event& operator=(const Event&) = delete;

 private:
  CommandStream* list_ = nullptr;
};
// "resolve c0 edram 0 k_8_8_8_8 (0,0)-(1280,720) -> 1F3A0000 k_8_8_8_8 1280x720".
std::string DescribeResolve(const ResolveParams& p);

// Render thread, at the end of guest frame `frame` (its Swap), `marked` when the frame was a
// mark: whether RenderDoc captures frame + 1.
bool CaptureNext(uint64_t frame, bool marked);
// The command stream's thread: around a captured frame. EndCapture names the file after the
// game and the guest frame and logs it.
void StartCapture(ID3D12Device* device, uint64_t frame);
void EndCapture(ID3D12Device* device, uint64_t frame);

}  // namespace rexnative::dbg
