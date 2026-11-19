// ReXNative renderer: guest Resolve -> host texture. See resolve.h.
//
// Format packing follows ReXGlue/Xenia's resolve shaders and xenos.h conversions (BSD-3,
// Copyright Xenia/ReXGlue developers): 7e3 and 20e4 float encodings, unorm rounding, the
// red/blue swap of RB_COPY_DEST_INFO and the sample selections of RB_COPY_CONTROL.
// The oracle dumps and the self test are in resolve_debug.cpp.
#include "resolve.h"

#include "clock.h"
#include "gpu_timer.h"
#include "guest_memory.h"
#include "hook_args.h"
#include "log.h"
#include "texture_layout.h"
#include "upscaler.h"
#include "host.h"

#include "gpu/xenos/draw_state.h"
#include "gpu/xenos/host/fixed_point.h"

#include <rex/cvar.h>
#include <rex/kernel/heap.h>
#include "native/sdk.h"
#include <rex/logging.h>

#include <d3dcompiler.h>

#include <algorithm>
#include <bit>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

REXCVAR_DEFINE_INT32(rexnative_resolve_writeback, 0, "ReXNative",
                     "Experimental: resolves of at most this many texels, and any resolve into a "
                     "destination no texture or front buffer has used yet (up to "
                     "rexnative.resolve_writeback_unused), are also written into guest memory "
                     "once the GPU has done them, as the console's resolve writes them: one to "
                     "three frames late, so a game that reads them at once (Minecraft's 64x64 save "
                     "thumbnail) still reads zeros, and one that reuses the memory meanwhile has "
                     "it overwritten; 0 off");
REXCVAR_DEFINE_INT32(rexnative_resolve_writeback_unused, 1310720, "ReXNative",
                     "With rexnative.resolve_writeback: the largest resolve (texels) written back "
                     "because no texture has used its destination yet; 0 none");
REXCVAR_DEFINE_BOOL(rexnative_resolve_specialize, true, "ReXNative",
                    "Compile a copy shader per kind of resolve (formats, samples, swap, bias) on a "
                    "worker thread; resolves use the generic shader until it is ready");

