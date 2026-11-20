// ReXNative renderer: the previous frame's constants of each draw, for the motion
// vector pass of the upscaler's scene (DxbcShaderTranslator's motion_vectors).
//
// A draw is matched with the previous frame's by its identity (its vertex shader, index buffer
// and index count: the draws of a frame keep these across frames, while per-frame vertex streams
// such as instance transforms move) and its place in the frame's order. The race draws its scene
// in the same order frame after frame (2520 of 2522 draws matched in td-265's draw logs), so
// each draw looks at the previous frame's draws within kWindow on either side of the one after
// the last matched, not matched yet: draws that appear, vanish or swap places with a neighbour
// (culling, sorting) cost a few compares and leave the rest in step.
//
// A draw with no match, a mesh that came into view or changed its level of detail (1 to 3 % of
// the race's, td-529), would read its own constants and show no motion at all while the camera
// moves, a block of the road or a building that FSR smears where it stood (dark boxes over the
// road in the 20261029-205920 session). It moves with the camera instead (Camera).
//
// Many draws share an identity: the copies of one mesh along the track (barrier sections,
// chevron boards, kerbs), told apart by their constants alone. Taking the first such draw in
// order pairs every copy after one that came into or went out of view with its neighbour's
// transform, meters off (12 draws a frame in the draw logs of the 20261029-205920 session),
// and FSR smears those surfaces with the wrong motion. Among several, a draw takes the one with
// the most float constants bit for bit its own: a copy's world transform stays the same from
// frame to frame where its neighbours' differ, while the camera's constants change alike for
// all (in that session's logs every pair this picks has the transform bit for bit); ties keep
// the order.
//
// Some games batch the copies, several to a draw, and fill the batches' slots anew every frame:
// Sega Rally Revo draws its trees four to a draw, each one's matrix in its own registers (c7-c9,
// c23-c25, c39-c41, c55-c57), sorted again each frame, so the tree in a draw's fourth slot was in
// another's first the frame before (2026-11-19 capture). No draw of the previous frame then holds
// this one's transforms, and the one matched gave its trees motion of hundreds of pixels on a
// still view: frame generation drew them where they were not, every other frame, a flicker.
// Registers the same in two copies of the previous frame are the frame's (the camera's,
// its time); the others are each copy's own. When the copy matched has its own registers far
// from this draw's (OtherCopies), it holds other objects: the draw moves with the camera instead,
// its own registers kept and the frame's at their previous values, as static copies do.
//
// Draws of one identity are also the tiles of a terrain: Sega Rally Revo draws its track's ground
// in tiles of a few index counts (its levels of detail), all of one index buffer and the same
// constants (the tiles' vertices are in world space), each tile's vertices in a buffer of its
// own. When two draws earlier in the frame came or went, each tile took the one two places
// before it in the previous frame, its neighbour: motion vectors read from another tile's
// vertices, tens to hundreds of pixels off in bands across the road (2026-11-20 motion logs:
// 33 of 1500 draws a frame), which frame generation drew as smears below the car. So a
// previous draw with this one's vertex buffer (its first binding's base and first vertex) comes
// first among those of its identity. Without one, in a game whose matches nearly all keep their
// buffer (stable_), the draw found is another mesh (a tile in another's place: one that came
// into view or changed its level of detail, its vertices in a buffer of each level's), and this
// draw moves with the camera (Camera) instead of taking that mesh's vertices, unless both first
// vertices are known and apart: their distance then tells another mesh from this one moved (the
// anchor limit below), as for Jet Set Radio. Otherwise told apart by nothing else from a draw that
// writes its vertices anew each frame (the ground beside a
// building in the same race), which the camera also moves exactly while it stands still; a
// moving one of those loses its own motion, where another tile's vertices put motion vectors
// hundreds of pixels off. Where most draws write their vertices anew (Jet Set Radio's ring of
// vertices), the draw found stays.
//
// The frames' draws are kept in order, in arrays read and written front to back, which a
// hash table probed at random spots for every draw is not (td-499 perf: three cache misses a
// draw, 6 % of the render thread). The constants stay in the upload memory they were committed
// to, which the constant pool keeps for one more frame (UploadPool::set_keep_previous_frame);
// the float constants are also copied, once per upload, into an array of the frame's, as the
// upload memory is write-combined and slow to read.
//
// Threading: the thread that records the draws.
#pragma once

#include <d3d12.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rexnative {

