// ReXNative renderer: guest Resolve -> host texture copies.
//
// A guest Resolve(Flags, pSourceRect, pDestTexture, pDestPoint, DestLevel, DestSliceOrFace,
// pClearColor, ClearZ, ClearStencil, pParameters) copies a rectangle of a render target
// (color RT 0-3 or depth) into a texture level, converting the format, averaging MSAA samples,
// and optionally clears the source surfaces afterwards. Here every resolve destination
// (guest physical address of the level/slice) gets a host Texture2D that holds exactly the
// texels the Xenos plugin writes into guest memory, untiled and endian-unswapped, in
// the typeless family of GetHostFormat(dest format)->resource (texture_layout.h), so the
// texture cache can bind it in place of its own texture (or copy it into one) instead of the
// guest-memory upload. A compute shader (format packing as ReXGlue's resolve shaders do it)
// writes the texels straight into the image through a UINT view. Images persist per
// destination address, format and size, so their resources and views stay valid.
//
// Arguments are decoded the way the guest D3D does it (read from its generated code,
// Resolve): NULL source rect = the destination level's size, right/bottom rounded up to
// 8; NULL dest point = (0, 0); sample select from Flags bits 4-6 (0 = by the surface's MSAA:
// 1x sample 0, 2x average of 2, 4x average of 4); exponent bias from Flags bits 26-31;
// red/blue swap when the destination's swizzle X is not R; raw copies for depth, conversion
// for color (the guest D3D never asks for a raw color copy). The guest Resolve runs first, so
// DecodeResolve also compares the decoded copy registers with the ones the D3D wrote into its
// register shadow and logs mismatches.
//
// Oracle dumps (--rexnative_resolve_dump=<dir>, resolve_debug.cpp, with the self test):
// creating <dir>\dump.request arms a dump of the next frame's resolves: for each, the native
// texels (`f<F>-r<i>-native.bin`, tight rows), the guest memory Xenos wrote at the same address
// in the same frame (`-guest.bin`, tiled, big-endian; read in AfterSwap with the plugin on full
// readback, see HoldFullReadback), and one JSON line in `f<F>-resolves.jsonl`. When a frame
// resolves twice into one address, every entry's guest copy is the last write.
// tools/compare_resolves.py untiles and compares them. AfterSwap's wait holds the guest only
// when frames are recorded on the guest thread, so dumps need --rexnative_render_thread off.
//
// Pipeline state: Resolve sets its own compute root signature, PSO and shader-visible
// descriptor heap on the command list; callers that draw afterwards must set theirs again.
#pragma once

#include "render_targets.h"

#include "native/sdk.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace rexnative {

class GpuTimer;
class Upscaler;

// What the texture cache needs to use a resolve result (TextureMod contract).
// The resources live as long as the Resolver.
struct ResolvedImage {
  uint32_t guest_base = 0;                       // physical address of the destination level/slice
  rex::graphics::xenos::TextureFormat format{};  // destination format (RB_COPY_DEST_INFO)
  uint32_t width = 0, height = 0;                // texels written from (0, 0), dest point included
  ID3D12Resource* texture = nullptr;  // state: COPY_SOURCE | ALL_SHADER_RESOURCE; typeless
  // Depth resolves only: R32_FLOAT = float20e4(d >> 8) (or unorm24 for D24S8), what the
  // texture cache's k_24_8(_FLOAT) conversion gives when the game samples the k_8_8_8_8
  // destination as depth. Same size and state as `texture`.
  ID3D12Resource* depth_r32f = nullptr;
  // With linear output (Resolver::set_linear_output), the last resolve from a float target into
  // k_2_10_10_10: the copied color before the format's clamp to [0, 1], R16G16B16A16_FLOAT, same
  // size and state as `texture`; nullptr otherwise.
  ID3D12Resource* linear = nullptr;
  uint64_t seq = 0;  // increases with every write (global resolve counter)
  // The guest layout the image stands for (Resolver::FindContaining): its memory from guest_base
  // for extent_bytes, tiled or not, rows of pitch_blocks texels of bytes_per_texel.
  uint32_t extent_bytes = 0, pitch_blocks = 0, bytes_per_texel = 0;
  bool tiled = false;
  // This port: host texels per guest texel (--rexnative_resolution_scale for the screen-sized
  // surfaces' resolves); the textures are width x height times it.
  uint32_t scale = 1;
};