namespace rexnative {
namespace {

namespace xenos = rex::graphics::xenos;
namespace texture_util = rex::graphics::texture_util;

constexpr uint32_t kViewsPerResolve = 4;  // t0 color/depth, t1 stencil, u0 texel bits, u1 floats
// Descriptors per frame slot: 1024 resolves (Halo: Combat Evolved Anniversary passed 128 in its
// cutscenes, and the ones after were skipped).
constexpr uint32_t kHeapPerFrame = 1024 * kViewsPerResolve;
constexpr uint32_t kHeapSlots = 4;                          // > frames in flight
static_assert(kHeapSlots > D3D12Device::kFramesInFlight);
constexpr D3D12_RESOURCE_STATES kImageReadState = D3D12_RESOURCE_STATE_COPY_SOURCE |
                                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

// How the shader writes a texel through the image's UAV (kShader's Store).
enum DestLayout : uint32_t {
  kR8,
  kR8G8,
  kR8G8B8A8,
  kR10G10B10A2,
  kR16,
  kR16G16,
  kR16G16B16A16,
  kR32,          // one to four 32-bit components
  kB5G6R5Unorm,  // no UINT view exists: unorm floats
  kB5G5R5A1,     // no UINT view: always the R16_UINT texel bits and a copy (UsesTexelBits)
};

// The host image of a copy destination format: the texture cache's host layout
// (GetHostFormat) in its typeless family, so the shader writes the Xenos texel bits through a
// UINT view and the cache binds or copies the image as its own format.
struct DestStorage {
  int32_t bpp_log2;
  DXGI_FORMAT resource;
  DXGI_FORMAT uav;
  DestLayout layout;
};

// nullptr if the resolve can't write the format.
const DestStorage* GetDestStorage(uint32_t f) {
  static constexpr DestStorage k8{0, DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UINT, kR8};
  static constexpr DestStorage k8_8{1, DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_UINT, kR8G8};
  // k_5_6_5 and k_6_5_5 share the B5G6R5 host layout (the texture cache's load conversions).
  static constexpr DestStorage k565{1, DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_B5G6R5_UNORM,
                                    kB5G6R5Unorm};
  // k_1_5_5_5 as the cache's B5G5R5A1 (R and B swapped on load). Fable II resolves three
  // 256x256 mip chains a frame into such textures.
  static constexpr DestStorage k1555{1, DXGI_FORMAT_B5G5R5A1_UNORM, DXGI_FORMAT_B5G5R5A1_UNORM,
                                     kB5G5R5A1};
  static constexpr DestStorage k16f{1, DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UINT, kR16};
  static constexpr DestStorage k8888{2, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UINT,
                                     kR8G8B8A8};
  static constexpr DestStorage k2101010{2, DXGI_FORMAT_R10G10B10A2_TYPELESS,
                                        DXGI_FORMAT_R10G10B10A2_UINT, kR10G10B10A2};
  static constexpr DestStorage k16_16f{2, DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_UINT,
                                       kR16G16};
  static constexpr DestStorage k32f{2, DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_UINT, kR32};
  static constexpr DestStorage k16x4f{3, DXGI_FORMAT_R16G16B16A16_TYPELESS,
                                      DXGI_FORMAT_R16G16B16A16_UINT, kR16G16B16A16};
  static constexpr DestStorage k32_32f{3, DXGI_FORMAT_R32G32_TYPELESS, DXGI_FORMAT_R32G32_UINT,
                                       kR32};
  static constexpr DestStorage k32x4f{4, DXGI_FORMAT_R32G32B32A32_TYPELESS,
                                      DXGI_FORMAT_R32G32B32A32_UINT, kR32};
  // 16-bit fixed point (From Dust's terrain height field is k_16_16 with the signed integer
  // number format and an exponent bias): the bits through UINT views like the float ones.
  static constexpr DestStorage k16{1, DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UINT, kR16};
  static constexpr DestStorage k16_16{2, DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_UINT,
                                      kR16G16};
  static constexpr DestStorage k16x4{3, DXGI_FORMAT_R16G16B16A16_TYPELESS,
                                     DXGI_FORMAT_R16G16B16A16_UINT, kR16G16B16A16};
  switch (xenos::TextureFormat(f)) {
    case xenos::TextureFormat::k_8:
      return &k8;
    case xenos::TextureFormat::k_8_8:
      return &k8_8;
    case xenos::TextureFormat::k_5_6_5:
    case xenos::TextureFormat::k_6_5_5:
      return &k565;
    case xenos::TextureFormat::k_1_5_5_5:
      return &k1555;
    case xenos::TextureFormat::k_16_FLOAT:
      return &k16f;
    case xenos::TextureFormat::k_8_8_8_8:
      return &k8888;
    case xenos::TextureFormat::k_2_10_10_10:
      return &k2101010;
    case xenos::TextureFormat::k_16_16_FLOAT:
      return &k16_16f;
    case xenos::TextureFormat::k_32_FLOAT:
      return &k32f;
    case xenos::TextureFormat::k_16_16_16_16_FLOAT:
      return &k16x4f;
    case xenos::TextureFormat::k_32_32_FLOAT:
      return &k32_32f;
    case xenos::TextureFormat::k_32_32_32_32_FLOAT:
      return &k32x4f;
    case xenos::TextureFormat::k_16:
      return &k16;
    case xenos::TextureFormat::k_16_16:
      return &k16_16;
    case xenos::TextureFormat::k_16_16_16_16:
      return &k16x4;
    default:
      return nullptr;
  }
}

// log2 of the bytes per texel of a copy destination format; -1 if the resolve can't write it.
int DestBppLog2(uint32_t f) {
  const DestStorage* s = GetDestStorage(f);
  return s ? s->bpp_log2 : -1;
}

// The guest D3D's mapping from the destination texture's format to RB_COPY_DEST_INFO's
// (Resolve: the cmplwi chain on the fetch format).
uint32_t D3DCopyDestFormat(uint32_t f) {
  using enum xenos::TextureFormat;
  switch (xenos::TextureFormat(f)) {
    case k_8_8_8_8_AS_16_16_16_16:  // Forza Horizon
      return uint32_t(k_8_8_8_8);
    case k_2_10_10_10_AS_16_16_16_16:
      return uint32_t(k_2_10_10_10);
    case k_10_11_11_AS_16_16_16_16:
      return uint32_t(k_10_11_11);
    case k_11_11_10_AS_16_16_16_16:
      return uint32_t(k_11_11_10);
    case k_16_EXPAND:
      return uint32_t(k_16_FLOAT);
    case k_16_16_EXPAND:
      return uint32_t(k_16_16_FLOAT);
    case k_16_16_16_16_EXPAND:
      return uint32_t(k_16_16_16_16_FLOAT);
    case k_24_8:
    case k_24_8_FLOAT:
      return uint32_t(k_8_8_8_8);
    default:
      return f;
  }
}

// Color render target formats (xenos::ColorRenderTargetFormat) that hold values above 1: 7e3
// and 16-bit float.
bool IsFloatTarget(uint32_t f) { return f == 3 || f == 7 || f == 12; }

bool IsFloatFormat(uint32_t f) {
  switch (xenos::TextureFormat(f)) {
    case xenos::TextureFormat::k_16_FLOAT:
    case xenos::TextureFormat::k_16_16_FLOAT:
    case xenos::TextureFormat::k_16_16_16_16_FLOAT:
    case xenos::TextureFormat::k_32_FLOAT:
    case xenos::TextureFormat::k_32_32_FLOAT:
    case xenos::TextureFormat::k_32_32_32_32_FLOAT:
      return true;
    default:
      return false;
  }
}

const char kShader[] = R"(
cbuffer Params : register(b0) {
  int2 src_origin;      // source rectangle origin in the render target (guest pixels)
  uint2 size;           // rectangle size
  uint2 dest_origin;    // destination point in the image
  uint cb_dest_format;  // xenos::TextureFormat
  uint cb_src_format;   // xenos::ColorRenderTargetFormat, or the depth format
  uint cb_flags;        // 1: swap, 4-6: sample select, 8-9: log2 of the samples, 10: the host's
                        // own samples, 11: alpha from src_alpha, 12-14: number, 15: also the
                        // converted color into dst_float
  int cb_exp_bias;
  uint cb_dest_layout;  // DestLayout of dst
  uint dest_scale;      // host texels of the image per guest texel (ResolvedImage::scale)
  float2 src_scale;     // host pixels of the render target per guest pixel (RenderScale)
  float2 alpha_scale;   // the same for src_alpha
};
// A specialized variant has the parameters of one kind of resolve compiled in, so the compiler
// drops the paths and conversions it does not take; the generic shader reads them above.
#if SPECIALIZED
#define dest_format ((uint)SPEC_DEST_FORMAT)
#define src_format ((uint)SPEC_SRC_FORMAT)
#define flags ((uint)SPEC_FLAGS)
#define exp_bias ((int)SPEC_EXP_BIAS)
#define dest_layout ((uint)SPEC_DEST_LAYOUT)
#else
#define dest_format cb_dest_format
#define src_format cb_src_format
#define flags cb_flags
#define exp_bias cb_exp_bias
#define dest_layout cb_dest_layout
#endif
#if DEPTH
#if MSAA
Texture2DMS<float> src_depth : register(t0);
Texture2DMS<uint2> src_stencil : register(t1);
#else
Texture2D<float> src_depth : register(t0);
Texture2D<uint2> src_stencil : register(t1);
#endif
#else
#if MSAA
Texture2DMS<float4> src : register(t0);
#else
Texture2D<float4> src : register(t0);
#endif
// Flag 11: src is the upscaler's output, which FSR leaves without alpha, and the alpha is the
// scene target's under the pixel's center (the motion blur's mask, upscaler.h).
Texture2D<float4> src_alpha : register(t1);
#endif
// The destination image through its UINT view (Xenos texel bits), or (B5G6R5, the R32_FLOAT
// image of depth resolves, and the linear image of flag 15) as floats.
RWTexture2D<uint4> dst : register(u0);
RWTexture2D<float4> dst_float : register(u1);

uint SampleSelect() { return (flags >> 4) & 7; }
uint SampleCount() { return 1u << ((flags >> 8) & 3u); }
// The target's samples are the host's own (--rexnative_msaa), not the guest's: a one-sample
// copy takes sample 0, and an averaging one all of them.
bool OwnSamples() { return (flags & 0x400u) != 0u; }
// Host sample of a guest sample s (0..3). SanitizeCopySampleSelect: 2x k2/k3 -> k0/k1, 1x -> 0.
// GetD3D10SampleIndexForGuest2xMSAA (native 2x): Xenos 2x sample 0 is the top one, which is
// sample 1 in Direct3D 10.1+; 4x samples map 1:1.
uint HostSample(uint s) {
  uint n = SampleCount();
  return n == 4u ? s : (n == 2u ? 1u - (s & 1u) : 0u);
}

uint U(float v, float scale) { return (uint)(saturate(v) * scale + 0.5); }
uint S8(float v) { return ((uint)(int)round(clamp(v, -1.0, 1.0) * 127.0)) & 0xFF; }
// A 16-bit fixed-point component by RB_COPY_DEST_INFO's number format (Xenia's resolve.xesli):
// 0 unorm, 1 snorm, 2 unsigned integer, 3 signed integer (exponent bias already applied).
uint F16(float v, uint number) {
  if (number == 1u) return ((uint)(int)round(clamp(v, -1.0, 1.0) * 32767.0)) & 0xFFFFu;
  if (number == 2u) return (uint)clamp(round(v), 0.0, 65535.0);
  if (number == 3u) return ((uint)(int)clamp(round(v), -32768.0, 32767.0)) & 0xFFFFu;
  return U(v, 65535.0);
}

// Xenia xenos::Float32To20e4 truncating, as ReXGlue's EDRAM store of host D32 depth
// (PreClampedDepthTo20e4 with depth_float24_round off, the plugin's default).
uint F20e4(float f) {
  uint u = asuint(clamp(f, 0.0, 1.99999988));
  if (u < 0x38800000u) {
    uint shift = 113u - (u >> 23);
    u = shift < 24u ? (((u & 0x7FFFFFu) | 0x800000u) >> shift) : 0u;
  } else {
    u += 0xC8000000u;
  }
  return (u >> 3) & 0xFFFFFFu;
}
float F20e4ToF32(uint v) {
  uint m = v & 0xFFFFFu, e = v >> 20;
  if (e == 0u) {
    if (m == 0u) return 0.0;
    uint lz = firstbithigh(m);  // 0..19
    return asfloat(((lz + 93u) << 23) | (((m << (20u - lz)) & 0xFFFFFu) << 3));
  }
  return asfloat(((e + 112u) << 23) | (m << 3));
}
// Float7e3To32(Float32To7e3(c)) without the encoding: from 2^-2 up, c rounded to nearest even
// at 7 mantissa bits; below, in 7e3's denormal steps of 2^-9, from c * 2^25 truncated as
// Float32To7e3's shift does (exact for the R16G16B16A16_FLOAT host targets either way).
float3 Quantize7e3(float3 c) {
  c = clamp(c, 0.0, 31.875);
  uint3 u = asuint(c);
  float3 normal = asfloat((u + 0x7FFFu + ((u >> 16) & 1u)) & 0xFFFF0000u);
  uint3 t = (uint3)(c * 33554432.0);
  float3 denormal = (float3)((t + 0x7FFFu + ((t >> 16) & 1u)) >> 16) * (1.0 / 512.0);
  return c < 0.25 ? denormal : normal;
}

#if !DEPTH
// One sample as the Xenos EDRAM holds it. The host target of a 7e3 surface is
// R16G16B16A16_FLOAT; ReXGlue packs every sample into 7e3 (alpha 2-bit unorm) when it stores
// the target to EDRAM before a resolve, so values below 7e3's smallest step (2^-9) become 0.
float4 EdramSample(float4 c) {
  if (src_format == 3u || src_format == 12u)
    return float4(Quantize7e3(c.rgb), (float)U(c.a, 3.0) * (1.0 / 3.0));
  // k_16_16 and k_16_16_16_16 targets are R16G16[B16A16]_SNORM holding the guest's -32..32 over
  // 32 (the draws' color exponent bias minus 5, draw.cpp): back to the guest's values, as the
  // EDRAM holds them. From Dust resolves its terrain height field from a k_16_16 target.
  if (src_format == 4u || src_format == 5u) return c * 32.0;
  return c;
}

float4 LoadColor(int2 p, float2 center) {
#if MSAA
  uint n = SampleCount(), s = SampleSelect();
  if (OwnSamples()) {
    if (s <= 3u) return EdramSample(src.Load(p, 0));
    float4 sum = 0.0;
    for (uint i = 0u; i < n; ++i) sum += EdramSample(src.Load(p, i));
    return sum / (float)n;
  }
  if (s <= 3u) return EdramSample(src.Load(p, HostSample(s)));
  if (n == 4u) {
    if (s == 6u)
      return (EdramSample(src.Load(p, 0)) + EdramSample(src.Load(p, 1)) + EdramSample(src.Load(p, 2)) +
              EdramSample(src.Load(p, 3))) * 0.25;
    if (s == 5u) return (EdramSample(src.Load(p, 2)) + EdramSample(src.Load(p, 3))) * 0.5;
  }
  return (EdramSample(src.Load(p, 0)) + EdramSample(src.Load(p, 1))) * 0.5;
#else
  float4 c = src.Load(int3(p, 0));
  if (flags & 0x800u) c.a = src_alpha.Load(int3(center * alpha_scale, 0)).a;
  return EdramSample(c);
#endif
}

// The copied color before the destination format: exponent bias and red/blue swap applied.
float4 Convert(float4 c) {
  c *= exp2((float)exp_bias);
  return (flags & 2u) ? c.bgra : c;
}

// Texels of the destination format (color copies are all conversions) of a Convert result.
uint4 PackDest(float4 c) {
  uint number = (flags >> 12) & 7u;
  switch (dest_format) {
    case 2u: return uint4(U(c.r, 255.0), 0, 0, 0);                                  // k_8
    case 10u: return uint4(U(c.r, 255.0) | (U(c.g, 255.0) << 8), 0, 0, 0);          // k_8_8
    // 16-bit formats in the texture cache's host layout (its load conversion applied):
    // k_5_6_5 as B5G6R5 (guest R in bits 0-4 moves to 11-15), k_6_5_5 as B5G6R5 holding
    // R | B << 5 | G << 11 (sampled with swizzle RBGG).
    case 4u: return uint4(U(c.b, 31.0) | (U(c.g, 63.0) << 5) | (U(c.r, 31.0) << 11), 0, 0, 0);
    case 5u: return uint4(U(c.r, 31.0) | (U(c.b, 63.0) << 5) | (U(c.g, 31.0) << 11), 0, 0, 0);
    // k_1_5_5_5 as B5G5R5A1 (guest R in bits 0-4 moves to 10-14).
    case 3u: return uint4(U(c.b, 31.0) | (U(c.g, 31.0) << 5) | (U(c.r, 31.0) << 10) | (U(c.a, 1.0) << 15), 0, 0, 0);
    case 6u:                                                                         // k_8_8_8_8
      if (number == 1u) return uint4(S8(c.r) | (S8(c.g) << 8) | (S8(c.b) << 16) | (S8(c.a) << 24), 0, 0, 0);
      return uint4(U(c.r, 255.0) | (U(c.g, 255.0) << 8) | (U(c.b, 255.0) << 16) | (U(c.a, 255.0) << 24), 0, 0, 0);
    case 7u:                                                                         // k_2_10_10_10
      return uint4(U(c.r, 1023.0) | (U(c.g, 1023.0) << 10) | (U(c.b, 1023.0) << 20) | (U(c.a, 3.0) << 30), 0, 0, 0);
    case 24u: return uint4(F16(c.r, number), 0, 0, 0);                              // k_16
    case 25u: return uint4(F16(c.r, number) | (F16(c.g, number) << 16), 0, 0, 0);  // k_16_16
    case 26u:                                                                       // k_16_16_16_16
      return uint4(F16(c.r, number) | (F16(c.g, number) << 16),
                   F16(c.b, number) | (F16(c.a, number) << 16), 0, 0);
    case 30u: return uint4(f32tof16(c.r), 0, 0, 0);
    case 31u: return uint4(f32tof16(c.r) | (f32tof16(c.g) << 16), 0, 0, 0);
    case 32u: return uint4(f32tof16(c.r) | (f32tof16(c.g) << 16), f32tof16(c.b) | (f32tof16(c.a) << 16), 0, 0);
    case 36u: return uint4(asuint(c.r), 0, 0, 0);
    case 37u: return uint4(asuint(c.r), asuint(c.g), 0, 0);
    case 38u: return asuint(c);
    default: return uint4(0, 0, 0, 0);
  }
}
#endif

// p: the host pixel, center: the guest pixel's center. floats: the depth float image's value, or
// the converted color.
uint4 PackPixel(int2 p, float2 center, out float4 floats) {
#if DEPTH
#if MSAA
  // Depth is never averaged (ReXGlue SanitizeCopySampleSelect): 4x k23 -> sample 2, else 0.
  uint sel = SampleSelect();
  uint s = OwnSamples() ? 0u : HostSample(sel <= 3u ? sel : (sel == 5u && SampleCount() == 4u ? 2u : 0u));
  float d = src_depth.Load(p, s);
  uint stencil = src_stencil.Load(p, s).g;
#else
  float d = src_depth.Load(int3(p, 0));
  uint stencil = src_stencil.Load(int3(p, 0)).g;
#endif
  // D24S8: round to the nearest even like ReXGlue's EDRAM store (OpRoundNE of d * 0xFFFFFF).
  uint d24 = src_format == 1u ? F20e4(d) : (uint)round(saturate(d) * 16777215.0);
  floats = (src_format == 1u ? F20e4ToF32(d24) : (float)d24 / 16777215.0).xxxx;
  return uint4((d24 << 8) | (stencil & 0xFFu), 0, 0, 0);
#else
  floats = Convert(LoadColor(p, center));
  return PackDest(floats);
#endif
}

// The components of the UINT view holding packed texel bits v.
uint4 Unpack(uint4 v) {
  switch (dest_layout) {
    case 0u: return uint4(v.x & 0xFFu, 0, 0, 0);
    case 1u: return uint4(v.x & 0xFFu, (v.x >> 8) & 0xFFu, 0, 0);
    case 2u: return uint4(v.x & 0xFFu, (v.x >> 8) & 0xFFu, (v.x >> 16) & 0xFFu, v.x >> 24);
    case 3u: return uint4(v.x & 0x3FFu, (v.x >> 10) & 0x3FFu, (v.x >> 20) & 0x3FFu, v.x >> 30);
    case 4u: return uint4(v.x & 0xFFFFu, 0, 0, 0);
    case 5u: return uint4(v.x & 0xFFFFu, v.x >> 16, 0, 0);
    case 6u: return uint4(v.x & 0xFFFFu, v.x >> 16, v.y & 0xFFFFu, v.y >> 16);
    default: return v;
  }
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  // One thread per host texel of the image: dest_scale x dest_scale per guest texel.
  const uint ds = max(dest_scale, 1u);
  if (id.x >= size.x * ds || id.y >= size.y * ds) return;
  float4 floats;
  // The texel's center in guest pixels, and the host pixel of the target under it (the same
  // pixel at scales of 1).
  const float2 center = float2(src_origin) + (float2(id.xy) + 0.5) / (float)ds;
  int2 hp = int2(center * src_scale);
  // At a scale of 2 or more the half-pixel offset leaves the target's first host column / row
  // undrawn by full-screen passes (Xenia's resolution scale fills them the same way).
  const int2 gap = int2(floor(src_scale * 0.5));
  if (src_origin.x == 0) hp.x = max(hp.x, gap.x);
  if (src_origin.y == 0) hp.y = max(hp.y, gap.y);
  uint4 v = PackPixel(hp, center, floats);
  uint2 p = dest_origin * ds + id.xy;
#if DEPTH
  dst[p] = Unpack(v);
  dst_float[p] = floats;
#else
  if (dest_layout == 8u) {  // B5G6R5: R in bits 11-15, exact unorm steps
    dst_float[p] = float4(float3((v.x >> 11) & 31u, (v.x >> 5) & 63u, v.x & 31u) / float3(31.0, 63.0, 31.0), 1.0);
  } else {
    dst[p] = Unpack(v);
    if (flags & 0x8000u) dst_float[p] = floats;
  }
#endif
}
)";

struct ShaderConstants {
  int32_t src_origin[2];
  uint32_t size[2];
  uint32_t dest_origin[2];
  uint32_t dest_format, src_format, flags;  // flags: kShader's cb_flags
  int32_t exp_bias;
  uint32_t dest_layout, dest_scale;
  float src_scale[2];
  float alpha_scale[2];
};
static_assert(sizeof(ShaderConstants) == 64);

// What a copy pipeline is compiled for: the generic ones read the resolve's parameters from the
// root constants, a specialized one has them compiled in (kShader's SPEC_* macros).
struct PipelineSpec {
  bool depth = false, msaa = false, specialized = false;
  uint32_t src_format = 0, dest_format = 0, flags = 0, dest_layout = 0;
  int32_t exp_bias = 0;

