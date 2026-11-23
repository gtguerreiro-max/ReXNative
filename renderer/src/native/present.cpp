// ReXNative renderer: Swap -> debug window, front buffer captures.
// See present.h.
#include "present.h"

#include "native/sdk.h"

#include "d3d12_device.h"
#include "fsr.h"
#include "guest_memory.h"
#include "overlay.h"
#include "present_debug.h"
#include "renderer.h"
#include "snapshot.h"
#include "resolve.h"
#include "texture_layout.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include "host.h"
#include <rex/kernel/heap.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

REXCVAR_DEFINE_STRING(
    rexnative_capture_dir, "", "ReXNative",
    "Folder for front buffer captures (native + Xenos PNGs, PSNR), marks and draw "
    "logs; when set, <dir>\\capture.request and the mark key (F12) take a "
    "capture and <dir>\\draw-debug.txt is read. Empty: nothing is polled");
REXCVAR_DEFINE_INT32(rexnative_capture_frame, -1, "ReXNative",
                     "Capture the front buffer of this native frame index (-1 = off)");
REXCVAR_DEFINE_INT32(rexnative_capture_burst, 1, "ReXNative",
                     "Frames each capture request takes, one after the other (1-8)");
REXCVAR_DEFINE_INT32(rexnative_capture_hold_ms, 500, "ReXNative",
                     "How long a capture holds the guest thread in Swap so the Xenos plugin "
                     "finishes the frame before its front buffer is read from guest memory");
REXCVAR_DEFINE_BOOL(rexnative_capture_xenos, true, "ReXNative",
                    "Captures also write the Xenos plugin's copy of the frame and the PSNR "
                    "between them, holding the guest thread (--rexnative_capture_hold_ms; not "
                    "with frame generation on); false: the native image alone, written off the "
                    "guest thread");
REXCVAR_DEFINE_BOOL(rexnative_capture_shown, false, "ReXNative",
                    "Captures as the screen shows them: the game's display gamma ramp applied, "
                    "as the presenter applies it (the classic renderer's frames have it "
                    "already). Off: the front buffer as the game wrote it, before the ramp");
REXCVAR_DEFINE_BOOL(rexnative_luma_probe, false, "ReXNative",
                    "Read back three rows of every front buffer (at 10, 40 and 80 % of its "
                    "height) and log their mean brightness in the frame CSV, to find frames "
                    "drawn partly black");
REXCVAR_DEFINE_DOUBLE(rexnative_display_aspect, rexnative::game::kDefaultDisplayAspect, "ReXNative",
                      "The shape the front buffer is shown at, width over height, as the console's "
                      "scaler shows it (From Dust's 1024x600 at 16:9); 0: its pixels square");
REXCVAR_DEFINE_BOOL(rexnative_fsr, true, "ReXNative",
                    "Scale the front buffer up to the window with AMD FSR 1 (EASU + RCAS) instead "
                    "of bilinear filtering (SDR output only)");
REXCVAR_DEFINE_DOUBLE(rexnative_fsr_sharpness, 0.2, "ReXNative",
                      "FSR 1's sharpening (RCAS) reduction in stops: 0 the sharpest, 2 soft")
    .range(0.0, 2.0);
REXCVAR_DEFINE_STRING(rexnative_hdr_sdr_curve, "auto", "ReXNative",
                      "HDR output: the curve that turns the game's SDR values (up to its white) "
                      "into light: srgb (the piecewise sRGB curve, which Windows shows SDR "
                      "windows with in HDR mode, Xenia's among them), 2.2 (a 2.2 power, darker "
                      "shadows) or auto (srgb where Windows reports its SDR brightness, 2.2 "
                      "elsewhere)");
REXCVAR_DEFINE_INT32(rexnative_hdr_paper_white, 0, "ReXNative",
                     "HDR output: the luminance of SDR white (the HUD, the game's 1.0) in nits; "
                     "0: Windows' SDR content brightness for the display (Settings > Display > "
                     "HDR), so the game is as bright as an SDR game there up to white, else 400, "
                     "what gamescope shows SDR content at (--hdr-sdr-content-nits). ITU-R "
                     "BT.2408's reference is 203");