// One decoded Resolve call.
struct ResolveParams {
  SurfaceRegs regs;                          // the source surfaces (register shadow at the call)
  uint32_t source = 0;                       // 0-3 color render target, 4 depth
  uint32_t sample_select = 0;                // xenos::CopySampleSelect
  uint32_t copy_command = 1;                 // xenos::CopyCommand: 0 raw (depth), 1 convert
  int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;    // source rectangle, surface pixels, exclusive end
  int32_t dest_x = 0, dest_y = 0;            // destination point
  uint32_t dest_base = 0;                    // physical address of the destination level/slice
  uint32_t dest_format = 0;                  // xenos::TextureFormat (copy destination format)
  uint32_t dest_width = 0, dest_height = 0;  // destination level size, texels
  uint32_t dest_endian = 0;  // xenos::Endian (dumps only; the host image is unswapped)
  uint32_t dest_number = 0;  // xenos::SurfaceNumberFormat: 0/1 unorm/snorm, 2/3 uint/sint, 7 float
  bool dest_swap = false;    // swap red and blue
  int32_t exp_bias = 0;      // color *= 2^exp_bias (convert only)
  // Guest layout of the destination (dumps only).
  bool dest_tiled = false;
  uint32_t dest_pitch_blocks = 0;  // row pitch in texels (tiled: 32-aligned)
  uint32_t dest_extent_bytes = 0;  // guest memory the level/slice occupies
  // Clears after the copy (Flags 0x100 color, 0x200 depth+stencil) over the source rect, all
  // of it on the target even where the copy is clipped to the destination level.
  // Inside EndTiling the rect is one tile, which the guest rendered with its origin (edram_x,
  // edram_y) at the surface's EDRAM base, so that is where the clear lands in the EDRAM.
  uint32_t clear_flags = 0;
  int32_t edram_x = 0, edram_y = 0;
  float clear_color[4] = {};
  float clear_depth = 1.0f;
  uint8_t clear_stencil = 0;
  // Decoded from the registers (DecodeRegisterResolve): dest_base is where the copy's address
  // computation starts, which the guest moves past the level's base for a destination point
  // below the rectangle's origin (a stack of shadow cascades in one texture). Resolver::Resolve
  // writes such a copy into the image whose memory holds that address.
  bool from_registers = false;
};