  static PipelineSpec Of(bool depth, bool msaa, const ShaderConstants& c) {
    return {depth, msaa, true, c.src_format, c.dest_format, c.flags, c.dest_layout, c.exp_bias};
  }
  // Unique per variant: the formats fit 4 and 6 bits, the layout 4, flags 15, the bias is
  // 6-bit signed.
  uint64_t Key() const {
    return uint64_t(depth) | uint64_t(msaa) << 1 | uint64_t(src_format & 15) << 2 |
           uint64_t(dest_format & 63) << 6 | uint64_t(dest_layout & 15) << 12 |
           uint64_t(flags & 0xFFFF) << 16 | uint64_t(uint32_t(exp_bias) & 63) << 32;
  }
};

// RB_COPY_DEST_BASE / RB_COPY_DEST_PITCH (0x2319, 0x231A) in the shadow block 0x2300
// (kDevShadow2300), as the guest Resolve that just ran left them, against the decoded copy. The
// Resolve zeroes RB_COPY_CONTROL and RB_COPY_DEST_INFO there afterwards, so those are checked
// offline against the trace (the dump's copy_control/dest_info vs the trace's RESOLVE lines,
// compare_resolves.py --trace).
void CheckAgainstShadow(const uint8_t* base, uint32_t device, const ResolveParams& p) {
  const uint32_t b = device + kDevShadow2300 + 0x19 * 4;
  const uint32_t dest = Load32(base, b), pitch = Load32(base, b + 4);
  // The D3D moves the base by the tiled offset of the rectangle origin minus the dest point;
  // only the unshifted case is compared.
  const bool origin = p.x0 == p.dest_x && p.y0 == p.dest_y;
  // RB_COPY_DEST_PITCH's height is the level height minus the dest point's y (r18 in the D3D).
  if ((!origin || dest == p.dest_base) &&
      ((pitch >> 16) & 0x3FFF) == p.dest_height - uint32_t(p.dest_y))
    return;
  REXNATIVE_LOG_FIRST(16, WARN,
                 "ReXNative resolve: decode vs D3D shadow: dest {:08X}/{:08X} pitch {:08X} "
                 "(decoded {}x{}, rect {},{}-{},{} point {},{})",
                 dest, p.dest_base, pitch, p.dest_width, p.dest_height, p.x0, p.y0, p.x1, p.y1,
                 p.dest_x, p.dest_y);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Decoding

bool DecodeResolve(const uint8_t* base, const HookArgs& args, ResolveParams* out) {
  ResolveParams p;
  const uint32_t device = args.device;
  const uint32_t flags = args.r[1];
  const uint32_t rect_ptr = args.r[2], tex = args.r[3], point_ptr = args.r[4];
  const uint32_t level = args.r[5], slice = args.r[6], color_ptr = args.r[7];
  if (level >= xenos::kTextureMaxMips) {
    REXNATIVE_LOG_FIRST(4, WARN, "ReXNative resolve: destination level {} out of range", level);
    return false;
  }
  p.regs = SurfaceRegsFromDevice(base, device);
  p.source = flags & 7;
  if (p.source > 4) {
    REXNATIVE_LOG_FIRST(4, WARN, "ReXNative resolve: source select {} unsupported", p.source);
    return false;
  }
  // Sample select: Flags bits 4-6 hold it plus one; 0 means "by the source's MSAA".
  uint32_t field = (flags >> 4) & 7;
  if (!field) {
    const uint32_t samples = SurfaceSamples(p.regs.surface_info);
    field = samples == 1 ? 1 : samples == 2 ? 5 : 7;
  }
  p.sample_select = field - 1;
  if (flags & 0x2000) {
    REXNATIVE_LOG_FIRST(4, WARN, "ReXNative resolve: null copy (Flags {:08X}) unsupported", flags);
    return false;
  }
  p.copy_command = p.source == 4 ? 0 : 1;
  p.exp_bias = int32_t(flags) >> 26;  // bits 26-31, signed

  // The destination texture's fetch constant: D3DBaseTexture + 28, 6 big-endian dwords.
  if (!tex) {
    REXNATIVE_LOG_FIRST(4, WARN, "ReXNative resolve: no destination texture");
    return false;
  }
  uint32_t fetch[6];
  for (int i = 0; i < 6; ++i) fetch[i] = Load32(base, tex + game::kTextureFetchOffset + 4 * i);
  TextureKey key;
  if (!TextureKeyFromFetch(fetch, &key)) {
    REXNATIVE_LOG_FIRST(4, WARN, "ReXNative resolve: invalid destination fetch constant {:08X} {:08X}",
                   fetch[0], fetch[1]);
    return false;
  }
  // A 3D destination: one depth slice (the slice argument), its base level only.
  p.dest_3d = key.dimension == xenos::DataDimension::k3D;
  if (p.dest_3d && level) {
    REXNATIVE_LOG_FIRST(4, WARN, "ReXNative resolve: 3D destination level {} unsupported", level);
    return false;
  }
  const uint32_t fetch_format = fetch[1] & 0x3F;
  p.dest_format = D3DCopyDestFormat(fetch_format);
  p.dest_endian = (fetch[1] >> 6) & 3;
  p.dest_swap = ((fetch[3] >> 1) & 7) != 0;  // swizzle X is not R
  {
    const uint32_t sign_x = (fetch[0] >> 2) & 3;
    const bool integer = fetch[3] & 1;
    p.dest_number = IsFloatFormat(p.dest_format) ? 7
                    : integer                    ? (sign_x == 1 ? 3 : 2)
                                                 : (sign_x == 1 ? 1 : 0);
  }
  if (DestBppLog2(p.dest_format) < 0) {
    REXNATIVE_LOG_FIRST(
        4, WARN,
        "ReXNative resolve: destination format {} (fetch format {}) unsupported, source {}, "
        "surface {:08X} {:08X} {:08X}",
        p.dest_format, fetch_format, p.source, p.regs.surface_info, p.regs.color_info[0],
        p.regs.depth_info);
    return false;
  }
  p.dest_width = std::max(1u, key.width >> level);
  p.dest_height = std::max(1u, key.height >> level);
  // Destination address of the level and slice. The texture object holds virtual addresses;
  // GpuAddress is also the physical address the GPU-side fetch constant (and so the texture
  // cache) sees.
  const xenos::GuestTextureLayout layout = texture_util::GetGuestTextureLayout(
      key.dimension, key.pitch, key.width, key.height, key.depth_or_array_size, key.tiled,
      rex::graphics::GetBaseFormat(xenos::TextureFormat(fetch_format)), key.packed_mips, true,
      std::max(level, key.mip_max_level));
  const xenos::GuestTextureLayout::Level* lv;
  if (level == 0) {
    p.dest_base = GpuAddress(fetch[1] & 0xFFFFF000u);
    lv = &layout.base;
  } else {
    if (layout.packed_level != UINT32_MAX && level >= layout.packed_level) {
      REXNATIVE_LOG_FIRST(4, WARN,
                     "ReXNative resolve: packed mip tail destination (level {}) unsupported",
                     level);
      return false;
    }
    p.dest_base = GpuAddress(fetch[5] & 0xFFFFF000u) + layout.mip_offsets_bytes[level];
    lv = &layout.mips[level];
  }
  p.dest_tiled = key.tiled;
  p.dest_pitch_blocks = lv->row_pitch_bytes >> DestBppLog2(p.dest_format);
  if (p.dest_3d) {
    if (slice >= key.depth()) {
      REXNATIVE_LOG_FIRST(4, WARN, "ReXNative resolve: slice {} of a {}-deep 3D destination",
                          slice, key.depth());
      return false;
    }
    p.dest_extent_bytes = Slice3DKeyStrideBytes(*lv);
    p.dest_base += p.dest_extent_bytes * slice;
  } else {
    p.dest_base += lv->array_layer_stride_bytes * slice;
    p.dest_extent_bytes = lv->array_layer_data_extent_bytes;
  }

  // Rectangle: pSourceRect or the destination level's size; right/bottom rounded up to 8.
  if (rect_ptr) {
    p.x0 = int32_t(Load32(base, rect_ptr));
    p.y0 = int32_t(Load32(base, rect_ptr + 4));
    p.x1 = int32_t(Load32(base, rect_ptr + 8));
    p.y1 = int32_t(Load32(base, rect_ptr + 12));
  } else {
    p.x1 = int32_t(p.dest_width);
    p.y1 = int32_t(p.dest_height);
  }
  p.x1 = (p.x1 + 7) & ~7;
  p.y1 = (p.y1 + 7) & ~7;
  if (point_ptr) {
    p.dest_x = int32_t(Load32(base, point_ptr));
    p.dest_y = int32_t(Load32(base, point_ptr + 4));
  }
  // Clears: color from pClearColor (default: the D3D's constant at 0x82258A0C), depth from
  // ClearZ (f1), stencil from the stack argument (caller r1 + 92, read in Resolve).
  p.clear_flags = flags & 0x300;
  if (p.clear_flags) {
    if (color_ptr) {
      for (int i = 0; i < 4; ++i) p.clear_color[i] = LoadFloat(base, color_ptr + 4 * i);
    } else {
      // The D3D's default (game_layer.h): XDK 6132 splats one float, XDK 20353 copies a
      // 16-byte vector.
      for (int i = 0; i < 4; ++i)
        p.clear_color[i] = LoadFloat(
            base, rexnative::game::kResolveDefaultClearColor + (rexnative::game::kResolveDefaultClearIsVector ? 4u * i : 0u));
    }
    p.clear_depth = float(args.f[0]);
    p.clear_stencil =
        args.stencil >= 0 ? uint8_t(args.stencil) : uint8_t(Load32(base, args.sp + 92));
    if (args.tiling == TilingCall::kEnd) {
      const uint32_t tiles = std::min(Load32(base, device + kDevTileCount), kMaxTiles);
      for (uint32_t i = 0; i < tiles; ++i) {
        const uint32_t r = device + kDevTileRects + 16 * i;
        if (p.x0 >= int32_t(Load32(base, r)) && p.y0 >= int32_t(Load32(base, r + 4)) &&
            p.x0 < int32_t(Load32(base, r + 8)) && p.y0 < int32_t(Load32(base, r + 12))) {
          p.edram_x = int32_t(Load32(base, device + kDevTileOrigins + 8 * i));
          p.edram_y = int32_t(Load32(base, device + kDevTileOrigins + 8 * i + 4));
          break;
        }
      }
    }
  }
  CheckAgainstShadow(base, device, p);
  *out = p;
  return true;
}

namespace {

// A dword the GPU reads with `endian` (xenos::Endian), from memory as the host loads it.
uint32_t GpuSwap(uint32_t v, uint32_t endian) {
  switch (endian & 3) {
    case 1:  // 8in16
      return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    case 2:  // 8in32
      return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
    case 3:  // 16in32
      return (v << 16) | (v >> 16);
    default:
      return v;
  }
}

float HalfToFloat(uint32_t h) {
  const uint32_t sign = (h & 0x8000u) << 16, exp = (h >> 10) & 0x1F, man = h & 0x3FF;
  if (!exp) return (sign ? -1.0f : 1.0f) * float(man) * (1.0f / 16777216.0f);  // 2^-24
  if (exp == 31) return std::bit_cast<float>(sign | 0x7F800000u | (man << 13));
  return std::bit_cast<float>(sign | ((exp + 112) << 23) | (man << 13));
}

// 10-bit 7e3 (k_2_10_10_10_FLOAT's colour) and 24-bit 20e4 (D24FS8's depth), as ReXGlue's
// Float7e3To32 / Float20e4To32: no sign, the exponent biased by 3 / 15, denormals at exponent 0.
float SmallFloat(uint32_t v, uint32_t mantissa_bits, int32_t bias) {
  const uint32_t man = v & ((1u << mantissa_bits) - 1), exp = v >> mantissa_bits;
  const float m = float(man) / float(1u << mantissa_bits);
  return exp ? std::ldexp(1.0f + m, int32_t(exp) - bias) : std::ldexp(m, 1 - bias);
}

// The clear colour the guest packed for the EDRAM (RB_COLOR_CLEAR = the low 32 bits,
// RB_COLOR_CLEAR_LO the high 32 of a 64-bit format, as ReXGlue's resolve clear takes them) back
// to the values a D3D Clear gives (RenderTargets::Clear scales k_16_16(_16_16)'s itself).
// `format`: xenos::ColorRenderTargetFormat.
bool UnpackClearColor(uint32_t format, uint32_t lo, uint32_t hi, float out[4]) {
  auto unorm = [&](uint32_t v, uint32_t shift, uint32_t bits) {
    return float((v >> shift) & ((1u << bits) - 1)) / float((1u << bits) - 1);
  };
  auto fixed16 = [&](uint32_t v) {  // -32..32 over the signed 16-bit range
    return std::max(float(int16_t(v & 0xFFFF)), -32767.0f) * (32.0f / 32767.0f);
  };
  switch (format) {
    case 0:  // k_8_8_8_8
    case 1:  // k_8_8_8_8_GAMMA
      for (uint32_t i = 0; i < 4; ++i) out[i] = unorm(lo, 8 * i, 8);
      return true;
    case 2:   // k_2_10_10_10
    case 10:  // k_2_10_10_10_AS_10_10_10_10
      for (uint32_t i = 0; i < 3; ++i) out[i] = unorm(lo, 10 * i, 10);
      out[3] = unorm(lo, 30, 2);
      return true;
    case 3:   // k_2_10_10_10_FLOAT
    case 12:  // k_2_10_10_10_FLOAT_AS_16_16_16_16
      for (uint32_t i = 0; i < 3; ++i) out[i] = SmallFloat((lo >> (10 * i)) & 0x3FF, 7, 3);
      out[3] = unorm(lo, 30, 2);
      return true;
    case 4:  // k_16_16
      out[0] = fixed16(lo);
      out[1] = fixed16(lo >> 16);
      out[2] = 0.0f;
      out[3] = 1.0f;
      return true;
    case 5:  // k_16_16_16_16
      out[0] = fixed16(lo);
      out[1] = fixed16(lo >> 16);
      out[2] = fixed16(hi);
      out[3] = fixed16(hi >> 16);
      return true;
    case 6:  // k_16_16_FLOAT
      out[0] = HalfToFloat(lo & 0xFFFF);
      out[1] = HalfToFloat(lo >> 16);
      out[2] = 0.0f;
      out[3] = 1.0f;
      return true;
    case 7:  // k_16_16_16_16_FLOAT
      out[0] = HalfToFloat(lo & 0xFFFF);
      out[1] = HalfToFloat(lo >> 16);
      out[2] = HalfToFloat(hi & 0xFFFF);
      out[3] = HalfToFloat(hi >> 16);
      return true;
    case 14:  // k_32_FLOAT
      out[0] = std::bit_cast<float>(lo);
      out[1] = out[2] = 0.0f;
      out[3] = 1.0f;
      return true;
    case 15:  // k_32_32_FLOAT
      out[0] = std::bit_cast<float>(lo);
      out[1] = std::bit_cast<float>(hi);
      out[2] = 0.0f;
      out[3] = 1.0f;
      return true;
    default:
      return false;
  }
}

}  // namespace

bool DecodeRegisterResolve(const uint8_t* physical, const ResolveRegisters& r,
                           ResolveParams* out) {
  ResolveParams p;
  p.from_registers = true;
  p.regs = r.surface;
  p.source = r.copy_control & 7;
  p.copy_command = (r.copy_control >> 20) & 3;
  if (p.source > 4 || p.copy_command > 1) {
    REXNATIVE_LOG_FIRST(4, WARN,
                        "ReXNative resolve (registers): source {} copy command {} unsupported",
                        p.source, p.copy_command);
    return false;
  }
  const bool depth = p.source == 4;
  const uint32_t msaa = (p.regs.surface_info >> 16) & 3;
  p.sample_select = uint32_t(rex::gpu::draw_state::SanitizeCopySampleSelect(
      xenos::CopySampleSelect((r.copy_control >> 4) & 7), xenos::MSAASamples(msaa), depth));

  // The rectangle: the three vertices of fetch constant 0 (a 6-float vertex buffer), in the
  // 16.8 fixed point of the rasterizer, top-left inclusive.
  const uint32_t f0 = r.vertex_fetch[0], f1 = r.vertex_fetch[1];
  const uint32_t vertices = f0 & 0x1FFFFFFCu;
  if ((f0 & 3) != 3 || ((f1 >> 2) & 0xFFFFFF) != 6 || vertices + 24 > 0x20000000u) {
    REXNATIVE_LOG_FIRST(4, WARN,
                        "ReXNative resolve (registers): vertex fetch {:08X} {:08X} is not the "
                        "rectangle's",
                        f0, f1);
    return false;
  }
  const float half_pixel = (r.vtx_cntl & 1) ? 0.0f : 0.5f;  // pix_center kD3DZero
  int32_t v[6];
  for (uint32_t i = 0; i < 6; ++i) {
    uint32_t raw;
    std::memcpy(&raw, physical + vertices + 4 * i, 4);
    v[i] = rex::gpu::FloatToD3D11Fixed16p8(std::bit_cast<float>(GpuSwap(raw, f1)) + half_pixel);
  }
  int32_t x0 = (std::min({v[0], v[2], v[4]}) + 127) >> 8;
  int32_t y0 = (std::min({v[1], v[3], v[5]}) + 127) >> 8;
  int32_t x1 = (std::max({v[0], v[2], v[4]}) + 127) >> 8;
  int32_t y1 = (std::max({v[1], v[3], v[5]}) + 127) >> 8;
  // PA_SC_WINDOW_OFFSET: two signed 15-bit fields.
  const int32_t wx = int32_t(r.window_offset << 17) >> 17;
  const int32_t wy = int32_t((r.window_offset >> 16) << 17) >> 17;
  if (r.su_sc_mode_cntl & (1u << 16)) {  // vtx_window_offset_enable
    x0 += wx;
    y0 += wy;
    x1 += wx;
    y1 += wy;
  }
  // The scissor, as ReXGlue's GetScissor: the window scissor (moved by the window offset unless
  // window_offset_disable) inside the screen scissor.
  int32_t sx0 = int32_t(r.window_scissor_tl & 0x7FFF), sy0 = int32_t((r.window_scissor_tl >> 16) & 0x7FFF);
  int32_t sx1 = int32_t(r.window_scissor_br & 0x7FFF), sy1 = int32_t((r.window_scissor_br >> 16) & 0x7FFF);
  if (!(r.window_scissor_tl >> 31)) {
    sx0 += wx;
    sy0 += wy;
    sx1 += wx;
    sy1 += wy;
  }
  sx0 = std::max({sx0, int32_t(r.screen_scissor_tl << 17) >> 17, 0});
  sy0 = std::max({sy0, int32_t((r.screen_scissor_tl >> 16) << 17) >> 17, 0});
  sx1 = std::max(std::min(sx1, int32_t(r.screen_scissor_br << 17) >> 17), sx0);
  sy1 = std::max(std::min(sy1, int32_t((r.screen_scissor_br >> 16) << 17) >> 17), sy0);
  x0 = std::clamp(x0, sx0, sx1) & ~7;
  y0 = std::clamp(y0, sy0, sy1) & ~7;
  x1 = (std::clamp(x1, sx0, sx1) + 7) & ~7;
  y1 = (std::clamp(y1, sy0, sy1) + 7) & ~7;
  const int32_t pitch = int32_t(p.regs.surface_info & 0x3FFF) & ~7;
  x0 = std::min(x0, pitch);
  x1 = std::min(x1, pitch);
  if (x0 >= x1 || y0 >= y1) {
    REXNATIVE_LOG_FIRST(4, WARN, "ReXNative resolve (registers): empty rectangle {},{}-{},{}", x0,
                        y0, x1, y1);
    return false;
  }
  p.x0 = x0;
  p.y0 = y0;
  p.x1 = x1;
  p.y1 = y1;

  // The destination: a 2D tiled level from RB_COPY_DEST_BASE, each source pixel at its own
  // coordinates there.
  const uint32_t info = r.copy_dest_info;
  if (info & 8) {
    REXNATIVE_LOG_FIRST(4, WARN, "ReXNative resolve (registers): array destination (info {:08X}) "
                                 "unsupported", info);
    return false;
  }
  p.dest_endian = info & 7;
  p.dest_format = D3DCopyDestFormat((info >> 7) & 0x3F);
  p.dest_number = (info >> 13) & 7;
  p.exp_bias = int32_t(info << 10) >> 26;  // bits 16-21, signed
  p.dest_swap = (info >> 24) & 1;
  const int bpp_log2 = DestBppLog2(p.dest_format);
  if (bpp_log2 < 0) {
    REXNATIVE_LOG_FIRST(4, WARN,
                        "ReXNative resolve (registers): destination format {} unsupported, "
                        "source {}, surface {:08X} {:08X} {:08X}",
                        p.dest_format, p.source, p.regs.surface_info, p.regs.color_info[0],
                        p.regs.depth_info);
    return false;
  }
  const uint32_t dest_pitch = r.copy_dest_pitch & 0x3FFF;
  const uint32_t dest_rows = (r.copy_dest_pitch >> 16) & 0x3FFF;
  p.dest_base = GpuAddress(r.copy_dest_base);
  p.dest_x = x0;
  p.dest_y = y0;
  // The guest writes the level's height minus the destination point's row, and moves the base
  // that many rows down when the point is below the rectangle's origin: from the base, the level
  // has that height plus the rectangle's first row. The pitch is 32-aligned: a copy that reaches
  // its last column of tiles gives the level's width (a 560-wide texture has a pitch of 576).
  p.dest_width = dest_pitch > uint32_t(x1) && dest_pitch - uint32_t(x1) < 32 ? uint32_t(x1)
                                                                            : dest_pitch;
  p.dest_height = dest_rows + uint32_t(y0);
  if (!p.dest_width || !p.dest_height) {
    REXNATIVE_LOG_FIRST(4, WARN, "ReXNative resolve (registers): destination pitch {:08X}",
                        r.copy_dest_pitch);
    return false;
  }
  p.dest_tiled = true;
  p.dest_pitch_blocks = (dest_pitch + 31) & ~31u;
  p.dest_extent_bytes = (p.dest_pitch_blocks * ((p.dest_height + 31) & ~31u)) << bpp_log2;

  // The clears after the copy (bits 8 and 9), with the values the guest packed.
  p.clear_flags = r.copy_control & 0x300;
  if (p.clear_flags & 0x100) {
    const uint32_t format = (p.regs.color_info[depth ? 0 : p.source] >> 16) & 0xF;
    if (!UnpackClearColor(format, r.color_clear, r.color_clear_lo, p.clear_color)) {
      REXNATIVE_LOG_FIRST(4, WARN, "ReXNative resolve (registers): clear colour of format {} not "
                                   "decoded (left 0)", format);
    }
  }
  if (p.clear_flags & 0x200) {
    const uint32_t d24 = r.depth_clear >> 8;
    p.clear_depth = (p.regs.depth_info >> 16) & 1 ? SmallFloat(d24, 20, 15)
                                                  : float(d24) * (1.0f / 16777215.0f);
    p.clear_stencil = uint8_t(r.depth_clear & 0xFF);
  }
  *out = p;
  return true;
}

bool DecodeRegisterClear(const uint8_t* physical, const ResolveRegisters& r, ClearCall* out) {
  constexpr uint32_t kVertexDwords = 7;  // x, y, z, r, g, b, a
  const uint32_t f0 = r.vertex_fetch[0], f1 = r.vertex_fetch[1];
  const uint32_t vertices = f0 & 0x1FFFFFFCu;
  if ((f0 & 3) != 3 || ((f1 >> 2) & 0xFFFFFF) != 3 * kVertexDwords ||
      vertices + 3 * kVertexDwords * 4 > 0x20000000u) {
    REXNATIVE_LOG_FIRST(4, WARN,
                        "ReXNative clear (registers): vertex fetch {:08X} {:08X} is not a clear "
                        "quad's; left undone",
                        f0, f1);
    return false;
  }
  float v[3 * kVertexDwords];
  for (uint32_t i = 0; i < 3 * kVertexDwords; ++i) {
    uint32_t raw;
    std::memcpy(&raw, physical + vertices + 4 * i, 4);
    v[i] = std::bit_cast<float>(GpuSwap(raw, f1));
  }
  // The rectangle the three corners span, in pixels (centers at +0.5 unless pix_center is
  // kD3DZero), moved by the window offset.
  const float half_pixel = (r.vtx_cntl & 1) ? 0.0f : 0.5f;
  auto edge = [&](uint32_t c, bool high) {
    const float a = v[c], b = v[kVertexDwords + c], d = v[2 * kVertexDwords + c];
    const float e = high ? std::max({a, b, d}) : std::min({a, b, d});
    return int32_t(std::lround(std::clamp(e + half_pixel, -16384.0f, 16384.0f)));
  };
  int32_t x0 = edge(0, false), y0 = edge(1, false), x1 = edge(0, true), y1 = edge(1, true);
  if (r.su_sc_mode_cntl & (1u << 16)) {  // vtx_window_offset_enable
    const int32_t wx = int32_t(r.window_offset << 17) >> 17;
    const int32_t wy = int32_t((r.window_offset >> 16) << 17) >> 17;
    x0 += wx;
    y0 += wy;
    x1 += wx;
    y1 += wy;
  }
  const int32_t pitch = int32_t(r.surface.surface_info & 0x3FFF);
  x0 = std::clamp(x0, 0, pitch);
  x1 = std::clamp(x1, 0, pitch);
  y0 = std::clamp(y0, 0, 8192);
  y1 = std::clamp(y1, 0, 8192);
  ClearCall c;
  ClearParams& p = c.p;
  // The color targets the quad writes; one whose RB_COLORn_INFO repeats an earlier one's is that
  // surface again (the D3D leaves RB_COLOR_MASK 0xFFFF and RB_COLOR1..3_INFO 0 for one target).
  for (uint32_t i = 0; i < 4; ++i) {
    if (!((r.color_mask >> (4 * i)) & 0xF)) continue;
    bool again = false;
    for (uint32_t j = 0; j < i; ++j)
      again |= (p.flags >> j & 1) && r.surface.color_info[j] == r.surface.color_info[i];
    if (!again) p.flags |= 1u << i;
  }
  c.bound_colors = p.flags & 0xF;
  if ((r.depth_control & 6) == 6) p.flags |= 0x10;  // z_enable, z_write_enable
  if ((r.depth_control & 1) && (r.stencil_ref_mask >> 16 & 0xFF)) p.flags |= 0x20;
  c.depth_bound = (p.flags & 0x30) != 0;
  if (!p.flags || x0 >= x1 || y0 >= y1) {
    REXNATIVE_LOG_FIRST(4, WARN,
                        "ReXNative clear (registers): nothing to clear (mask {:08X}, depth "
                        "control {:08X}, rectangle {},{}-{},{})",
                        r.color_mask, r.depth_control, x0, y0, x1, y1);
    return false;
  }
  for (uint32_t i = 0; i < 4; ++i) p.color[i] = v[3 + i];
  p.depth = v[2];
  p.stencil = uint8_t(r.stencil_ref_mask & 0xFF);
  p.rects.push_back({LONG(x0), LONG(y0), LONG(x1), LONG(y1)});
  p.edram_alias = true;
  c.regs = r.surface;
  c.height = uint32_t(y1);
  REXNATIVE_LOG_FIRST(1, INFO,
                      "ReXNative clear (registers): the D3D's clear quad in a replayed buffer, "
                      "surface {:08X} flags {:X} color {},{},{},{} depth {} rectangle {},{}-{},{}",
                      r.surface.surface_info, p.flags, p.color[0], p.color[1], p.color[2],
                      p.color[3], p.depth, x0, y0, x1, y1);
  *out = std::move(c);
  return true;
}

// ---------------------------------------------------------------------------------------------
// Resolver

struct Resolver::Image {
  ComPtr<ID3D12Resource> texture, depth_r32f, linear;
  // This port, B5G6R5 without a typed UAV store: the shader's R16_UINT texel bits and the buffer
  // they go through into `texture` (copies between the two formats need raw bytes).
  ComPtr<ID3D12Resource> bits, staging;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT bits_footprint{}, texture_footprint{};
  const DestStorage* storage = nullptr;
  uint32_t width = 0, height = 0;
  D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;  // of all its resources
  ResolvedImage view;
};

// The copy pipelines. Variants are looked up and queued on the recording thread and compiled on
// the worker; Variant::done publishes the pipeline.
struct Resolver::Pipelines {
  enum class Specialize { kOff, kAsync, kSync };
  struct Variant {
    ComPtr<ID3D12PipelineState> pso;  // null if compilation failed
    std::atomic<bool> done{false};
  };

  ID3D12Device* device = nullptr;
  pD3DCompile compile = nullptr;
  ComPtr<ID3D12RootSignature> root_signature;
  ComPtr<ID3D12PipelineState> generic[4];  // [depth][msaa]
  Specialize specialize = Specialize::kAsync;
  std::unordered_map<uint64_t, std::unique_ptr<Variant>> variants;

  std::mutex mutex;
  std::condition_variable cv;
  std::deque<std::pair<PipelineSpec, Variant*>> jobs;
  bool stopping = false;
  std::thread worker;

  ~Pipelines() {
    if (!worker.joinable()) return;
    {
      std::lock_guard lock(mutex);
      stopping = true;
    }
    cv.notify_all();
    worker.join();
  }

  ComPtr<ID3D12PipelineState> Compile(const PipelineSpec& s) const {
    std::vector<D3D_SHADER_MACRO> defines = {{"DEPTH", s.depth ? "1" : "0"},
                                             {"MSAA", s.msaa ? "1" : "0"},
                                             {"SPECIALIZED", s.specialized ? "1" : "0"}};
    const std::string values[] = {std::to_string(s.src_format) + "u",
                                  std::to_string(s.dest_format) + "u",
                                  std::to_string(s.flags) + "u",
                                  std::to_string(s.dest_layout) + "u", std::to_string(s.exp_bias)};
    if (s.specialized) {
      const char* names[] = {"SPEC_SRC_FORMAT", "SPEC_DEST_FORMAT", "SPEC_FLAGS",
                             "SPEC_DEST_LAYOUT", "SPEC_EXP_BIAS"};
      for (size_t i = 0; i < std::size(names); ++i)
        defines.push_back({names[i], values[i].c_str()});
    }
    defines.push_back({nullptr, nullptr});
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(compile(kShader, sizeof(kShader) - 1, "rexnative_resolve.hlsl", defines.data(), nullptr,
                       "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors))) {
      REXLOG_ERROR("ReXNative resolve: shader {:09X}{}: {}", s.Key(),
                   s.specialized ? "" : " (generic)",
                   errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
      return nullptr;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = root_signature.Get();
    pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    ComPtr<ID3D12PipelineState> pso;
    if (Failed(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)),
               "resolve CreateComputePipelineState"))
      return nullptr;
    return pso;
  }

  // The specialized pipeline of the spec once compiled, else the generic one; queues the
  // variant the first time it is asked for.
  ID3D12PipelineState* Get(const PipelineSpec& s) {
    ID3D12PipelineState* fallback = generic[(s.depth ? 2 : 0) + (s.msaa ? 1 : 0)].Get();
    if (specialize == Specialize::kOff) return fallback;
    auto [it, added] = variants.try_emplace(s.Key());
    if (added) {
      it->second = std::make_unique<Variant>();
      if (specialize == Specialize::kSync) {
        it->second->pso = Compile(s);
        it->second->done.store(true, std::memory_order_release);
      } else {
        {
          std::lock_guard lock(mutex);
          jobs.emplace_back(s, it->second.get());
        }
        if (!worker.joinable()) {
          worker = std::thread([this] { Work(); });
        }
        cv.notify_one();
      }
    }
    const Variant& v = *it->second;
    return v.done.load(std::memory_order_acquire) && v.pso ? v.pso.Get() : fallback;
  }

  void Work() {
    SetThreadDescription(GetCurrentThread(), L"ReXNative resolve shaders");
    for (;;) {
      std::pair<PipelineSpec, Variant*> job;
      {
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return stopping || !jobs.empty(); });
        if (stopping) return;
        job = jobs.front();
        jobs.pop_front();
      }
      const uint64_t t0 = NowNs();
      job.second->pso = Compile(job.first);
      job.second->done.store(true, std::memory_order_release);
      REXLOG_INFO("ReXNative resolve: copy shader {:09X} {} in {:.1f} ms", job.first.Key(),
                  job.second->pso ? "specialized" : "failed", double(NowNs() - t0) / 1e6);
    }
  }
};

struct Resolver::Writeback {
  ComPtr<ID3D12Resource> readback;
  uint64_t size = 0;
  uint64_t fence = 0;
  ResolveParams p;
  int32_t x = 0, y = 0;  // the texels' place in the destination level
  uint32_t width = 0, height = 0, row_pitch = 0;
};

Resolver::Resolver() = default;
Resolver::~Resolver() = default;
bool Resolver::Initialize(ID3D12Device* device, RenderTargets* targets, ID3D12CommandQueue* queue) {
  device_ = device;
  targets_ = targets;
  D3D12_DESCRIPTOR_HEAP_DESC hd = {};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors = kHeapPerFrame * kHeapSlots;
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (Failed(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_)),
             "resolve CreateDescriptorHeap"))
    return false;
  heap_stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = {DXGI_FORMAT_B5G6R5_UNORM};
  b5g6r5_store_ =
      SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))) &&
      (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);
  if (!b5g6r5_store_)
    REXLOG_INFO("ReXNative resolve: no typed UAV store for B5G6R5; k_5_6_5 resolves go through "
                "R16_UINT texel bits and a buffer");
  if (!CreatePipelines()) return false;
  InitializeOracles(queue);
  pipelines_->specialize = REXCVAR_GET(rexnative_resolve_specialize)
                               ? Pipelines::Specialize::kAsync
                               : Pipelines::Specialize::kOff;
  return true;
}