namespace rexnative {

namespace xenos = rex::graphics::xenos;
using present_debug::BlitConstants;
using present_debug::ToRgb;
using present_debug::WritePng;

namespace {

// The display controller's gamma ramp, as the Xenos command processor holds it from the guest's
// DC_LUT register writes: 128 PWL entries per channel (DC_LUT_PWL_DATA: base 0:15, delta
// 16:31) for 2_10_10_10 front buffers, 256 table entries (DC_LUT_30_COLOR: blue 0:9, green
// 10:19, red 20:29) for 8_8_8_8 ones, as D3D's presentation parameters set them up. The ramp
// math follows Xenia's apply_gamma_pwl / apply_gamma_table (BSD-3-Clause, see
// THIRD_PARTY_NOTICES.md). The ramp applies to each texel before filtering, so a scaled image
// filters display values like the console's scaler does.
enum RampMode : uint32_t { kRampNone = 0, kRampPwl = 1, kRampTable = 2 };
constexpr uint32_t kPwlWords = 128 * 3, kTableWords = 256;
constexpr uint32_t kRampSlotBytes = kPwlWords * 4;  // a multiple of the 256-byte CBV alignment
static_assert(kRampSlotBytes % D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT == 0);
static_assert(sizeof(rex::graphics::reg::DC_LUT_PWL_DATA) == 4 &&
              sizeof(rex::graphics::reg::DC_LUT_30_COLOR) == 4);

// image_size: the image in texels (the texture can be larger than the image); point: the
// viewport is the image's size, every pixel is one texel.
//
// HDR output (hdr = 1): src is the front buffer's linear image (ResolvedImage::linear), the
// game's color before the front buffer clips it at 1. The game renders linear light and leaves
// the display curve to the gamma ramp, so up to 1 a texel is shown as in SDR: through the ramp,
// then an SDR display's curve (srgb: the piecewise sRGB one Windows shows SDR windows with in HDR
// mode; else a 2.2 power), with SDR white at paper_white. Above 1 it continues
// linearly from the ramp's white. A hue-preserving shoulder (Reinhard on the largest component,
// slope 1 at the knee) fits the highlights under the display's peak; the HUD stays at paper
// white. Then BT.709 -> BT.2020 and the PQ curve (SMPTE ST 2084).
const char kBlitHlsl[] = R"(
Texture2D<float4> src : register(t0);
cbuffer blit : register(b0) {
  float2 image_size; uint ramp_mode; uint point_sampled;
  uint hdr; float paper_white; float peak; float knee;  // paper_white in 10000 nits (PQ 1.0);
  float3 white; uint srgb;                              // peak and knee over SDR white
};
cbuffer gamma : register(b1) { uint4 ramp[96]; };
void vs(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 texel : TEXCOORD0) {
  float2 uv = float2((id << 1) & 2, id & 2);
  pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  texel = uv * image_size;
}
uint RampWord(uint i) { return ramp[i >> 2][i & 3]; }
float3 Gamma(float3 c) {
  if (ramp_mode == 1) {
    uint3 x = uint3(c * 1023.0 + 0.5);
    uint3 w = uint3(RampWord((x.r >> 3) * 3), RampWord((x.g >> 3) * 3 + 1),
                    RampWord((x.b >> 3) * 3 + 2));
    return min((float3(w & 0xFFFF) + float3((x & 7) * (w >> 16)) * 0.125) * (1.0 / 65472.0), 1.0);
  }
  if (ramp_mode == 2) {
    uint3 x = uint3(c * 255.0 + 0.5);
    return float3((RampWord(x.r) >> 20) & 1023, (RampWord(x.g) >> 10) & 1023,
                  RampWord(x.b) & 1023) * (1.0 / 1023.0);
  }
  return c;
}
float3 Texel(int2 p) {
  float3 c = src.Load(int3(p, 0)).rgb;
  if (!hdr) return Gamma(c);
  float3 g = Gamma(saturate(c));
  float3 sdr = srgb ? (g <= 0.04045 ? g / 12.92 : pow((g + 0.055) / 1.055, 2.4)) : pow(g, 2.2);
  return c > 1.0 ? white * c : sdr;
}
float3 Hdr10(float3 c) {
  float m = max(max(c.r, c.g), c.b);
  if (m > knee) {
    float d = m - knee, room = peak - knee;
    c *= (knee + d * room / (d + room)) / m;
  }
  c = mul(float3x3(0.6274040, 0.3292820, 0.0433136,
                   0.0690970, 0.9195400, 0.0113612,
                   0.0163916, 0.0880132, 0.8955950), c);
  float3 y = pow(max(c, 0.0) * paper_white, 0.1593017578125);
  return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), 78.84375);
}
float3 Image(float2 texel) {
  int2 hi = int2(image_size) - 1;
  if (point_sampled) return Texel(min(int2(texel), hi));
  float2 t = texel - 0.5;
  float2 f = t - floor(t);
  int2 a = clamp(int2(floor(t)), 0, hi), b = min(a + 1, hi);
  float3 top = lerp(Texel(a), Texel(int2(b.x, a.y)), f.x);
  float3 bottom = lerp(Texel(int2(a.x, b.y)), Texel(b), f.x);
  return lerp(top, bottom, f.y);
}
float4 ps(float4 pos : SV_Position, float2 texel : TEXCOORD0) : SV_Target {
  float3 c = Image(texel);
  return float4(hdr ? Hdr10(c) : c, 1.0);
}
)";

// The Xenos driver's command processor's gamma ramps (rex/xenos/draw_hook.h, sdk/patches).
rex::xenos::GammaRampView GuestGammaRamps() { return rex::xenos::GetGammaRampView(); }

// --rexnative_hdr_sdr_curve: auto takes sRGB where Windows reports its SDR brightness (DWM shows
// SDR windows through the sRGB curve, so the game matches them, Xenia's included), and the 2.2
// power under Wine (gamescope's HDR).
uint32_t SrgbCurve(const D3D12Device& d) {
  const std::string curve = REXCVAR_GET(rexnative_hdr_sdr_curve);
  if (curve == "srgb") return 1;
  if (curve == "2.2") return 0;
  return d.hdr_sdr_white_nits() > 0.0f ? 1 : 0;
}

// Output of a PWL ramp channel for a 10-bit input (the shader's math), for the log.
double PwlOut(const uint32_t* words, uint32_t channel, uint32_t x) {
  const uint32_t w = words[(x >> 3) * 3 + channel];
  return std::min(1.0, ((w & 0xFFFFu) + (x & 7u) * (w >> 16) / 8.0) / 65472.0);
}

// The ramp the presenter applies to a front buffer (WriteRamp's source), copied for a capture
// (--rexnative_capture_shown). kRampNone when the driver holds none.
RampMode CopyRamp(bool rgb10a2, uint32_t* out) {
  const rex::xenos::GammaRampView ramps = GuestGammaRamps();
  const RampMode mode = rgb10a2 ? kRampPwl : kRampTable;
  const uint32_t* words = mode == kRampPwl ? ramps.pwl_rgb : ramps.table_256;
  if (!words) return kRampNone;
  const uint32_t count = mode == kRampPwl ? kPwlWords : kTableWords;
  for (uint32_t i = 0; i < count; ++i) out[i] = __atomic_load_n(words + i, __ATOMIC_RELAXED);
  return mode;
}