// The registers a resolve reads, as the GPU has them at the resolve's draw packet (and those a
// clear quad's draw packet needs: DecodeRegisterClear).
struct ResolveRegisters {
  SurfaceRegs surface;                       // 0x2000..0x2005
  uint32_t screen_scissor_tl = 0, screen_scissor_br = 0;               // 0x200E, 0x200F
  uint32_t window_offset = 0, window_scissor_tl = 0, window_scissor_br = 0;  // 0x2080..0x2082
  uint32_t su_sc_mode_cntl = 0;              // PA_SU_SC_MODE_CNTL (0x2205)
  uint32_t vtx_cntl = 0;                     // PA_SU_VTX_CNTL (0x2302)
  uint32_t copy_control = 0, copy_dest_base = 0, copy_dest_pitch = 0, copy_dest_info = 0;  // 0x2318..
  uint32_t depth_clear = 0, color_clear = 0, color_clear_lo = 0;     // 0x231D..0x231F
  uint32_t vertex_fetch[2] = {};             // vertex fetch constant 0 (0x4800, 0x4801)
  uint32_t color_mask = 0;                   // RB_COLOR_MASK (0x2104)
  uint32_t stencil_ref_mask = 0;             // RB_STENCILREFMASK (0x210D)
  uint32_t depth_control = 0;                // RB_DEPTHCONTROL (0x2200)
  // Each of them through `get(reg)`.
  template <typename Get>
  static ResolveRegisters From(Get&& get) {
    ResolveRegisters r;
    r.surface.surface_info = get(0x2000);
    r.surface.color_info[0] = get(0x2001);
    r.surface.depth_info = get(0x2002);
    for (uint32_t i = 1; i < 4; ++i) r.surface.color_info[i] = get(0x2002 + i);
    r.screen_scissor_tl = get(0x200E);
    r.screen_scissor_br = get(0x200F);
    r.window_offset = get(0x2080);
    r.window_scissor_tl = get(0x2081);
    r.window_scissor_br = get(0x2082);
    r.su_sc_mode_cntl = get(0x2205);
    r.vtx_cntl = get(0x2302);
    r.copy_control = get(0x2318);
    r.copy_dest_base = get(0x2319);
    r.copy_dest_pitch = get(0x231A);
    r.copy_dest_info = get(0x231B);
    r.depth_clear = get(0x231D);
    r.color_clear = get(0x231E);
    r.color_clear_lo = get(0x231F);
    r.vertex_fetch[0] = get(0x4800);
    r.vertex_fetch[1] = get(0x4801);
    r.color_mask = get(0x2104);
    r.stencil_ref_mask = get(0x210D);
    r.depth_control = get(0x2200);
    return r;
  }
};

// Decodes a resolve from the registers (`physical` = the guest's physical membase, for the
// rectangle's vertices), as ReXGlue's GetResolveInfo does: a resolve no hooked call made, which
// the push buffer between hooked calls holds (rexnative.inline_resolves: an engine that writes
// its own, or a D3D path the layer does not hook). The rectangle is the three vertices of vertex fetch constant 0, moved by
// the window offset, inside the scissor, 8-pixel aligned; each source pixel goes to the
// destination texel with its coordinates from RB_COPY_DEST_BASE; the clears take their values
// from RB_COLOR_CLEAR(_LO) / RB_DEPTH_CLEAR in the source surface's format. Returns false (logged
// once per reason) for what is not supported: copy commands other than raw/convert, array
// destinations, formats the resolve cannot write, an empty rectangle.
bool DecodeRegisterResolve(const uint8_t* physical, const ResolveRegisters& regs,
                           ResolveParams* out);

// Decodes the D3D's Clear quad from the registers at its draw packet (RB_MODECONTROL kColorDepth,
// DRAW_INDX_2): a clear no hooked call made, recorded in a command buffer the game replays
// (Darwinia+ records its frame's Clear there each frame). Vertex fetch constant 0 holds its three
// vertices, each x, y, z and the RGBA color; the surfaces are those the registers bind, the
// color targets those RB_COLOR_MASK writes, depth and stencil those RB_DEPTHCONTROL writes (the
// stencil value is the reference the quad writes). The Clear draws a 1x surface as 4x MSAA of
// half its pitch and height: ClearParams::edram_alias carries it to the 1x targets. Returns
// false (logged once) for another vertex layout, nothing written, an empty rectangle.
bool DecodeRegisterClear(const uint8_t* physical, const ResolveRegisters& regs, ClearCall* out);

// Decodes a Resolve hook call (`args` = r3..r10, f1, sp at entry; guest memory at `base`), after
// the guest Resolve returned, and logs where its copy registers in the D3D's shadow disagree.
// Returns false (and logs once per reason) for what is not supported yet: packed mip tails,
// 3D destinations, copy commands other than raw/convert.
bool DecodeResolve(const uint8_t* base, const HookArgs& args, ResolveParams* out);