bool Resolver::CreatePipelines() {
  // Root signature: b0 = the root constants (ShaderConstants), one table of t0-t1 (sources) and
  // u0-u1 (images).
  D3D12_DESCRIPTOR_RANGE ranges[2] = {};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[0].NumDescriptors = 2;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  ranges[1].NumDescriptors = 2;
  ranges[1].OffsetInDescriptorsFromTableStart = 2;
  D3D12_ROOT_PARAMETER params[2] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants.Num32BitValues = sizeof(ShaderConstants) / 4;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable.NumDescriptorRanges = 2;
  params[1].DescriptorTable.pDescriptorRanges = ranges;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC rd = {};
  rd.NumParameters = 2;
  rd.pParameters = params;
  ComPtr<ID3DBlob> blob, error;
  if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error))) {
    REXLOG_ERROR("ReXNative resolve: root signature: {}",
                 error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
    return false;
  }
  pipelines_ = std::make_unique<Pipelines>();
  Pipelines& pl = *pipelines_;
  pl.device = device_;
  if (Failed(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                          IID_PPV_ARGS(&pl.root_signature)),
             "resolve CreateRootSignature"))
    return false;

  pl.compile = D3DCompiler();
  if (!pl.compile) return false;
  for (int depth = 0; depth < 2; ++depth) {
    for (int msaa = 0; msaa < 2; ++msaa) {
      PipelineSpec s;
      s.depth = depth;
      s.msaa = msaa;
      if (!(pl.generic[depth * 2 + msaa] = pl.Compile(s))) return false;
    }
  }
  return true;
}