class MotionHistory {
 public:
  struct Constants {
    const void* vs = nullptr;  // the shader the float constants are packed for
    D3D12_GPU_VIRTUAL_ADDRESS float_constants = 0, fetch_constants = 0;
    // The draw's first vertex (VGT_INDX_OFFSET) and the first dword (base address and type) of
    // the vertex fetch constants of its shader's first kVertexBases bindings: where its vertices
    // were (DrawRenderer's BindPreviousConstants).
    uint32_t vertex_index_offset = 0;
    static constexpr uint32_t kVertexBases = 8;  // more bindings than that: the rest not moved
    uint32_t vertex_base[kVertexBases] = {};
    // What its first vertex had in its first binding: the first attribute of 3 or 4 floats (x,
    // y, z: its position) and the first of 2 (a texture coordinate), 0 for one it lacks.
    // Between previous draws Match finds alike, the nearest is the same mesh where the game
    // wrote its vertices transformed (Jet Set Radio: a crowd of draws of one shader and index
    // count, same constants, told apart by nothing else; its overlays' quads by their
    // positions, or by their texture coordinates where those are all the first vertex differs in).
    static constexpr int kAnchor = 5;
    float anchor[kAnchor] = {};
    bool anchor_valid = false;
  };
  // How the last Match found the previous constants.
  enum class Found {
    kNone,    // not at all
    kOrder,   // the draw of its identity next in order
    kCopy,    // among copies of its mesh, the one with its constants (another than the order's)
    kNearest, // among draws as alike, the one whose first vertex was nearest (Constants::anchor)
    kCamera,  // none: camera_floats(), its own with the camera's of the previous frame
  };

  MotionHistory() {
    current_.reserve(kReserved);
    previous_.reserve(kReserved);
    current_floats_.reserve(kReservedFloats);
    previous_floats_.reserve(kReservedFloats);
  }

  // Frame `frame`'s first draw: the last frame's draws become the previous frame's, if that was
  // frame - 1 (the constant pool keeps one frame's memory, drawn or not).
  void BeginFrame(uint64_t frame) {
    std::swap(current_, previous_);
    std::swap(current_floats_, previous_floats_);
    stable_ = buffer_matches_ >= 64 && same_buffer_matches_ * 10 >= buffer_matches_ * 9;
    buffer_matches_ = same_buffer_matches_ = 0;
    current_.clear();
    current_floats_.clear();
    if (frame != frame_ + 1) {
      previous_.clear();
      previous_floats_.clear();
    }
    frame_ = frame;
    cursor_ = 0;
    by_loose_built_ = false;
    // The limit on a match's anchor distance (Match): 4 times the lower quartile of the last
    // frame's distances, matched or refused (a camera's move over most draws; the quartile
    // stays below the mismatches, which were a quarter to a half of Jet Set Radio's), at least
    // kAnchorMin. No limit before a frame with anchors.
    if (!distances_.empty()) {
      auto q = distances_.begin() + distances_.size() / 4;
      std::nth_element(distances_.begin(), q, distances_.end());
      anchor_limit_ = std::max(kAnchorMin, 4.0f * *q);
    } else {
      anchor_limit_ = std::numeric_limits<float>::infinity();
    }
    distances_.clear();
    // The global search from the first frame whose draws the window mostly missed on (it then
    // finds them, so the window's misses no longer tell).
    global_ = global_ || (draws_ >= 64 && misses_ * 5 > draws_);
    draws_ = misses_ = 0;
  }