// A captured image (8-bit RGB, the fetch constant's swizzle applied) through the ramp, as the
// blit shader's Gamma does it, so it matches the window and the classic renderer's frames.
void ApplyRamp(RampMode mode, const uint32_t* ramp, std::vector<uint8_t>* rgb) {
  if (mode == kRampNone) return;
  uint8_t lut[3][256];
  for (uint32_t c = 0; c < 3; ++c)
    for (uint32_t v = 0; v < 256; ++v) {
      const double out = mode == kRampPwl
                             ? PwlOut(ramp, c, uint32_t(v * 1023.0 / 255.0 + 0.5))
                             : double((ramp[v] >> (20 - 10 * c)) & 1023u) / 1023.0;
      lut[c][v] = uint8_t(std::lround(std::clamp(out, 0.0, 1.0) * 255.0));
    }
  for (size_t i = 0; i < rgb->size(); ++i) (*rgb)[i] = lut[i % 3][(*rgb)[i]];
}

double Psnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
  if (a.size() != b.size() || a.empty()) return 0.0;
  double se = 0.0;
  for (size_t i = 0; i < a.size(); ++i) {
    double d = double(a[i]) - double(b[i]);
    se += d * d;
  }
  double mse = se / double(a.size());
  return mse == 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

struct Extent {
  uint32_t width = 0, height = 0;
};

// The texels a resolve wrote from (0, 0) (0: the whole texture), within its texture, which can
// be larger than the image.
Extent ImageExtent(const ResolvedImage& image, const D3D12_RESOURCE_DESC& desc) {
  const uint32_t w = uint32_t(desc.Width), h = desc.Height, s = std::max(image.scale, 1u);
  return {std::min(image.width ? image.width * s : w, w),
          std::min(image.height ? image.height * s : h, h)};
}

// A completed front buffer copy as RGB rows.
bool ReadbackRgb(ID3D12Resource* readback, uint32_t row_pitch, uint32_t width, uint32_t height,
                 uint32_t mapping, bool rgb10a2, std::vector<uint8_t>* rgb) {
  void* p = nullptr;
  const D3D12_RANGE range = {0, SIZE_T(row_pitch) * height};
  if (FAILED(readback->Map(0, &range, &p))) {
    REXLOG_ERROR("ReXNative capture: readback map failed");
    return false;
  }
  ToRgb(static_cast<const uint8_t*>(p), row_pitch, width, height, mapping, rgb10a2, rgb);
  const D3D12_RANGE none = {0, 0};
  readback->Unmap(0, &none);
  return true;
}

}  // namespace

struct Presenter::Impl {
  NativeRenderer& r;
  ComPtr<ID3D12RootSignature> root_signature;
  ComPtr<ID3D12PipelineState> pso, hdr_pso;  // into kBackBufferFormat, kHdrBackBufferFormat
  ComPtr<ID3D12DescriptorHeap> heap;
  uint32_t heap_stride = 0, heap_next = 0;
  static constexpr uint32_t kHeapSize = 16;
  // One gamma ramp slot per frame in flight (D3D12Device::frame_slot), persistently mapped.
  ComPtr<ID3D12Resource> ramp_buffer;
  uint8_t* ramp_mapped = nullptr;
  uint32_t last_ramp[kPwlWords] = {};
  RampMode last_ramp_mode = kRampNone;
  uint32_t ramp_changes_logged = 0;
  float ramp_white[3] = {1.0f, 1.0f, 1.0f};  // the last ramp's output for 1.0, display-encoded

  struct Capture {
    bool armed = false;  // requested: capture the next shown frame
    bool xenos = true;   // with the Xenos copy (--rexnative_capture_xenos): full readback held
    bool pending = false;
    uint64_t frame = 0, fence = 0;
    uint32_t fetch[6] = {};
    uint32_t base = 0;
    uint32_t width = 0, height = 0;
    uint32_t mapping = 0;
    bool rgb10a2 = false;
    RampMode ramp_mode = kRampNone;  // --rexnative_capture_shown: the ramp of the frame
    uint32_t ramp[kPwlWords] = {};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    ComPtr<ID3D12Resource> readback;
  } capture;
  // Captures without the Xenos copy: each frame of a burst its own readback, its PNG written once
  // the frame has completed. One slot took every other frame, which hid a flicker that alternates
  // frame by frame.
  struct Shot {
    ComPtr<ID3D12Resource> readback;
    uint32_t pitch = 0, width = 0, height = 0, mapping = 0;
    bool rgb10a2 = false;
    RampMode ramp_mode = kRampNone;  // --rexnative_capture_shown: the ramp of the frame
    std::vector<uint32_t> ramp;
    uint64_t frame = 0, fence = 0;
    std::filesystem::path dir;  // --rexnative_capture_dir, or a snapshot's folder (snapshot.h)
  };
  std::vector<Shot> shots;
  std::vector<std::thread> shot_writers;
  uint64_t last_shot_frame = ~0ull;
  // capture_holding_guest: set by the render thread, read by the guest thread.
  std::atomic<bool> capture_holding{false};
  bool log_fmt = false, log_missing = false;
  bool front_buffer_found = false;  // the last Swap's front buffer had a host image
  uint32_t front_buffer_base = 0;
  ImageRect image_rect;

  present_debug::GlitchCatch glitch;
  present_debug::LumaProbeReadback probe;
  // The settings menu over the frame (overlay.h).
  Overlay overlay;
  OverlayFrame overlay_frame;
  bool overlay_ok = false;
  Fsr1 fsr;  // --rexnative_fsr
  bool fsr_ok = false;

  explicit Impl(NativeRenderer& renderer) : r(renderer) {}
  ~Impl() {
    for (std::thread& t : shot_writers) t.join();
  }

  bool Initialize();
  void OnSwap(const SwapCall& swap, bool shown, const SwapCopy* copy,
              const ResolvedImage* classic);
  // Copies the guest's current ramp of the mode into the frame's slot; false without one.
  bool WriteRamp(RampMode mode, uint32_t slot);
  bool CaptureRequested(uint64_t frame);
  // A capture with the Xenos copy ended (taken or given up): the guest thread runs on.
  void ReleaseCapture() {
    if (capture.xenos) HoldFullReadback(false);
    capture_holding.store(false, std::memory_order_release);
  }
  // With the Xenos copy: waits for the frame, holds the guest thread while the plugin finishes
  // it, reads both images and writes the PNGs and their PSNR.
  void FinishCapture();
  // Without it: the PNGs of the shots whose frames have completed, on a thread of their own.
  void WriteShots();