Resolver::Image* Resolver::GetImage(const ResolveParams& p, Destination& dest, uint32_t scale) {
  const DestStorage& storage = *GetDestStorage(p.dest_format);
  // 16-bit packed formats without a typed UAV store: R16_UINT texel bits, then a copy.
  const bool bits565 =
      storage.layout == kB5G5R5A1 || (storage.layout == kB5G6R5Unorm && !b5g6r5_store_);
  const bool depth = p.source == 4;
  for (const std::unique_ptr<Image>& img : dest.images)
    if (img->storage == &storage && img->width == p.dest_width && img->height == p.dest_height &&
        bool(img->depth_r32f) == depth && img->view.scale == scale)
      return img.get();
  auto img = std::make_unique<Image>();
  img->storage = &storage;
  img->width = p.dest_width;
  img->height = p.dest_height;
  img->view.scale = scale;
  D3D12_RESOURCE_DESC d = {};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = p.dest_width * scale;
  d.Height = p.dest_height * scale;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.Format = storage.resource;
  d.SampleDesc.Count = 1;
  d.Flags = bits565 ? D3D12_RESOURCE_FLAG_NONE : D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  // Written by the shader (UAV), or with bits565 by a copy.
  img->state = bits565 ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, img->state, nullptr,
                                              IID_PPV_ARGS(&img->texture)),
             "resolve destination texture"))
    return nullptr;
  if (bits565) {
    D3D12_RESOURCE_DESC bd = d;
    bd.Format = DXGI_FORMAT_R16_UINT;
    bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    uint64_t bytes = 0;
    device_->GetCopyableFootprints(&bd, 0, 1, 0, &img->bits_footprint, nullptr, nullptr, &bytes);
    img->texture_footprint = img->bits_footprint;
    img->texture_footprint.Footprint.Format = storage.resource;
    D3D12_RESOURCE_DESC buf = {};
    buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf.Width = bytes;
    buf.Height = buf.DepthOrArraySize = buf.MipLevels = 1;
    buf.SampleDesc.Count = 1;
    buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                IID_PPV_ARGS(&img->bits)),
               "resolve 16-bit texel bits") ||
        Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &buf,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&img->staging)),
               "resolve 16-bit staging buffer"))
      return nullptr;
  }
  if (depth) {
    d.Format = DXGI_FORMAT_R32_FLOAT;
    if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                IID_PPV_ARGS(&img->depth_r32f)),
               "resolve depth_r32f texture"))
      return nullptr;
  }
  img->view.guest_base = p.dest_base;
  img->view.format = xenos::TextureFormat(p.dest_format);
  img->view.texture = img->texture.Get();
  img->view.depth_r32f = img->depth_r32f.Get();
  // Creation stalls the frame-recording thread (the render thread by default); every one is
  // logged so frame-time spikes can be matched against it.
  REXLOG_INFO("ReXNative resolve: image {} {} {:08X}: format {} {}x{}{}{} ({} there)",
              ++image_count_, &dest == &own_ ? "for copies of" : "at", p.dest_base, p.dest_format,
              p.dest_width, p.dest_height, scale > 1 ? std::format(" at {}x", scale) : "",
              depth ? " + depth float" : "", dest.images.size() + 1);
  return dest.images.emplace_back(std::move(img)).get();
}