// The Xenos plugin's --readback_resolve "fast"/"some" copy the previous resolve of the same
// address range into guest memory (one frame late), so guest memory never holds the frame the
// guest just swapped. Oracles (captures, dumps) call HoldFullReadback(true) a frame ahead, which
// switches the plugin to "full" (synchronous, exact) through the cvar registry, and
// HoldFullReadback(false) after reading guest memory, which restores the previous value. Nested
// holds are counted. On the frame-recording thread only (resolve dumps and Presenter captures
// call it from Execute*: the render thread by default).
void HoldFullReadback(bool hold);

class Resolver {
 public:
  Resolver();
  ~Resolver();

  // `queue` runs SelfTest at start when --rexnative_resolve_selftest is set.
  bool Initialize(ID3D12Device* device, RenderTargets* targets, ID3D12CommandQueue* queue);

  // Once per frame before recording it, like RenderTargets::BeginFrame (handles dumps too).
  // On the frame-recording thread, as are AfterSwap and Resolve.
  void BeginFrame(uint64_t frame, uint64_t frame_fence, uint64_t completed_fence);

  // After the swap's EndFrame: for a dumped frame, sleeps --rexnative_capture_hold_ms so the
  // Xenos plugin can execute the frame, then copies the guest memory of its resolves. That
  // sleep holds the guest only when frames are recorded on the guest thread: with the render
  // thread (the default) the guest runs on and the copies may hold a later frame.
  void AfterSwap();

  bool Resolve(CommandStream* list, const ResolveParams& p);
  // The copy of `p` into the Resolver's own image instead of p.dest_base, without its clears:
  // the texels the guest's resolve would write there now (frame_generation.h's HUD-less copy of
  // the front buffer). No texture sees it. Returns the image, nullptr on failure (logged).
  const ResolvedImage* ResolveCopy(CommandStream* list, const ResolveParams& p);

  // The last resolve into this guest physical address, or nullptr.
  // The image whose guest memory holds `address` at the start of a row of its 32x32 tiles
  // (tiled) or of a row (linear), other than at its base: *row is that row. A stacked texture's
  // slices (Brothers - A Tale of Two Sons' colour grading table, 16 slices of 16x16 resolved as
  // one 16x512 image, a slice every 32 rows) and levels kept in one resolved image start there.
  const ResolvedImage* FindContaining(uint32_t address, uint32_t* row) const;
  const ResolvedImage* Find(uint32_t guest_phys_base) const;
  // Called with a guest physical address whenever its Find result may change (an image is
  // resolved into, created, replaced or erased), so a cache can recheck only the textures
  // that sample that address.
  void SetChangedCallback(std::function<void(uint32_t guest_phys_base)> f) {
    changed_ = std::move(f);
  }
  // HDR output: resolves from a float target into k_2_10_10_10 also keep their linear color
  // (ResolvedImage::linear). Takes effect at the next resolve.
  void set_linear_output(bool on) { linear_output_ = on; }
  // With the upscaler: resolves out of its scene surface copy the upscaled color (upscaler.h).
  void set_upscaler(Upscaler* upscaler) { upscaler_ = upscaler; }
  // With GPU timing: Resolve marks its phases (GpuTimer::ResolvePhase).
  void set_gpu_timer(GpuTimer* timer) { timer_ = timer; }

  // Clears host targets of the formats PGR4 resolves from (8888, 7e3, 16F, 32F, D24FS8/D24S8;
  // 1x/2x/4x) to known values, resolves them into the destination formats PGR4 uses on its own
  // command list, and checks the read-back texels against hand-computed Xenos encodings. Logs
  // each case; returns true when all pass. Initialize runs it with --rexnative_resolve_selftest.
  bool SelfTest(ID3D12CommandQueue* queue);