  // This frame's next draw, of `identity` with the constants `now`, `floats` the `size` bytes
  // (whole registers) of float constants at now.float_constants: the previous frame's constants
  // for it, or nullptr. Valid until the next Match.
  // `loose`: the identity without what a game may change every frame for the same mesh (the
  // index buffer's address: Jet Set Radio writes its geometry into a ring each frame, so 60 % of
  // its draws found no draw of their identity and took the camera fallback, which reads another
  // draw's matrices in its shaders of one combined transform: FSR smeared its cars and outlines).
  // A draw first looks for its identity, then for its loose one.
  const Constants* Match(uint64_t identity, uint64_t loose, const Constants& now,
                         const uint8_t* floats, uint32_t size) {
    Draw d = {identity, loose, now, 0, size, kNone, false};
    ++draws_;
    if (!current_.empty() && current_.back().constants.float_constants == now.float_constants &&
        current_.back().size == size) {
      d.offset = current_.back().offset;  // the same upload
    } else {
      d.offset = uint32_t(current_floats_.size());
      current_floats_.insert(current_floats_.end(), floats, floats + size);
    }
    current_.push_back(d);

    const size_t n = previous_.size();
    size_t best = n, first = n;
    uint32_t best_same = 0;
    bool several = false;
    bool use_loose = false;
    bool nearest = false;  // best taken over an equal one for its anchor
    size_t other = n;      // another candidate than the first (OtherCopies)
    bool best_buffer = false;  // best has this draw's vertex buffer
    auto consider = [&](size_t i) {
      const Draw& p = previous_[i];
      if ((use_loose ? p.loose != loose : p.identity != identity) || p.matched) return;
      const bool buffer = buffers_ && SameBuffer(p.constants, now);
      if (best == n) {
        best = first = i;
        best_buffer = buffer;
        return;
      }
      if (other == n) other = i;
      if (!several) {
        several = true;
        best_same = SameRegisters(previous_[best], floats, size);
      }
      // This draw's own vertices first (the header: a terrain's tiles).
      if (buffer != best_buffer) {
        if (buffer) {
          best = i;
          best_buffer = true;
          best_same = SameRegisters(p, floats, size);
          nearest = false;
        }
        return;
      }
      const uint32_t same = SameRegisters(p, floats, size);
      if (same > best_same) {
        best = i;
        best_same = same;
        nearest = false;
      } else if (same == best_same && Nearer(now, p.constants, previous_[best].constants)) {
        best = i;
        nearest = true;
      }
    };
    ForWindow(consider);
    if (best == n && loose != identity) {
      use_loose = true;
      ForWindow(consider);
    }
    if (best == n) ++misses_;
    if (best == n && global_) {
      // Anywhere in the previous frame: a game whose draws the window mostly misses (Jet Set
      // Radio: its geometry rewritten into a ring and sorted, 55 % of its draws a frame) moves
      // them farther than the window from frame to frame. Not where the window finds nearly all
      // (PGR4, Iron Brigade: 1 to 3 % missed, meshes that came into view, for which a copy of
      // the mesh elsewhere would be the wrong motion and the camera's is exact).
      use_loose = true;
      if (!by_loose_built_) {
        by_loose_.clear();
        for (size_t i = 0; i < n; ++i) by_loose_[previous_[i].loose].push_back(uint32_t(i));
        by_loose_built_ = true;
      }
      if (auto it = by_loose_.find(loose); it != by_loose_.end())
        for (uint32_t i : it->second) consider(i);
    }
    far_ = compared_ = false;
    last_refusal_ = 0;
    if (buffers_ && best != n && !best_buffer && now.vertex_base[0] && stable_ &&
        !(now.anchor_valid && previous_[best].constants.anchor_valid &&
          Distance2(now, previous_[best].constants) > 0)) {
      // Another buffer's vertices, in a game whose meshes keep their buffers: another mesh (a
      // tile in another's place). With both first vertices known and apart, their distance
      // tells (below); the same first vertex in two buffers tells nothing (Sega Rally Revo's
      // ground tiles all start at the same corner, their place in another binding).
      Camera(now, floats, size);
      if (found_ == Found::kCamera) {
        ++other_buffers_;
        last_refusal_ = 1;
        return nullptr;
      }
      found_ = Found::kNone;  // no camera without a matched draw of the shader: that draw
      last_refusal_ = 2;
    }
    if (best == n) return Camera(now, floats, size);
    if (now.vertex_base[0]) {
      ++buffer_matches_;
      same_buffer_matches_ += best_buffer;
    }
    // Copies batched into draws whose slots are filled anew each frame (the header): the draw
    // matched holds other objects; the camera's motion instead.
    if (several && other != n && OtherCopies(floats, size, best, other)) {
      ++batch_shuffles_;
      cursor_ = best + 1;  // the window stays in step with the frame's order
      return nullptr;
    }
    // A draw whose first vertex was far from this one's is another mesh, not this one moved: Jet
    // Set Radio clips its polygons on the CPU, so the road's index count changes from frame to
    // frame, and the nearest draw of this frame's count was a building a third of the screen
    // away (its motion vectors, 27 % of a frame's pixels off the camera's by over 256 pixels,
    // made FSR smear the road). Such a draw moves with the camera instead.
    if (now.anchor_valid && previous_[best].constants.anchor_valid) {
      const float d = std::sqrt(Distance2(now, previous_[best].constants));
      distances_.push_back(d);
      compared_ = true;
      if (d > anchor_limit_) {
        far_ = true;
        return Camera(now, floats, size);
      }
    }
    found_ = !several || best == first ? Found::kOrder
             : nearest                  ? Found::kNearest
                                        : Found::kCopy;
    previous_[best].matched = true;
    current_.back().previous = uint32_t(best);
    cursor_ = best + 1;
    last_index_ = best;
    return &previous_[best].constants;
  }
  Found found() const { return found_; }
  // Draws this frame matched with copies holding other objects (OtherCopies), and resets it.
  uint32_t TakeBatchShuffles() { return std::exchange(batch_shuffles_, 0); }
  // The last Match's previous draw, its place in the previous frame (debugging), and the frame's.
  size_t last_index() const { return last_index_; }
  size_t previous_count() const { return previous_.size(); }
  bool global() const { return global_; }
  bool stable() const { return stable_; }
  // Debugging (kDebugLogMotion): the last Match's refusal of another mesh's vertex buffer (0
  // none, 1 the camera instead, 2 no camera: that draw kept).
  uint32_t last_refusal() const { return last_refusal_; }
  // Whether matches are told apart by their vertex buffers (the header; on unless debugging).
  void set_buffers(bool on) { buffers_ = on; }
  // Draws this frame sent to the camera instead of another mesh's vertex buffer, and resets it.
  uint32_t TakeOtherBuffers() { return std::exchange(other_buffers_, 0); }
  bool refused() const { return far_; }  // the last Match refused the nearest draw: too far
  bool compared() const { return compared_; }
  float anchor_limit() const { return anchor_limit_; }
  // Found::kCamera: the float constants of the previous frame for the last Match's draw, its
  // size bytes. Valid until the next Match.
  const std::vector<uint8_t>& camera_floats() const { return camera_floats_; }