const ResolvedImage* Resolver::Find(uint32_t guest_phys_base) const {
  auto it = destinations_.find(guest_phys_base);
  if (it == destinations_.end() || !it->second.current) return nullptr;
  it->second.sampled = true;
  return &it->second.current->view;
}

const ResolvedImage* Resolver::FindContaining(uint32_t address, uint32_t* row) const {
  for (const auto& [base, dest] : destinations_) {
    if (!dest.current || address <= base) continue;
    const ResolvedImage& v = dest.current->view;
    if (!v.extent_bytes || !v.pitch_blocks || !v.bytes_per_texel || address - base >= v.extent_bytes)
      continue;
    const uint32_t delta = address - base;
    // Tiled: a row of 32x32 tiles is contiguous, pitch (32-aligned) x 32 texels.
    const uint32_t rows = v.tiled ? 32u : 1u;
    const uint32_t block = ((v.pitch_blocks + rows - 1) & ~(rows - 1)) * rows * v.bytes_per_texel;
    if (delta % block) continue;
    *row = delta / block * rows;
    if (*row >= v.height) continue;
    dest.sampled = true;
    return &v;
  }
  return nullptr;
}

void Resolver::BeginFrame(uint64_t frame, uint64_t frame_fence, uint64_t completed_fence) {
  frame_ = frame;
  frame_fence_ = frame_fence;
  heap_next_ = 0;
  if (!writebacks_.empty()) WritebackFlush(completed_fence);
  if (dump_) DumpBeginFrame(completed_fence);
}

void Resolver::WritebackRecord(CommandStream* list, const ResolveParams& p,
                               ID3D12Resource* texture, int32_t dx, int32_t dy, uint32_t w,
                               uint32_t h) {
  const uint32_t bpp = DestBytesPerTexel(p.dest_format);
  // A 16-bit texel swapped in 32-bit units would trade places with its neighbour: not seen.
  if (!bpp || (bpp == 2 && p.dest_endian > 1) || dx < 0 || dy < 0) return;
  constexpr uint32_t kAlign = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
  const uint32_t row_pitch = (w * bpp + kAlign - 1) & ~(kAlign - 1);
  const uint64_t size = uint64_t(row_pitch) * h;
  std::unique_ptr<Writeback> wb;
  for (auto it = writeback_pool_.begin(); it != writeback_pool_.end(); ++it) {
    if ((*it)->size >= size) {
      wb = std::move(*it);
      writeback_pool_.erase(it);
      break;
    }
  }
  if (!wb) {
    wb = std::make_unique<Writeback>();
    wb->size = size;
    if (!CreateBuffer(device_, D3D12_HEAP_TYPE_READBACK, size, D3D12_RESOURCE_STATE_COPY_DEST,
                      "resolve writeback", &wb->readback))
      return;
  }
  static std::unordered_map<uint32_t, uint32_t> logged;  // each destination's first size
  if (logged.size() < 64 && logged.emplace(p.dest_base, w * h).second)
    REXLOG_INFO("ReXNative resolve writeback: {:08X} format {} {}x{} at {},{} of {}x{}{}",
                p.dest_base, p.dest_format, w, h, dx, dy, p.dest_width, p.dest_height,
                p.from_registers ? " (registers)" : "");
  wb->fence = frame_fence_;
  wb->p = p;
  wb->x = dx;
  wb->y = dy;
  wb->width = w;
  wb->height = h;
  wb->row_pitch = row_pitch;
  D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
  src.pResource = texture;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst.pResource = wb->readback.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint.Footprint = {DestResourceFormat(p.dest_format), w, h, 1, row_pitch};
  const D3D12_BOX box = {UINT(dx), UINT(dy), 0, UINT(dx) + w, UINT(dy) + h, 1};
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
  writebacks_.push_back(std::move(wb));
}