 private:
  struct Image;
  // Every image resolved into one guest address, one per format and size. Images are never
  // destroyed: the texture cache binds them in place of its own textures and the draws keep
  // descriptors of them, and an address that alternates between formats or sizes reuses its
  // images instead of recreating one per resolve.
  struct Destination {
    std::vector<std::unique_ptr<Image>> images;
    Image* current = nullptr;  // the last one written (Find)
    // Looked up by the texture cache or the presenter (Find, FindContaining): the GPU uses it.
    // A destination it never used is one the CPU reads (rexnative.resolve_writeback).
    mutable bool sampled = false;
  };
  struct Dump;
  struct DumpDeleter {
    void operator()(Dump* dump) const;
  };
  bool CreatePipelines();
  // Resolve into `own` (ResolveCopy, no clears), or with own null into p.dest_base.
  bool Resolve(CommandStream* list, const ResolveParams& p, Destination* own);
  // For a resolve decoded from the registers: the image of another destination whose guest
  // memory holds p.dest_base at the start of a row of its tiles, with p's format and pitch, and
  // that row (*row); nullptr if none.
  const Image* Holding(const ResolveParams& p, uint32_t* row) const;
  // The image of `dest` for p's format and size, created if missing.
  Image* GetImage(const ResolveParams& p, Destination& dest, uint32_t scale);
  // Copies the clipped rectangle `src` of rt to (dx, dy) in an image of `dest` (p.dest_base's or
  // own_), with the alpha of `alpha` when not null (rt is the upscaler's output, alpha the scene
  // target it upscaled).
  bool Copy(CommandStream* list, const ResolveParams& p, RenderTarget* rt, RenderTarget* alpha,
            const D3D12_RECT& src, int32_t dx, int32_t dy, Destination& dest);

  // Oracle dumps and the self test (resolve_debug.cpp), and what they use of resolve.cpp.
  void InitializeOracles(ID3D12CommandQueue* queue);  // as their cvars ask
  // While a dump records, reads back `img`'s texels from (0, 0) to (width, height).
  // rexnative.resolve_writeback (off by default): a resolve's texels read back to the CPU after
  // its frame (WritebackRecord, at the copy) and written into guest memory as the console's
  // resolve writes them, tiled and endian-swapped, once the GPU has done that frame
  // (WritebackFlush, in BeginFrame): one to three frames after the guest's own resolve.
  struct Writeback;
  void WritebackRecord(CommandStream* list, const ResolveParams& p, ID3D12Resource* texture,
                       int32_t dx, int32_t dy, uint32_t w, uint32_t h);
  void WritebackFlush(uint64_t completed_fence);
  std::vector<std::unique_ptr<Writeback>> writebacks_, writeback_pool_;

  void DumpRecord(CommandStream* list, const ResolveParams& p, const ResolvedImage& img,
                  uint32_t width, uint32_t height);
  void DumpBeginFrame(uint64_t completed_fence);
  static DXGI_FORMAT DestResourceFormat(uint32_t dest_format);  // of the host image
  static uint32_t DestBytesPerTexel(uint32_t dest_format);
  static bool IsFloatDest(uint32_t dest_format);
  // false: the generic copy pipelines only; true: each kind's specialized one, compiled on
  // first use on the calling thread.
  void SetSpecialization(bool sync);
  void EraseDestination(uint32_t dest_base);  // and every image of it

  ID3D12Device* device_ = nullptr;
  RenderTargets* targets_ = nullptr;
  struct Pipelines;
  std::unique_ptr<Pipelines> pipelines_;
  ComPtr<ID3D12DescriptorHeap> heap_;  // shader visible: per-frame rings of view tables
  uint32_t heap_stride_ = 0;
  uint32_t heap_next_ = 0;
  bool b5g6r5_store_ = false;  // typed UAV store of B5G6R5_UNORM (k_5_6_5 / k_6_5_5 images)
  bool linear_output_ = false;
  std::unordered_map<uint32_t, Destination> destinations_;
  Destination own_;  // ResolveCopy's
  size_t image_count_ = 0;
  uint64_t frame_ = 0, frame_fence_ = 0;
  uint64_t seq_ = 0;
  std::function<void(uint32_t)> changed_;
  std::unique_ptr<Dump, DumpDeleter> dump_;
  GpuTimer* timer_ = nullptr;
  Upscaler* upscaler_ = nullptr;
};

}  // namespace rexnative