  // The mark key, F12 (a Steam Input binding can send it from a back grip). True on the press.
  bool MarkPressed();
  bool mark_down = false;
  // --rexnative_capture_dir; empty: no request file and no mark key are polled.
  std::filesystem::path capture_dir, capture_request;
  uint32_t request_poll = 0;
  uint32_t burst_left = 0;  // --rexnative_capture_burst: frames still to capture
};

bool Presenter::Impl::Initialize() {
  ID3D12Device* device = r.d3d12();
  const pD3DCompile compile = D3DCompiler();
  if (!compile) return false;
  ComPtr<ID3DBlob> vs, ps, err;
  if (FAILED(compile(kBlitHlsl, sizeof(kBlitHlsl) - 1, "blit", nullptr, nullptr, "vs", "vs_5_1", 0,
                     0, &vs, &err)) ||
      FAILED(compile(kBlitHlsl, sizeof(kBlitHlsl) - 1, "blit", nullptr, nullptr, "ps", "ps_5_1", 0,
                     0, &ps, &err))) {
    REXLOG_ERROR("ReXNative present: blit shader: {}",
                 err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
    return false;
  }
  D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
  D3D12_ROOT_PARAMETER params[3] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable = {1, &range};
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants = {0, 0, sizeof(BlitConstants) / 4};  // b0
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  params[2].Descriptor = {1, 0};  // b1: the gamma ramp
  params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC rs = {3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ComPtr<ID3DBlob> blob;
  if (Failed(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err),
             "present root signature serialize") ||
      Failed(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                         IID_PPV_ARGS(&root_signature)),
             "present root signature"))
    return false;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
  d.pRootSignature = root_signature.Get();
  d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
  d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  d.SampleMask = UINT_MAX;
  d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  d.NumRenderTargets = 1;
  d.RTVFormats[0] = D3D12Device::kBackBufferFormat;
  d.SampleDesc.Count = 1;
  if (Failed(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)), "present pipeline"))
    return false;
  d.RTVFormats[0] = D3D12Device::kHdrBackBufferFormat;
  if (Failed(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&hdr_pso)),
             "present HDR pipeline"))
    return false;
  D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kHeapSize,
                                   D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  if (Failed(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)), "present heap")) return false;
  heap_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  {
    void* p = nullptr;
    const D3D12_RANGE none = {0, 0};
    if (!CreateBuffer(
            device, D3D12_HEAP_TYPE_UPLOAD, uint64_t(kRampSlotBytes) * D3D12Device::kFramesInFlight,
            D3D12_RESOURCE_STATE_GENERIC_READ, "present gamma ramp buffer", &ramp_buffer) ||
        Failed(ramp_buffer->Map(0, &none, &p), "present gamma ramp map"))
      return false;
    ramp_mapped = static_cast<uint8_t*>(p);
  }
  capture_dir = std::filesystem::path(REXCVAR_GET(rexnative_capture_dir));
  if (!capture_dir.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(capture_dir, ec);
    capture_request = capture_dir / "capture.request";
  }
  glitch.Initialize();
  overlay_ok = overlay.Initialize(device);
  fsr_ok = fsr.Initialize(device);
  return true;
}

bool Presenter::Impl::WriteRamp(RampMode mode, uint32_t slot) {
  const rex::xenos::GammaRampView ramps = GuestGammaRamps();
  const uint32_t* words = mode == kRampPwl ? ramps.pwl_rgb : ramps.table_256;
  if (!words) return false;
  const uint32_t count = mode == kRampPwl ? kPwlWords : kTableWords;
  // The command processor's thread writes the ramp as it executes the guest's register writes;
  // a ramp read during an update shows for one frame, the next frame reads the finished one.
  uint32_t ramp[kPwlWords];
  for (uint32_t i = 0; i < count; ++i) ramp[i] = __atomic_load_n(words + i, __ATOMIC_RELAXED);
  std::memcpy(ramp_mapped + size_t(slot) * kRampSlotBytes, ramp, count * 4);
  if (mode != last_ramp_mode || std::memcmp(ramp, last_ramp, count * 4)) {
    last_ramp_mode = mode;
    std::memcpy(last_ramp, ramp, count * 4);
    for (uint32_t c = 0; c < 3; ++c)
      ramp_white[c] = mode == kRampPwl ? float(PwlOut(ramp, c, 1023))
                                       : float((ramp[255] >> (20 - 10 * c)) & 1023) / 1023.0f;
    if (ramp_changes_logged < 8) {
      ++ramp_changes_logged;
      if (mode == kRampPwl)
        REXLOG_INFO(
            "ReXNative present: PWL gamma ramp at frame {}: 0.25/0.5/0.75 -> R {:.3f} "
            "{:.3f} {:.3f}, G {:.3f} {:.3f} {:.3f}, B {:.3f} {:.3f} {:.3f}",
            r.frame_index(), PwlOut(ramp, 0, 256), PwlOut(ramp, 0, 512), PwlOut(ramp, 0, 767),
            PwlOut(ramp, 1, 256), PwlOut(ramp, 1, 512), PwlOut(ramp, 1, 767), PwlOut(ramp, 2, 256),
            PwlOut(ramp, 2, 512), PwlOut(ramp, 2, 767));
      else
        REXLOG_INFO(
            "ReXNative present: gamma table at frame {}: 64/128/192 -> R {} {} {} (of 1023)",
            r.frame_index(), (ramp[64] >> 20) & 1023, (ramp[128] >> 20) & 1023,
            (ramp[192] >> 20) & 1023);
    }
  }
  return true;
}