void Resolver::WritebackFlush(uint64_t completed_fence) {
  static uint64_t count = 0, bytes = 0, since = 0;
  if (!since) since = frame_;
  if (frame_ >= since + 600) {
    REXLOG_INFO("ReXNative resolve writeback: {:.1f} resolves, {:.0f} KB a frame written back to "
                "guest memory",
                double(count) / double(frame_ - since),
                double(bytes) / 1024.0 / double(frame_ - since));
    count = bytes = 0;
    since = frame_;
  }
  uint8_t* physical = rexnative::host::Memory()->GetPhysicalMembase();
  auto* view = static_cast<rex::kernel::PhysicalHeap*>(
      rexnative::host::Memory()->LookupHeap(0xA0000000u));
  size_t kept = 0;
  for (size_t i = 0; i < writebacks_.size(); ++i) {
    std::unique_ptr<Writeback>& wb = writebacks_[i];
    if (wb->fence > completed_fence) {
      writebacks_[kept++] = std::move(wb);
      continue;
    }
    const ResolveParams& p = wb->p;
    const uint32_t bpp = DestBytesPerTexel(p.dest_format);
    const uint32_t bpp_log2 = uint32_t(std::countr_zero(bpp));
    // The texture cache's watches on these pages: their callbacks run and the host may write.
    if (view && p.dest_extent_bytes)
      view->TriggerCallbacks(0xA0000000u + p.dest_base, p.dest_extent_bytes, true, false);
    void* data = nullptr;
    const D3D12_RANGE range = {0, size_t(wb->size)};
    if (SUCCEEDED(wb->readback->Map(0, &range, &data))) {
      const uint8_t* rows = static_cast<const uint8_t*>(data);
      for (uint32_t y = 0; y < wb->height; ++y) {
        const uint8_t* src = rows + size_t(y) * wb->row_pitch;
        const int32_t gy = wb->y + int32_t(y);
        for (uint32_t x = 0; x < wb->width; ++x, src += bpp) {
          const int32_t gx = wb->x + int32_t(x);
          const uint32_t off =
              p.dest_tiled ? uint32_t(texture_util::GetTiledOffset2D(gx, gy, p.dest_pitch_blocks,
                                                                     bpp_log2))
                           : (uint32_t(gy) * p.dest_pitch_blocks + uint32_t(gx)) << bpp_log2;
          if (off + bpp > p.dest_extent_bytes) continue;
          uint8_t* out = physical + p.dest_base + off;
          if (bpp == 1) {
            *out = *src;
          } else if (bpp == 2) {
            out[0] = src[p.dest_endian == 1 ? 1 : 0];
            out[1] = src[p.dest_endian == 1 ? 0 : 1];
          } else {
            for (uint32_t k = 0; k < bpp; k += 4) {
              uint32_t v;
              std::memcpy(&v, src + k, 4);
              switch (p.dest_endian & 3) {
                case 1: v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu); break;
                case 2: v = __builtin_bswap32(v); break;
                case 3: v = (v << 16) | (v >> 16); break;
                default: break;
              }
              std::memcpy(out + k, &v, 4);
            }
          }
        }
      }
      const D3D12_RANGE none = {0, 0};
      wb->readback->Unmap(0, &none);
    }
    ++count;
    bytes += uint64_t(wb->width) * wb->height * bpp;
    writeback_pool_.push_back(std::move(wb));
  }
  writebacks_.resize(kept);
}

const Resolver::Image* Resolver::Holding(const ResolveParams& p, uint32_t* row) const {
  const DestStorage* storage = GetDestStorage(p.dest_format);
  const bool depth = p.source == 4;
  for (const auto& [base, dest] : destinations_) {
    const Image* img = dest.current;
    if (!img || p.dest_base <= base || img->storage != storage ||
        bool(img->depth_r32f) != depth || !img->view.tiled ||
        img->view.pitch_blocks != p.dest_pitch_blocks || !img->view.extent_bytes ||
        p.dest_base - base >= img->view.extent_bytes)
      continue;
    // A row of 32x32 tiles is contiguous: pitch (32-aligned) x 32 texels.
    const uint32_t block = (p.dest_pitch_blocks * 32u) << DestBppLog2(p.dest_format);
    const uint32_t delta = p.dest_base - base;
    if (delta % block) continue;
    *row = delta / block * 32u;
    if (*row + uint32_t(p.y0) >= img->height) continue;
    return img;
  }
  return nullptr;
}

bool Resolver::Resolve(CommandStream* list, const ResolveParams& p) {
  if (p.from_registers) {
    // The image already at the base, when the copy fits it: the guest writes the level's height
    // minus the destination row in some XDKs (6132-20353) and the whole height in others (Forza
    // Horizon's 21119), so a later tile (y0 256) would make an image taller than the level. The
    // first tile, at y0 0, made it with the level's size in both.
    const auto it = destinations_.find(p.dest_base);
    const Image* here = it != destinations_.end() ? it->second.current : nullptr;
    if (here) {
      if (here->storage == GetDestStorage(p.dest_format) &&
          bool(here->depth_r32f) == (p.source == 4) &&
          here->view.pitch_blocks == p.dest_pitch_blocks && here->width >= uint32_t(p.x1) &&
          here->height >= uint32_t(p.y1) &&
          (here->width != p.dest_width || here->height != p.dest_height)) {
        ResolveParams q = p;
        q.dest_width = here->width;
        q.dest_height = here->height;
        q.dest_extent_bytes = here->view.extent_bytes;
        return Resolve(list, q, nullptr);
      }
    }
    // A destination point below the rectangle's origin: the guest moved the base into the
    // level, which an earlier resolve made an image of (the first cascade of a stack resolved
    // from the level's base). The copy lands in that image, rows further down.
    uint32_t row = 0;
    if (const Image* img = Holding(p, &row)) {
      ResolveParams q = p;
      q.dest_base = img->view.guest_base;
      q.dest_y += int32_t(row);
      q.dest_width = img->width;
      q.dest_height = img->height;
      q.dest_extent_bytes = img->view.extent_bytes;
      return Resolve(list, q, nullptr);
    }
  }
  return Resolve(list, p, nullptr);
}

const ResolvedImage* Resolver::ResolveCopy(CommandStream* list, const ResolveParams& p) {
  return Resolve(list, p, &own_) && own_.current ? &own_.current->view : nullptr;
}

bool Resolver::Resolve(CommandStream* list, const ResolveParams& p, Destination* own) {
  if (!own) SceneNoteResolve(p);  // the game's resolves show where its scene is (scene.h)
  const bool depth = p.source == 4;
  if (!GetDestStorage(p.dest_format)) return false;
  RenderTarget* rt = depth ? targets_->GetDepth(p.regs, uint32_t(std::max(p.y1, 0)))
                           : targets_->GetColor(p.regs, p.source, uint32_t(std::max(p.y1, 0)));
  if (!rt) {
    REXNATIVE_LOG_FIRST(4, WARN, "ReXNative resolve: no host target for source {} (surface {:08X})",
                   p.source, p.regs.surface_info);
    return false;
  }
  // The source rectangle on the target. The clears cover all of it, the copy only what fits in
  // the destination level (ReXGlue's resolve clears the source rectangle whatever the copy).
  const int32_t x0 = std::max(p.x0, 0), y0 = std::max(p.y0, 0);
  const int32_t x1 = std::min<int32_t>(p.x1, int32_t(rt->width)),
                y1 = std::min<int32_t>(p.y1, int32_t(rt->height));
  if (x1 <= x0 || y1 <= y0) return true;  // nothing to copy or clear
  // A resolve taller than the predicated tiles: the surface is whole in the EDRAM again.
  if (rt->edram_rows && uint32_t(y1 - y0) > rt->edram_rows) rt->edram_rows = 0;
  const int32_t dx = p.dest_x + (x0 - p.x0), dy = p.dest_y + (y0 - p.y0);
  const int32_t copy_x1 = std::min<int32_t>(x1, x0 + int32_t(p.dest_width) - dx);
  const int32_t copy_y1 = std::min<int32_t>(y1, y0 + int32_t(p.dest_height) - dy);
  bool ok = true;
  if (copy_x1 > x0 && copy_y1 > y0 && dx >= 0 && dy >= 0) {
    const D3D12_RECT src = {LONG(x0), LONG(y0), LONG(copy_x1), LONG(copy_y1)};
    RenderTarget *source = rt, *alpha = nullptr;
    // After the game drew into the scene's surface again (Upscaler::EnterPostScene), its own
    // full-size target holds the upscaled scene and what came after: copied as it is.
    // A colour resolve only: a deferred renderer resolves the scene's depth before its lighting
    // (Midnight Club: Los Angeles, each frame's first full-width resolve of the scene's surface),
    // and upscaling there left the scene unlit, its lighting drawn after at the output's size.
    // The frame's last pass of the scene only (Upscaler::SceneResolveDue, which counts the passes
    // after the upscaled resolve too): an earlier pass's resolve copies the render-size target,
    // and the scene goes on at the render size.
    if (upscaler_ && !depth && Upscaler::IsScene(p.regs.surface_info) &&
        Upscaler::IsSceneResolve(p.x0, p.y0, p.x1, p.y1) && upscaler_->SceneResolveDue() &&
        !upscaler_->post_scene()) {
      RenderTarget* upscaled = upscaler_->Upscale(list, *targets_, p.regs);
      if (upscaled) upscaler_->NoteSceneResolve(p);
      if (upscaled && !depth) {
        source = upscaled;
        alpha = rt;
      }
      if (timer_) timer_->Pass(list, GpuTimer::KeyResolve(p));
    }
    targets_->PrepareRead(list, rt, src);
    ok = Copy(list, p, source, alpha, src, dx, dy, own ? *own : destinations_[p.dest_base]);
  }

  // Clears of the source surfaces over the source rectangle (after the copy).
  if (p.clear_flags && !own) {
    if (timer_) timer_->Pass(list, GpuTimer::KeyResolve(p, GpuTimer::kResolveClear));
    ClearParams cp;
    cp.flags = ((p.clear_flags & 0x100) ? (1u << (depth ? 0 : p.source)) : 0) |
               ((p.clear_flags & 0x200) ? 0x30u : 0);
    std::memcpy(cp.color, p.clear_color, sizeof(cp.color));
    cp.depth = p.clear_depth;
    cp.stencil = p.clear_stencil;
    cp.rects.push_back({LONG(x0), LONG(y0), LONG(x1), LONG(y1)});
    if (p.edram_x || p.edram_y)
      cp.edram_rects.push_back(
          {LONG(x0 - p.edram_x), LONG(y0 - p.edram_y), LONG(x1 - p.edram_x), LONG(y1 - p.edram_y)});
    // The resolve clears the EDRAM tiles: other depth surfaces on them are cleared as well.
    cp.edram_alias = true;
    targets_->Clear(list, p.regs, 0xF, true, uint32_t(std::max(p.y1, 0)), cp);
  }
  return ok;
}