 private:
  static constexpr size_t kWindow = 32;
  static constexpr size_t kReserved = 8192;  // twice the most draws in a race frame
  static constexpr size_t kReservedFloats = size_t(8) << 20;
  static constexpr uint32_t kNone = UINT32_MAX;
  struct Draw {
    uint64_t identity;
    uint64_t loose;  // Match's loose identity
    Constants constants;
    uint32_t offset, size;  // its float constants in the frame's array
    uint32_t previous;      // current_: the draw of previous_ it matched, or kNone
    bool matched;           // previous_: taken by a draw of this frame
  };

  // `f` for each draw of previous_ within kWindow of cursor_, those after it first.
  template <typename F>
  void ForWindow(F&& f) const {
    const size_t n = previous_.size();
    for (size_t i = cursor_, end = std::min(n, cursor_ + kWindow); i < end; ++i) f(i);
    for (size_t i = cursor_, end = cursor_ > kWindow ? cursor_ - kWindow : 0; i-- > end;) f(i);
  }

  // A draw's vertices: its first binding's buffer and its first vertex.
  static uint64_t BufferKey(const Constants& c) {
    return (uint64_t(c.vertex_base[0]) << 32) | c.vertex_index_offset;
  }
  static bool SameBuffer(const Constants& a, const Constants& b) {
    return a.vertex_base[0] == b.vertex_base[0] && a.vertex_index_offset == b.vertex_index_offset;
  }
  static float Distance2(const Constants& a, const Constants& b) {
    float d = 0;
    for (int k = 0; k < Constants::kAnchor; ++k)
      d += (a.anchor[k] - b.anchor[k]) * (a.anchor[k] - b.anchor[k]);
    return d;
  }
  // Whether `a`'s first vertex was nearer to `now`'s than `b`'s (all three known).
  static bool Nearer(const Constants& now, const Constants& a, const Constants& b) {
    if (!now.anchor_valid || !a.anchor_valid || !b.anchor_valid) return false;
    return Distance2(a, now) < Distance2(b, now);
  }

  // The registers (16 bytes each) of the previous frame's draw `p` the same as `floats`.
  uint32_t SameRegisters(const Draw& p, const uint8_t* floats, uint32_t size) const {
    if (p.size != size) return 0;
    const uint8_t* q = previous_floats_.data() + p.offset;
    uint32_t same = 0;
    for (uint32_t i = 0; i < size; i += 16) same += std::memcmp(q + i, floats + i, 16) == 0;
    return same;
  }