bool Presenter::Impl::CaptureRequested(uint64_t frame) {
  const int32_t at = REXCVAR_GET(rexnative_capture_frame);
  bool want = at >= 0 && frame == uint64_t(at);
  // A snapshot the player asked for (snapshot.h): with or without a capture folder.
  if (snapshot::Due(frame)) want = true;
  if (capture_dir.empty()) return want;
  // The mark key takes a burst as the request file does (--rexnative_capture_burst): a flicker
  // shows only across frames in a row.
  if (MarkPressed()) {
    REXLOG_INFO("ReXNative mark at frame {}", frame);
    want = true;
    burst_left = uint32_t(std::clamp(REXCVAR_GET(rexnative_capture_burst), 1, 8) - 1);
  }
  // Every 16th frame: a file query goes through Wine's path translation, and the scripts that
  // write the request do not mind a few frames' delay.
  if (burst_left) {
    --burst_left;
    want = true;
  }
  if (++request_poll >= 16) {
    request_poll = 0;
    std::error_code ec;
    if (std::filesystem::remove(capture_request, ec)) {
      want = true;
      burst_left = uint32_t(std::clamp(REXCVAR_GET(rexnative_capture_burst), 1, 8) - 1);
    }
  }
  return want;
}

bool Presenter::Impl::MarkPressed() {
  // The key's state rather than GetAsyncKeyState's "pressed since the last call" bit, which
  // most F12 presses from a Steam Input binding did not set under Proton. A key and not a pad
  // chord: the game reads the pad too, and R3 (L3+R3 was the chord) is PGR4's look back.
  const bool down = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
  const bool pressed = down && !mark_down;
  mark_down = down;
  return pressed;
}

void Presenter::Impl::WriteShots() {
  const uint64_t done = r.device().completed_fence();
  std::vector<Shot> ready;
  for (size_t i = 0; i < shots.size();) {
    if (shots[i].fence <= done) {
      ready.push_back(std::move(shots[i]));
      shots.erase(shots.begin() + ptrdiff_t(i));
    } else {
      ++i;
    }
  }
  if (ready.empty()) return;
  if (shot_writers.size() >= 16) {
    shot_writers.front().join();
    shot_writers.erase(shot_writers.begin());
  }
  shot_writers.emplace_back([ready = std::move(ready)] {
    for (const Shot& s : ready) {
      std::vector<uint8_t> rgb;
      if (!ReadbackRgb(s.readback.Get(), s.pitch, s.width, s.height, s.mapping, s.rgb10a2, &rgb))
        continue;
      if (!s.ramp.empty()) ApplyRamp(s.ramp_mode, s.ramp.data(), &rgb);
      const std::string name = "f" + std::to_string(s.frame) + "-native.png";
      const bool ok = WritePng((s.dir / name).c_str(), rgb, s.width, s.height);
      REXLOG_INFO("ReXNative capture: frame {} {}x{} ({}\\{}{})", s.frame, s.width, s.height,
                  s.dir.string(), name, ok ? "" : ", write failed");
    }
  });
}

void Presenter::Impl::FinishCapture() {
  Capture& c = capture;
  c.pending = false;
  r.device().WaitFence(c.fence);
  // Let the Xenos plugin's GPU thread execute this frame (its Swap was forwarded already) and
  // copy the resolve back into guest memory.
  Sleep(DWORD(std::max(0, REXCVAR_GET(rexnative_capture_hold_ms))));
  ReleaseCapture();

  std::vector<uint8_t> native_rgb, xenos_rgb;
  const bool read = ReadbackRgb(c.readback.Get(), c.footprint.Footprint.RowPitch, c.width, c.height,
                                c.mapping, c.rgb10a2, &native_rgb);
  c.readback.Reset();
  if (!read) return;
  {
    TextureKey key;
    if (!TextureKeyFromFetch(c.fetch, &key)) return;
    std::vector<HostSubresource> subs;
    uint64_t bytes = GetHostFootprints(key, false, &subs);
    uint32_t base_bytes = 0, mips_bytes = 0;
    GetGuestExtents(key, &base_bytes, &mips_bytes);
    const uint8_t* membase = rexnative::host::Memory()->GetPhysicalMembase();
    GuestTextureMemory mem;
    mem.base = membase + (key.base_page << 12);
    mem.base_size = base_bytes;
    mem.mips = key.mip_page ? membase + (key.mip_page << 12) : nullptr;
    mem.mips_size = mips_bytes;
    std::vector<uint8_t> host(bytes), scratch;
    if (!bytes || subs.empty() || !UntileTexture(key, mem, subs, host.data(), &scratch)) {
      REXLOG_ERROR("ReXNative capture: untiling the Xenos front buffer failed");
      return;
    }
    const HostSubresource& s0 = subs[0];
    uint32_t w = std::min(c.width, s0.width), h = std::min(c.height, s0.height);
    ToRgb(host.data() + s0.offset, s0.row_pitch, w, h, c.mapping, c.rgb10a2, &xenos_rgb);
    if (w != c.width || h != c.height) {
      std::vector<uint8_t> crop(size_t(w) * h * 3);
      for (uint32_t y = 0; y < h; ++y)
        std::memcpy(crop.data() + size_t(y) * w * 3, native_rgb.data() + size_t(y) * c.width * 3,
                    size_t(w) * 3);
      native_rgb.swap(crop);
      c.width = w;
      c.height = h;
    }
  }
  ApplyRamp(c.ramp_mode, c.ramp, &native_rgb);
  ApplyRamp(c.ramp_mode, c.ramp, &xenos_rgb);
  const std::filesystem::path& dir = capture_dir;
  const std::string stem = "f" + std::to_string(c.frame);
  bool ok = WritePng((dir / (stem + "-native.png")).c_str(), native_rgb, c.width, c.height) &&
            WritePng((dir / (stem + "-xenos.png")).c_str(), xenos_rgb, c.width, c.height);
  const double psnr = Psnr(native_rgb, xenos_rgb);
  REXLOG_INFO(
      "ReXNative capture: frame {} {}x{} front buffer {:08X}: PSNR {:.2f} dB ({}\\{}-*.png{})",
      c.frame, c.width, c.height, c.base, psnr, dir.string(), stem, ok ? "" : ", write failed");
  if (std::FILE* f = _wfopen((dir / "captures.csv").c_str(), L"a")) {
    std::fprintf(f, "%llu,%u,%u,%08X,%.3f\n", (unsigned long long)c.frame, c.width, c.height,
                 c.base, psnr);
    std::fclose(f);
  }
}