bool Resolver::Copy(CommandStream* list, const ResolveParams& p, RenderTarget* rt,
                    RenderTarget* alpha, const D3D12_RECT& src, int32_t dx, int32_t dy,
                    Destination& dest) {
  const bool depth = p.source == 4;
  const DestStorage* storage = GetDestStorage(p.dest_format);
  const uint32_t w = uint32_t(src.right - src.left), h = uint32_t(src.bottom - src.top);
  // The image at the source's integer scale: the screen-sized surfaces' and the upscaler's
  // output at --rexnative_resolution_scale, every other target's (and a scene rendered smaller
  // by DLSS Super Resolution) at the guest's size.
  const uint32_t scale = std::max(
      1u, uint32_t(std::lround(std::max(rt->scale.factor(0), rt->size_scale.factor(0)) - 0.25f)));
  Image* img = GetImage(p, dest, scale);
  if (!img) {
    REXNATIVE_LOG_FIRST(4, WARN, "ReXNative resolve: no host image for format {} {}x{}", p.dest_format,
                   p.dest_width, p.dest_height);
    return false;
  }

  // A float target resolved into k_2_10_10_10 (PGR4's front buffer) loses everything above 1;
  // for HDR output the linear image keeps the converted color.
  const bool linear = linear_output_ && !depth &&
                      p.dest_format == uint32_t(xenos::TextureFormat::k_2_10_10_10) &&
                      IsFloatTarget(rt->guest_format);
  if (linear && !img->linear) {
    D3D12_RESOURCE_DESC d = img->texture->GetDesc();
    d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, img->state, nullptr,
                                                IID_PPV_ARGS(&img->linear)),
               "resolve linear texture"))
      return false;
    REXLOG_INFO("ReXNative resolve: linear image at {:08X} {}x{}", p.dest_base, img->width,
                img->height);
  }
  // The image's second resource, in the same state as its texture.
  ID3D12Resource* const extra = img->depth_r32f ? img->depth_r32f.Get() : img->linear.Get();

  // Descriptors: t0 = color / depth plane, t1 = stencil plane or alpha, u0 = the image's UINT
  // view, u1 = its float view (B5G6R5), the depth float image or the linear image; null where
  // unused.
  if (heap_next_ + kViewsPerResolve > kHeapPerFrame) {
    // The frame's earlier dispatches read their descriptors when the GPU runs them.
    REXNATIVE_LOG_FIRST(4, WARN, "ReXNative resolve: more than {} resolves in a frame; skipped",
                   kHeapPerFrame / kViewsPerResolve);
    return false;
  }
  const uint32_t slot = uint32_t(frame_ % kHeapSlots) * kHeapPerFrame + heap_next_;
  heap_next_ += kViewsPerResolve;
  D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap_->GetCPUDescriptorHandleForHeapStart();
  cpu.ptr += size_t(slot) * heap_stride_;
  auto view = [&](uint32_t i) {
    return D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + size_t(i) * heap_stride_};
  };
  D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap_->GetGPUDescriptorHandleForHeapStart();
  gpu.ptr += uint64_t(slot) * heap_stride_;
  D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
  sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sd.ViewDimension =
      rt->samples > 1 ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
  if (rt->samples == 1) sd.Texture2D.MipLevels = 1;
  sd.Format = rt->srv_format;
  device_->CreateShaderResourceView(rt->resource.Get(), &sd, view(0));
  if (depth) {
    sd.Format = rt->stencil_srv_format;
    if (rt->samples == 1) sd.Texture2D.PlaneSlice = 1;
    device_->CreateShaderResourceView(rt->resource.Get(), &sd, view(1));
  } else {
    D3D12_SHADER_RESOURCE_VIEW_DESC ad = {};
    ad.Format = alpha ? alpha->srv_format : DXGI_FORMAT_R8G8B8A8_UNORM;
    ad.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    ad.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    ad.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(alpha ? alpha->resource.Get() : nullptr, &ad, view(1));
  }
  const bool bits565 = bool(img->bits);
  const bool float_texels = storage->layout == kB5G6R5Unorm && !bits565;
  D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
  ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  ud.Format = float_texels ? DXGI_FORMAT_R32_UINT : bits565 ? DXGI_FORMAT_R16_UINT : storage->uav;
  device_->CreateUnorderedAccessView(float_texels ? nullptr
                                     : bits565    ? img->bits.Get()
                                                  : img->texture.Get(),
                                     nullptr, &ud, view(2));
  ud.Format = float_texels ? storage->uav
              : linear     ? DXGI_FORMAT_R16G16B16A16_FLOAT
                           : DXGI_FORMAT_R32_FLOAT;
  device_->CreateUnorderedAccessView(float_texels ? img->texture.Get()
                                     : linear     ? img->linear.Get()
                                     : bits565    ? nullptr
                                                  : img->depth_r32f.Get(),
                                     nullptr, &ud, view(3));

  // Source readable by the compute shader, the image(s) writable: one barrier batch.
  D3D12_RESOURCE_BARRIER bs[4];
  uint32_t nb = 0;
  constexpr D3D12_RESOURCE_STATES kSourceState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  for (RenderTarget* t : {rt, alpha}) {
    if (!t || t->state == kSourceState) continue;
    bs[nb++] = TransitionBarrier(t->resource.Get(), t->state, kSourceState);
    t->state = kSourceState;
  }
  if (img->state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS && !bits565) {
    bs[nb++] =
        TransitionBarrier(img->texture.Get(), img->state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (extra)
      bs[nb++] = TransitionBarrier(extra, img->state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }
  if (nb) list->ResourceBarrier(nb, bs);
  ShaderConstants c = {};
  c.src_origin[0] = src.left;
  c.src_origin[1] = src.top;
  c.size[0] = w;
  c.size[1] = h;
  c.dest_origin[0] = uint32_t(dx);
  c.dest_origin[1] = uint32_t(dy);
  c.dest_format = p.dest_format;
  c.src_format = rt->guest_format;
  c.flags = (p.dest_swap ? 2u : 0u) | (p.sample_select << 4) |
            (uint32_t(std::countr_zero(rt->samples)) << 8) |
            (rt->samples > 1 && rt->samples != rt->edram_samples ? 0x400u : 0u) |
            (alpha ? 0x800u : 0u) | (p.dest_number << 12) | (linear ? 0x8000u : 0u);
  c.exp_bias = p.exp_bias;
  // The R16_UINT texel bits go through the shader's 16-bit UINT path (PackDest already packs
  // k_5_6_5, k_6_5_5 and k_1_5_5_5 in the host's B5G6R5 / B5G5R5A1 layout).
  c.dest_layout = bits565 ? uint32_t(kR16) : storage->layout;
  c.dest_scale = scale;
  c.src_scale[0] = rt->scale.factor(0);
  c.src_scale[1] = rt->scale.factor(1);
  if (alpha) {
    c.alpha_scale[0] = alpha->scale.factor(0);
    c.alpha_scale[1] = alpha->scale.factor(1);
  }
  ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(pipelines_->root_signature.Get());
  list->SetPipelineState(pipelines_->Get(PipelineSpec::Of(depth, rt->samples > 1, c)));
  list->SetComputeRoot32BitConstants(0, sizeof(c) / 4, &c, 0);
  list->SetComputeRootDescriptorTable(1, gpu);
  if (timer_) timer_->Pass(list, GpuTimer::KeyResolve(p, GpuTimer::kResolveCopy));
  list->Dispatch((w * scale + 7) / 8, (h * scale + 7) / 8, 1);

  if (timer_) timer_->Pass(list, GpuTimer::KeyResolve(p, GpuTimer::kResolveRelease));
  nb = 0;
  if (bits565) {
    // bits -> staging buffer (R16_UINT footprint) -> texture (the same bytes as B5G6R5 or
    // B5G5R5A1).
    bs[0] = TransitionBarrier(img->bits.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                              D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, bs);
    D3D12_TEXTURE_COPY_LOCATION tex = {}, buf = {};
    tex.pResource = img->bits.Get();
    tex.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    buf.pResource = img->staging.Get();
    buf.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    buf.PlacedFootprint = img->bits_footprint;
    list->CopyTextureRegion(&buf, 0, 0, 0, &tex, nullptr);
    bs[0] = TransitionBarrier(img->staging.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                              D3D12_RESOURCE_STATE_COPY_SOURCE);
    bs[1] = TransitionBarrier(img->texture.Get(), img->state, D3D12_RESOURCE_STATE_COPY_DEST);
    list->ResourceBarrier(img->state == D3D12_RESOURCE_STATE_COPY_DEST ? 1 : 2, bs);
    tex.pResource = img->texture.Get();
    buf.PlacedFootprint = img->texture_footprint;
    list->CopyTextureRegion(&tex, 0, 0, 0, &buf, nullptr);
    bs[nb++] = TransitionBarrier(img->texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kImageReadState);
    bs[nb++] = TransitionBarrier(img->staging.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                 D3D12_RESOURCE_STATE_COPY_DEST);
    bs[nb++] = TransitionBarrier(img->bits.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  } else {
    bs[nb++] = TransitionBarrier(img->texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                 kImageReadState);
    if (extra)
      bs[nb++] = TransitionBarrier(extra, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kImageReadState);
  }
  list->ResourceBarrier(nb, bs);
  img->state = kImageReadState;
  img->view.linear = linear ? img->linear.Get() : nullptr;
  img->view.width = std::max(img->view.width, uint32_t(dx) + w);
  img->view.height = std::max(img->view.height, uint32_t(dy) + h);
  img->view.seq = ++seq_;
  img->view.extent_bytes = p.dest_extent_bytes;
  img->view.pitch_blocks = p.dest_pitch_blocks;
  img->view.bytes_per_texel = DestBytesPerTexel(p.dest_format);
  img->view.tiled = p.dest_tiled;
  dest.current = img;
  if (&dest == &own_) return true;
  if (changed_) changed_(p.dest_base);
  // Written back for the CPU (rexnative.resolve_writeback): a small resolve, and one into a
  // destination the GPU has not used (no texture, no front buffer looked it up).
  const int32_t writeback = REXCVAR_GET(rexnative_resolve_writeback);
  const uint64_t texels = uint64_t(w) * h;
  if (writeback > 0 && scale == 1 && !p.dest_3d &&
      (texels <= uint64_t(writeback) ||
       (!dest.sampled &&
        texels <= uint64_t(std::max(0, REXCVAR_GET(rexnative_resolve_writeback_unused))))))
    WritebackRecord(list, p, img->texture.Get(), dx, dy, w, h);

  if (dump_ && scale == 1 && !p.dest_3d) DumpRecord(list, p, img->view, uint32_t(dx) + w, uint32_t(dy) + h);
  return true;
}

DXGI_FORMAT Resolver::DestResourceFormat(uint32_t dest_format) {
  const DestStorage* s = GetDestStorage(dest_format);
  return s ? s->resource : DXGI_FORMAT_UNKNOWN;
}

uint32_t Resolver::DestBytesPerTexel(uint32_t dest_format) {
  const int bpp_log2 = DestBppLog2(dest_format);
  return bpp_log2 < 0 ? 0 : 1u << bpp_log2;
}

bool Resolver::IsFloatDest(uint32_t dest_format) { return IsFloatFormat(dest_format); }

void Resolver::SetSpecialization(bool sync) {
  pipelines_->specialize = sync ? Pipelines::Specialize::kSync : Pipelines::Specialize::kOff;
}

void Resolver::EraseDestination(uint32_t dest_base) {
  auto it = destinations_.find(dest_base);
  if (it == destinations_.end()) return;
  image_count_ -= it->second.images.size();
  destinations_.erase(it);
  if (changed_) changed_(dest_base);
}

}  // namespace rexnative