  // Whether the previous frame's copy `best` holds other objects than this draw (`floats`):
  // its own registers (those that differ between `best` and the copy `other` of the same frame;
  // the ones the same in both are the frame's) far from this draw's in at least 3 components
  // (more than 0.5 apart and a quarter of their size: another object's matrix, where an object
  // that moved changes by less in a frame). Then camera_floats() is this draw's constants with
  // the frame's registers at their previous values (Found::kCamera).
  bool OtherCopies(const uint8_t* floats, uint32_t size, size_t best, size_t other) {
    const Draw& b = previous_[best];
    const Draw& o = previous_[other];
    if (b.size != size || o.size != size) return false;
    const uint8_t* then = previous_floats_.data() + b.offset;
    const uint8_t* copy = previous_floats_.data() + o.offset;
    uint32_t apart = 0;
    for (uint32_t i = 0; i + 4 <= size && apart < 3; i += 4) {
      if (!std::memcmp(then + i, copy + i, 4)) continue;  // the frame's
      float a, t;
      std::memcpy(&a, floats + i, 4);
      std::memcpy(&t, then + i, 4);
      const float d = std::fabs(a - t);
      if (std::isfinite(d) && d > 0.5f && d > 0.25f * std::max(std::fabs(a), std::fabs(t))) ++apart;
    }
    if (apart < 3) return false;
    camera_floats_.assign(floats, floats + size);
    for (uint32_t r = 0; r < size; r += 16)
      if (!std::memcmp(then + r, copy + r, 16)) std::memcpy(camera_floats_.data() + r, then + r, 16);
    found_ = Found::kCamera;
    return true;
  }

  // A draw without one of its identity in the previous frame (a mesh that came into view or
  // changed its level of detail) moves with the camera: camera_floats() is its own float
  // constants with those the last matched draw of its shader changed since the previous frame
  // (the camera's, and anything else of the frame's, such as its time) at their previous
  // values, and its own others (its transform). Exact for static geometry, which the track's
  // new draws are; a moving object's own motion is missing for its first frame. nullptr, with
  // Found::kCamera, or Found::kNone without such a draw or if that draw changed every register
  // (a moving object's, nothing of the frame's told apart from its own).
  const Constants* Camera(const Constants& now, const uint8_t* floats, uint32_t size) {
    found_ = Found::kNone;
    const Draw* ref = nullptr;
    for (size_t i = current_.size() - 1; i-- > 0;) {
      const Draw& c = current_[i];
      if (c.constants.vs == now.vs && c.size == size && c.previous != kNone) {
        ref = &c;
        break;
      }
    }
    if (!ref) return nullptr;
    const uint8_t* ref_now = current_floats_.data() + ref->offset;
    const uint8_t* ref_then = previous_floats_.data() + previous_[ref->previous].offset;
    camera_floats_.assign(floats, floats + size);
    bool kept = false;
    for (uint32_t r = 0; r < size; r += 16) {
      if (!std::memcmp(ref_now + r, ref_then + r, 16))
        kept = true;
      else if (!std::memcmp(ref_now + r, floats + r, 16))
        // Changed since the previous frame and shared with this draw: the frame's. A register
        // of the reference draw's own (its transform) stays this draw's.
        std::memcpy(camera_floats_.data() + r, ref_then + r, 16);
    }
    if (kept) found_ = Found::kCamera;
    return nullptr;
  }
  std::vector<Draw> current_, previous_;
  std::vector<uint8_t> current_floats_, previous_floats_;
  size_t cursor_ = 0;  // in previous_: after the last draw matched
  size_t last_index_ = 0;
  // Anchor distances of this frame's matches, and the limit from the last frame's (BeginFrame).
  static constexpr float kAnchorMin = 128.0f;
  std::vector<float> distances_;
  float anchor_limit_ = std::numeric_limits<float>::infinity();
  bool far_ = false;       // the last Match refused its draw for its distance
  bool compared_ = false;  // the last Match compared the anchors of its draw and the one found
  uint64_t frame_ = UINT64_MAX - 1;
  Found found_ = Found::kNone;
  std::vector<uint8_t> camera_floats_;  // Camera's and OtherCopies'
  uint32_t batch_shuffles_ = 0;         // TakeBatchShuffles
  // previous_ by loose identity, built at the first draw the window does not find this frame.
  std::unordered_map<uint64_t, std::vector<uint32_t>> by_loose_;
  bool by_loose_built_ = false;
  uint32_t draws_ = 0, misses_ = 0;  // this frame's draws, those the window did not find
  bool global_ = false;              // search the whole previous frame (BeginFrame)
  // The vertex buffers (the header): whether the last frame's matches nearly all kept their
  // buffer, and this frame's counts.
  bool stable_ = false;
  bool buffers_ = true;
  uint32_t last_refusal_ = 0;
  uint32_t buffer_matches_ = 0, same_buffer_matches_ = 0, other_buffers_ = 0;
};

}  // namespace rexnative