void Presenter::Impl::OnSwap(const SwapCall& swap, bool shown, const SwapCopy* copy,
                             const ResolvedImage* classic) {
  D3D12Device& d = r.device();
  CommandStream* list = d.command_list();
  ID3D12Device* device = r.d3d12();

  const uint32_t tex = classic ? 0 : swap.texture;
  uint32_t fetch[6] = {};
  const ResolvedImage* image = classic;
  if (tex) {
    std::memcpy(fetch, swap.fetch, sizeof(fetch));
    // The object holds the D3D's virtual address; the GPU (and the Resolver) use the physical one.
    const uint32_t phys = GpuAddress(fetch[1] & 0xFFFFF000u);
    fetch[1] = (fetch[1] & 0xFFFu) | phys;
    image = r.resolver().Find(phys);
  }
  front_buffer_base = tex ? fetch[1] & 0xFFFFF000u : 0;
  bool found = image && image->texture;
  // The host image is in the copy destination format (k_2_10_10_10 for PGR4's
  // k_2_10_10_10_AS_16_16_16_16 front buffer).
  const xenos::TextureFormat format = found ? image->format : xenos::TextureFormat(0);
  const HostFormat* host = found ? GetHostFormat(format) : nullptr;
  if (found && (!host || (format != xenos::TextureFormat::k_8_8_8_8 &&
                          format != xenos::TextureFormat::k_2_10_10_10))) {
    if (!log_fmt) {
      log_fmt = true;
      REXLOG_WARN("ReXNative present: front buffer format {} not handled", uint32_t(format));
    }
    found = false;
  }
  if (!image && !log_missing && !classic) {
    log_missing = true;
    REXLOG_WARN(
        "ReXNative present: no resolved image for the front buffer {:08X} (texture {:08X})",
        fetch[1] & 0xFFFFF000u, tex);
  }
  front_buffer_found = found;
  const bool rgb10a2 = format == xenos::TextureFormat::k_2_10_10_10;
  const bool hdr = d.hdr();
  ID3D12Resource* const linear = found && hdr ? image->linear : nullptr;

  uint32_t mapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  D3D12_RESOURCE_DESC desc = {};
  Extent extent;
  if (found) {
    // The Xenos plugin's frame is RGB already (its presenter's guest output).
    if (!classic)
      mapping = GuestToHostComponentMapping((fetch[3] >> 1) & 0xFFF, host->host_swizzle);
    desc = image->texture->GetDesc();
    extent = ImageExtent(*image, desc);
  }
  if (glitch.on() && !classic) glitch.Collect(d.frame_slot());
  // A held frame (DrawRenderer::FrameIncomplete) never reaches the screen: it is neither blitted
  // nor recorded, so the glitch catcher and the luma probe see what was shown.
  if (shown) {
    D3D12_RESOURCE_BARRIER b = TransitionBarrier(d.back_buffer(), D3D12_RESOURCE_STATE_PRESENT,
                                                 D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->ResourceBarrier(1, &b);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = d.back_buffer_rtv();
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    if (!found) list->ClearRenderTargetView(rtv, black, 0, nullptr);
    if (found) {
      // The image's view: its linear one with HDR output (`hdr_view`), as the front buffer's.
      const auto view = [&](const ResolvedImage& src, bool hdr_view) {
        const bool lin = hdr_view && linear;
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = lin ? DXGI_FORMAT_R16G16B16A16_FLOAT : host->unorm;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = mapping;
        sd.Texture2D.MipLevels = 1;
        const uint32_t slot = heap_next++ % kHeapSize;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += size_t(slot) * heap_stride;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += uint64_t(slot) * heap_stride;
        device->CreateShaderResourceView(lin ? src.linear : src.texture, &sd, cpu);
        return gpu;
      };
      ID3D12DescriptorHeap* heaps[] = {heap.Get()};
      list->SetDescriptorHeaps(1, heaps);
      list->SetGraphicsRootSignature(root_signature.Get());
      list->SetPipelineState(hdr ? hdr_pso.Get() : pso.Get());
      // The image scaled to fit the window, centred, at the console's display aspect
      // (--rexnative_display_aspect: From Dust's 1024x600 is shown at 16:9).
      const float iw = float(extent.width), ih = float(extent.height);
      const float ww = float(d.width()), wh = float(d.height());
      const double aspect_cvar = REXCVAR_GET(rexnative_display_aspect);
      const float aspect = aspect_cvar > 0.0 ? float(aspect_cvar) : iw / ih;
      float vw = ww, vh = std::round(ww / aspect);
      if (vh > wh) {
        vh = wh;
        vw = std::round(wh * aspect);
      }
      const LONG vx = LONG(std::floor((ww - vw) * 0.5f)), vy = LONG(std::floor((wh - vh) * 0.5f));
      image_rect = {int32_t(vx), int32_t(vy), int32_t(vw), int32_t(vh)};
      // Black bars around the aspect-fit image. The swapchain's images get no fast clear, so
      // a full clear would write every pixel the blit then writes again.
      const LONG w = LONG(d.width()), h = LONG(d.height()), rx = vx + LONG(vw), ry = vy + LONG(vh);
      const D3D12_RECT bars[4] = {{0, 0, w, vy}, {0, ry, w, h}, {0, vy, vx, ry}, {rx, vy, w, ry}};
      D3D12_RECT clear[4];
      UINT clears = 0;
      for (const D3D12_RECT& bar : bars)
        if (bar.right > bar.left && bar.bottom > bar.top) clear[clears++] = bar;
      // The Xenos plugin applied the ramp to its frame already.
      const RampMode wanted = classic ? kRampNone : rgb10a2 ? kRampPwl : kRampTable;
      const uint32_t ramp_slot = d.frame_slot();
      const int32_t paper_white_cvar = REXCVAR_GET(rexnative_hdr_paper_white);
      const float paper_white = paper_white_cvar > 0             ? float(paper_white_cvar)
                                : d.hdr_sdr_white_nits() > 0.0f ? d.hdr_sdr_white_nits()
                                                                : 400.0f;
      const float peak = d.hdr_peak_nits() / paper_white;
      BlitConstants constants = {{iw, ih},
                                 wanted != kRampNone && WriteRamp(wanted, ramp_slot) ? wanted
                                                                                     : kRampNone,
                                 vw == iw && vh == ih,
                                 hdr,
                                 paper_white / 10000.0f,
                                 peak,
                                 // The shoulder starts halfway to the peak, above white.
                                 std::max(std::min(1.0f, peak), 0.5f * peak),
                                 {},
                                 SrgbCurve(d)};
      // The ramp's white through the same curve as the shader's SDR values.
      for (uint32_t c = 0; c < 3; ++c) {
        const float w = ramp_white[c];
        constants.white[c] = constants.ramp_mode == kRampNone ? 1.0f
                             : !constants.srgb               ? std::pow(w, 2.2f)
                             : w <= 0.04045f                 ? w / 12.92f
                                             : std::pow((w + 0.055f) / 1.055f, 2.4f);
      }
      list->SetGraphicsRoot32BitConstants(1, sizeof(constants) / 4, &constants, 0);
      list->SetGraphicsRootConstantBufferView(
          2, ramp_buffer->GetGPUVirtualAddress() + uint64_t(ramp_slot) * kRampSlotBytes);
      D3D12_VIEWPORT vp = {float(vx), float(vy), vw, vh, 0.0f, 1.0f};
      D3D12_RECT sc = {0, 0, LONG(d.width()), LONG(d.height())};
      list->RSSetViewports(1, &vp);
      list->RSSetScissorRects(1, &sc);
      list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      const auto blit = [&](const ResolvedImage& src, D3D12_CPU_DESCRIPTOR_HANDLE target) {
        if (clears) list->ClearRenderTargetView(target, black, clears, clear);
        list->SetGraphicsRootDescriptorTable(0, view(src, true));
        list->OMSetRenderTargets(1, &target, FALSE, nullptr);
        list->DrawInstanced(3, 1, 0, 0);
      };
      // The copy first: the glitch catcher draws the front buffer's view again.
      if (copy && copy->image->format == format &&
          (linear ? copy->image->linear : copy->image->texture)) {
        D3D12_RESOURCE_BARRIER cb = TransitionBarrier(copy->target, D3D12_RESOURCE_STATE_PRESENT,
                                                      D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ResourceBarrier(1, &cb);
        blit(*copy->image, copy->rtv);
        std::swap(cb.Transition.StateBefore, cb.Transition.StateAfter);
        list->ResourceBarrier(1, &cb);
      }
      // FSR 1 when the image is scaled up: the image through the ramp at its size, then
      // EASU and RCAS into the back buffer (after the glitch catcher, which reuses the blit's
      // bindings).
      const bool use_fsr = fsr_ok && !hdr && REXCVAR_GET(rexnative_fsr) && vw >= iw && vh >= ih &&
                           (vw > iw || vh > ih);
      if (!use_fsr) blit(*image, rtv);
      if (glitch.on() && !classic) {
        // The catcher keeps SDR frames: the front buffer's display values through the ramp, as
        // without HDR (the linear image's above 1 would index past the ramp's end).
        if (hdr) {
          list->SetPipelineState(pso.Get());
          list->SetGraphicsRootDescriptorTable(0, view(*image, false));
        }
        glitch.Record(device, list, ramp_slot, iw, ih, constants.ramp_mode, r.frame_index());
      }
      D3D12_CPU_DESCRIPTOR_HANDLE fsr_input;
      if (use_fsr && fsr.BeginInput(device, list, uint32_t(iw), uint32_t(ih), uint32_t(vw),
                                    uint32_t(vh), &fsr_input)) {
        BlitConstants at_size = constants;
        at_size.point_sampled = 1;
        const D3D12_VIEWPORT ivp = {0.0f, 0.0f, iw, ih, 0.0f, 1.0f};
        const D3D12_RECT isc = {0, 0, LONG(iw), LONG(ih)};
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootSignature(root_signature.Get());
        list->SetPipelineState(pso.Get());
        list->SetGraphicsRoot32BitConstants(1, sizeof(at_size) / 4, &at_size, 0);
        list->SetGraphicsRootConstantBufferView(
            2, ramp_buffer->GetGPUVirtualAddress() + uint64_t(ramp_slot) * kRampSlotBytes);
        list->SetGraphicsRootDescriptorTable(0, view(*image, false));
        list->RSSetViewports(1, &ivp);
        list->RSSetScissorRects(1, &isc);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->OMSetRenderTargets(1, &fsr_input, FALSE, nullptr);
        list->DrawInstanced(3, 1, 0, 0);
        if (clears) list->ClearRenderTargetView(rtv, black, clears, clear);
        fsr.Upscale(list, rtv, vx, vy, float(REXCVAR_GET(rexnative_fsr_sharpness)));
      } else if (use_fsr) {
        blit(*image, rtv);
      }
    }
    if (overlay_ok && OverlayFrameFor(d.width(), d.height(), &overlay_frame))
      overlay.Record(device, list, d.frame_slot(), rtv, d.width(), d.height(), hdr,
                     overlay_frame);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    list->ResourceBarrier(1, &b);
    if (found && !classic && REXCVAR_GET(rexnative_luma_probe))
      probe.Record(device, list, d.frame_slot(), image->texture, extent.width, extent.height,
                   mapping, rgb10a2, r.frame_index());
  }

  // The Xenos plugin's frame is captured alone (its own copy is the image); a snapshot takes it
  // as it is.
  if (classic && REXCVAR_GET(rexnative_capture_xenos) && !snapshot::Capturing()) return;
  // A request arms the capture of the next shown frame. With the Xenos copy, that frame's
  // resolves must reach guest memory synchronously (HoldFullReadback) from its start, or the
  // copy is the previous frame's; the native image alone is this frame's.
  Capture& c = capture;
  if (!c.armed &&
      (swap.capture_request < 0 ? CaptureRequested(r.frame_index()) : swap.capture_request > 0)) {
    if (glitch.on()) glitch.Mark(r.frame_index());
    c.armed = true;
    // Not with frame generation: holding the guest thread half a second makes FidelityFX see a
    // scene change, and its next 4 generated frames are copies of the real ones (no motion), the
    // very frames the capture burst takes (frame_generation.h).
    // Nor for a snapshot: the frame alone, without holding the game.
    c.xenos = REXCVAR_GET(rexnative_capture_xenos) && !r.frame_generation_on() &&
              !snapshot::Capturing();
    if (c.xenos) {
      capture_holding.store(true, std::memory_order_release);
      HoldFullReadback(true);
      return;
    }
  }
  if (c.armed && shown && (!c.pending || !c.xenos)) {
    c.armed = false;
    if (!found) {
      ReleaseCapture();
      REXLOG_WARN("ReXNative capture: frame {}: no native front buffer to capture (front "
                  "buffer {:08X}, texture {:08X})",
                  r.frame_index(), front_buffer_base, tex);
      return;
    }
    UINT64 total = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &c.footprint, nullptr, nullptr, &total);
    if (!CreateBuffer(device, D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST,
                      "capture readback", &c.readback)) {
      ReleaseCapture();
      return;
    }
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = c.readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = c.footprint;
    src.pResource = image->texture;  // COPY_SOURCE | shader resource (the Resolver's read state)
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    if (!c.xenos) {
      Shot shot;
      shot.readback = std::move(c.readback);
      shot.pitch = c.footprint.Footprint.RowPitch;
      shot.width = extent.width;
      shot.height = extent.height;
      shot.mapping = mapping;
      shot.rgb10a2 = rgb10a2;
      if (REXCVAR_GET(rexnative_capture_shown) && !classic) {
        shot.ramp.resize(kPwlWords);
        shot.ramp_mode = CopyRamp(rgb10a2, shot.ramp.data());
        if (shot.ramp_mode == kRampNone) shot.ramp.clear();
      }
      shot.frame = last_shot_frame = r.frame_index();
      shot.fence = r.frame_fence();
      shot.dir = capture_dir;
      if (snapshot::Capturing()) {
        shot.dir = snapshot::Dir();
        snapshot::Taken(shot.frame);
      }
      shots.push_back(std::move(shot));
      return;
    }
    c.pending = true;
    c.frame = r.frame_index();
    c.fence = r.frame_fence();
    std::memcpy(c.fetch, fetch, sizeof(fetch));
    c.base = fetch[1] & 0xFFFFF000u;
    c.width = extent.width;
    c.height = extent.height;
    c.mapping = mapping;
    c.rgb10a2 = rgb10a2;
    c.ramp_mode = REXCVAR_GET(rexnative_capture_shown) && !classic ? CopyRamp(rgb10a2, c.ramp)
                                                                   : kRampNone;
  }
}

Presenter::Presenter(NativeRenderer& renderer) : impl_(std::make_unique<Impl>(renderer)) {}
Presenter::~Presenter() = default;

bool Presenter::Initialize() { return impl_->Initialize(); }

bool Presenter::CaptureRequested(uint64_t frame) { return impl_->CaptureRequested(frame); }

void Presenter::ReadSwap(const uint8_t* base, const HookArgs& args, SwapCall* out) {
  // The front buffer texture's fetch constant (D3DBaseTexture + 28; + 16 in D3D 2.0.2135).
  out->texture = args.r[1];
  if (out->texture)
    for (int i = 0; i < 6; ++i)
      out->fetch[i] = Load32(base, out->texture + game::kTextureFetchOffset + 4 * i);
}

void Presenter::OnSwap(const SwapCall& swap, bool shown, const SwapCopy* copy,
                       const ResolvedImage* classic) {
  impl_->OnSwap(swap, shown, copy, classic);
}

bool Presenter::front_buffer_found() const { return impl_->front_buffer_found; }
uint32_t Presenter::front_buffer_base() const { return impl_->front_buffer_base; }
bool Presenter::capturing(uint64_t frame) const {
  return (impl_->capture.pending && impl_->capture.frame == frame) ||
         impl_->last_shot_frame == frame;
}
const ImageRect& Presenter::image_rect() const { return impl_->image_rect; }

const LumaProbe& Presenter::luma_probe() const { return impl_->probe.result(); }

bool Presenter::capture_holds_guest() const {
  return REXCVAR_GET(rexnative_capture_xenos) && !snapshot::Pending();
}

bool Presenter::capture_holding_guest() const {
  return impl_->capture_holding.load(std::memory_order_acquire);
}

void Presenter::AfterEndFrame() {
  Impl& m = *impl_;
  if (!m.shots.empty()) m.WriteShots();
  if (m.capture.pending) m.FinishCapture();
}

void Presenter::FinishShots() {
  Impl& m = *impl_;
  if (!m.shots.empty()) m.WriteShots();
  if (m.capture.pending) m.FinishCapture();
  for (std::thread& t : m.shot_writers) t.join();
  m.shot_writers.clear();
}

}  // namespace rexnative
