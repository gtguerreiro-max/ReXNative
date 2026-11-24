// ReXNative renderer: the scene's upscaler. See upscaler.h. It runs NVIDIA DLSS / DLAA (dlss.h)
// or AMD FidelityFX Super Resolution through the FidelityFX API (pgr4-native's original upscaler,
// src/native/upscaler.cpp), whichever the GPU has (--rexnative_upscaler_backend).
#include "upscaler.h"

#include "clock.h"
#include "frame_generation.h"
#include "log.h"
#include "neural.h"
#include "resolve.h"

#include <ffx_upscale.h>
#include <dx12/ffx_api_dx12.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <vector>

REXCVAR_DEFINE_BOOL(rexnative_upscaler, false, "ReXNative",
                    "Render the 3D scene without MSAA and antialias or upscale it with NVIDIA "
                    "DLSS or AMD FSR (--rexnative_upscaler_backend, --rexnative_upscaler_mode) "
                    "from the start; bind_upscaler (F7) switches it");
REXCVAR_DEFINE_STRING(rexnative_upscaler_backend, "auto", "ReXNative",
                      "auto (NVIDIA DLSS where the GPU and nvngx_dlss.dll have it, AMD FSR "
                      "otherwise: FSR 4 on the GPUs that run it, FSR 3.1 elsewhere, the Steam "
                      "Deck among them), dlss or fsr");
REXCVAR_DEFINE_BOOL(rexnative_fsr_reactive, true, "ReXNative",
                    "FSR: a reactive mask made from the scene before and after its blended draws "
                    "without motion (particles, glows), so FSR 3 does not trail them "
                    "(FidelityFX's GenerateReactiveMask; Upscaler::SnapshotOpaque)");
REXCVAR_DEFINE_STRING(rexnative_fsr_version, "auto", "ReXNative",
                      "The FSR the FSR backend runs: auto (the newest this GPU has: FSR 4 on "
                      "AMD's GPUs with its driver library, the Steam Deck's under GE-Proton's "
                      "PROTON_FSR4_UPGRADE among them, FSR 3.1 elsewhere), 4 or 3 (the newest "
                      "FSR 4.x or 3.x the FidelityFX loader lists for this GPU)");
REXCVAR_DEFINE_BOOL(rexnative_fsr_auto, true, "ReXNative",
                    "With --rexnative_fsr_version=auto: FSR 4 gives way to FSR 3.1 for the rest "
                    "of the run when the GPU has no room for it (busy for most of each frame's "
                    "time); an explicit 4 or 3 is kept");
REXCVAR_DEFINE_DOUBLE(rexnative_fsr_auto_share, 0.9, "ReXNative",
                      "--rexnative_fsr_auto: the share of each frame's time the GPU is busy for "
                      "that makes FSR 4 give way (two windows of 120 frames in a row)");
REXCVAR_DEFINE_STRING(rexnative_upscaler_mode, "", "ReXNative",
                      "native (anti-aliasing at the game's resolution: DLAA, FSR Native AA), "
                      "quality, balanced, performance or ultra (from a smaller render size); "
                      "empty: --rexnative_dlss_mode");
REXCVAR_DEFINE_INT32(rexnative_upscaler_width, 1280, "ReXNative",
                     "Width the upscaler's scene renders at, upscaled to 1280 (320-1280; 1280 "
                     "with the height at the render height: antialiasing only)");
REXCVAR_DEFINE_INT32(rexnative_upscaler_height, 800, "ReXNative",
                     "Height the upscaler's scene renders at, upscaled to the render height "
                     "(--rexnative_render_height, 800 or 720; from a quarter of it up to it)");
REXCVAR_DEFINE_INT32(rexnative_upscaler_target_fps, 0, "ReXNative",
                     "Vary the upscaler's render size each frame, from the width and height "
                     "down to --rexnative_upscaler_min_height, so that the GPU keeps up with "
                     "this frame rate, or with the lower one the frames are shown at "
                     "(--rexnative_present_interval, frame generation; 0: a fixed size)");
REXCVAR_DEFINE_INT32(rexnative_upscaler_min_height, 360, "ReXNative",
                     "With --rexnative_upscaler_target_fps: the least height the scene renders "
                     "at (from a quarter of the render height up to it)");
REXCVAR_DEFINE_BOOL(rexnative_upscaler_depth_test, false, "ReXNative",
                    "The upscaler takes the frame's last pass of the scene whose draws test depth, "
                    "where by default it takes the last one whose draws write it: for a game that "
                    "draws a model into the scene's surface, resolves it and puts it in the scene "
                    "with a pass that only tests depth (Diablo III's hero creation and selection)");

REXCVAR_DEFINE_STRING(rexnative_dlss_mode, "dlaa", "ReXNative",
                      "With --rexnative_upscaler: dlaa (the game's resolution, anti-aliased by DLSS), "
                      "quality, balanced, performance or ultra (DLSS Super Resolution from a smaller "
                      "render size)");
REXCVAR_DEFINE_INT32(rexnative_dlss_preset, 0, "ReXNative",
                     "DLSS render preset (NVSDK_NGX_DLSS_Hint_Render_Preset_*: 0 the DLL's default, "
                     "10 J, 11 K, ...)");

namespace rexnative {

namespace {

constexpr DXGI_FORMAT kOutputFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr D3D12_RESOURCE_STATES kInputState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

// The scene's color for the upscaler in the range of the 7e3 EDRAM format, as the resolves read it: RGB in
// [0, 31.875], alpha in [0, 1], NaN 0 (resolve.cpp's Quantize7e3, Xenia's UnclampedFloat32To7e3).
// The host target is R16G16B16A16_FLOAT and keeps what the shaders write beyond that, such as
// the start lights' emissive colors above 31.875. Its root signature: the source's SRV and the
// destination's UAV in a table, then the size.
constexpr char kInputHlsl[] = R"(
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
cbuffer Constants : register(b0) { uint2 size; };
[numthreads(8, 8, 1)]
void main(uint2 p : SV_DispatchThreadID) {
  if (any(p >= size)) return;
  float4 c = src.Load(int3(p, 0));
  dst[p] = float4(clamp(c.rgb, 0.0, 31.875), saturate(c.a));
}
)";
// Per frame in flight: the input pass's SRV and UAV, EnterPostScene's copy's SRV, the opaque
// snapshot's SRV and UAV.
constexpr uint32_t kViewsPerFrame = 5;
constexpr uint32_t kOpaqueViews = 3;

// EnterPostScene's copy of the upscaled output into the scene surface's full-size target: a
// triangle over the target, each pixel loaded from the output (the same size).
constexpr char kCopyVs[] = R"(
float4 main(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
)";
constexpr char kCopyPs[] = R"(
Texture2D<float4> src : register(t0);
float4 main(float4 pos : SV_Position) : SV_Target { return src.Load(int3(pos.xy, 0)); }
)";

// The render size of one axis from its setting, at least a quarter of the scene's.
uint32_t RenderExtent(int32_t setting, uint32_t scene) {
  return uint32_t(std::clamp<int32_t>(setting, int32_t(scene / 4), int32_t(scene)));
}

// FidelityFX's messages, into the log.
void Message(uint32_t type, const wchar_t* message) {
  std::string s;
  const int n = WideCharToMultiByte(CP_UTF8, 0, message, -1, nullptr, 0, nullptr, nullptr);
  if (n > 1) {
    s.resize(size_t(n));
    WideCharToMultiByte(CP_UTF8, 0, message, -1, s.data(), n, nullptr, nullptr);
    s.resize(size_t(n - 1));
  }
  if (type == FFX_API_MESSAGE_TYPE_ERROR)
    REXNATIVE_LOG_FIRST(16, ERROR, "ReXNative upscaler: {}", s);
  else
    REXNATIVE_LOG_FIRST(16, WARN, "ReXNative upscaler: {}", s);
}

// RB_COLOR_INFO color formats with more range than 8 bits (scene.cpp's IsHdrFormat).
bool IsHdrColorFormat(uint32_t f) {
  return f == 3 || f == 5 || f == 6 || f == 7 || f == 12 || f == 14 || f == 15;
}

// --rexnative_upscaler_mode, or --rexnative_dlss_mode when it is empty ("native" is DLAA's).
// DLSS 5 runs inside the upscaler (on the scene after DLAA, or before the HUD from the upscaler's
// output): with --rexnative_upscaler off it did nothing at all, without a word (Halo CEA). It now
// brings the upscaler on, as DLAA at the game's resolution, on DLSS.
bool OnlyForDlss5() { return !REXCVAR_GET(rexnative_upscaler) && NeuralPass::Requested(); }

Dlss::Mode ModeSetting() {
  if (OnlyForDlss5()) return Dlss::Mode::kDlaa;
  std::string name = REXCVAR_GET(rexnative_upscaler_mode);
  if (name.empty()) name = REXCVAR_GET(rexnative_dlss_mode);
  return Dlss::ModeFromName(name);
}

const char* FsrModeName(Dlss::Mode mode) {
  switch (mode) {
    case Dlss::Mode::kQuality: return "Quality";
    case Dlss::Mode::kBalanced: return "Balanced";
    case Dlss::Mode::kPerformance: return "Performance";
    case Dlss::Mode::kUltraPerformance: return "Ultra Performance";
    default: return "Native AA";
  }
}

// What runs (Upscaler::Status), for the settings menu on the UI thread.
std::mutex g_status_mutex;
std::string g_status;
std::atomic<bool> g_status_on{false};

void SetStatus(std::string status) {
  std::lock_guard lock(g_status_mutex);
  g_status = std::move(status);
}

// "4.1.2" as {4, 1, 2}; what is not a number counts as 0.
std::array<int, 3> VersionNumbers(const char* name) {
  std::array<int, 3> v = {};
  for (size_t i = 0; i < v.size() && name && *name; ++i) {
    v[i] = std::atoi(name);
    while (*name && *name != '.') ++name;
    if (*name == '.') ++name;
  }
  return v;
}

}  // namespace

bool Upscaler::Requested() { return REXCVAR_GET(rexnative_upscaler) || NeuralPass::Requested(); }

std::string Upscaler::Status() {
  if (!g_status_on.load(std::memory_order_relaxed)) return {};
  std::lock_guard lock(g_status_mutex);
  return g_status;
}

void Upscaler::NoteOn(bool on) { g_status_on.store(on, std::memory_order_relaxed); }

// FSR 3.1 for the rest of the run in place of the newest (--rexnative_fsr_auto): set when FSR 4
// left the GPU no room, read by every context made after (the upscaler is made again).
std::atomic<bool> g_fsr3_this_run{false};

uint64_t Upscaler::FsrVersionId() const {
  const std::string want = g_fsr3_this_run.load(std::memory_order_relaxed) &&
                                   REXCVAR_GET(rexnative_fsr_version) == "auto"
                               ? std::string("3")
                               : REXCVAR_GET(rexnative_fsr_version);
  if (want != "3" && want != "4") return 0;
  ffxQueryDescGetVersions q = {};
  q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
  q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
  q.device = device_;
  uint64_t count = 0;
  q.outputCount = &count;
  if (api_->query(nullptr, &q.header) != FFX_API_RETURN_OK || !count) return 0;
  std::vector<uint64_t> ids(count);
  std::vector<const char*> names(count);
  q.versionIds = ids.data();
  q.versionNames = names.data();
  if (api_->query(nullptr, &q.header) != FFX_API_RETURN_OK) return 0;
  uint64_t id = 0;
  std::array<int, 3> best = {};
  for (uint64_t i = 0; i < count && i < ids.size(); ++i) {
    const std::array<int, 3> v = VersionNumbers(names[i]);
    if (v[0] == want[0] - '0' && (!id || v > best)) {
      id = ids[i];
      best = v;
    }
  }
  if (!id)
    REXLOG_WARN("ReXNative upscaler: no FSR {} for this GPU (--rexnative_fsr_version); the "
                "newest it has",
                want);
  return id;
}

const char* Upscaler::BackendName() const {
  return backend_ == Backend::kDlss ? "DLSS" : "FSR";
}

Upscaler::~Upscaler() {
  if (context_) api_->destroy(&context_, nullptr);
}

bool Upscaler::Initialize(ID3D12Device* device) {
  device_ = device;
  // The mode sets the render size (native: the game's own); no dynamic resolution.
  mode_ = ModeSetting();
  const double scale = Dlss::RenderScale(mode_);
  max_width_ = RenderExtent(int32_t(std::lround(OutputWidth() * scale)), OutputWidth());
  max_height_ = RenderExtent(int32_t(std::lround(OutputHeight() * scale)), OutputHeight());
  // DLSS where it runs, FSR otherwise (or as asked).
  const bool for_dlss5 = OnlyForDlss5();
  const std::string want = for_dlss5 ? std::string("dlss") : REXCVAR_GET(rexnative_upscaler_backend);
  if (want != "fsr" && dlss_.Initialize(device_)) {
    backend_ = Backend::kDlss;
    if (for_dlss5)
      REXLOG_INFO("ReXNative upscaler: on for DLSS 5 (--rexnative_upscaler is off), as DLAA at "
                  "the game's resolution");
  } else if (want == "dlss") {
    REXLOG_ERROR("ReXNative upscaler: DLSS unavailable ({})",
                 for_dlss5 ? "DLSS 5 needs it" : "--rexnative_upscaler_backend=dlss");
    return false;
  } else {
    backend_ = Backend::kFsr;
    api_ = LoadFfxApi();
    if (!api_) return false;
  }
  if (!SetLevel(DynamicResolution::kSteps) || !CreateContext(false)) return false;
  if (backend_ == Backend::kDlss) {
    REXLOG_INFO("ReXNative upscaler: {} from {}x{} to {}x{}", Dlss::Name(mode_), max_width_,
                max_height_, OutputWidth(), OutputHeight());
    SetStatus(std::format("NVIDIA {}, {}x{} to {}x{}", Dlss::Name(mode_), max_width_, max_height_,
                          OutputWidth(), OutputHeight()));
  }
  return CreateTargets() && CreateInputPass();
}

bool Upscaler::CreateInputPass() {
  const D3D12_DESCRIPTOR_RANGE ranges[2] = {
      {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
      {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1},
  };
  D3D12_ROOT_PARAMETER params[2] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable = {2, ranges};
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants = {0, 0, 2};
  const D3D12_ROOT_SIGNATURE_DESC rs = {2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ComPtr<ID3DBlob> blob, err;
  const D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                         kViewsPerFrame * D3D12Device::kFramesInFlight,
                                         D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  if (Failed(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err),
             "upscaler input root signature serialize") ||
      Failed(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                          IID_PPV_ARGS(&input_signature_)),
             "upscaler input root signature") ||
      Failed(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&input_heap_)), "upscaler input heap"))
    return false;
  input_stride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

  const pD3DCompile compile = D3DCompiler();
  constexpr char kName[] = "rexnative_upscaler_input.hlsl";
  ComPtr<ID3DBlob> code, error;
  if (!compile ||
      FAILED(compile(kInputHlsl, sizeof(kInputHlsl) - 1, kName, nullptr, nullptr, "main", "cs_5_0",
                     D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error))) {
    REXLOG_ERROR("ReXNative upscaler: {}: {}", kName,
                 error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
    return false;
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
  pd.pRootSignature = input_signature_.Get();
  pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
  return !Failed(device_->CreateComputePipelineState(&pd, IID_PPV_ARGS(&input_pso_)), kName);
}

void Upscaler::ClampInput(CommandStream* list, ID3D12Resource* color, DXGI_FORMAT format,
                          ID3D12Resource* target, uint32_t view_slot) {
  // In the frame's own views: those of a frame kFramesInFlight back, whose fence the device
  // waited for, are free.
  const uint32_t first =
      uint32_t(frame_ % D3D12Device::kFramesInFlight) * kViewsPerFrame + view_slot;
  D3D12_CPU_DESCRIPTOR_HANDLE cpu = input_heap_->GetCPUDescriptorHandleForHeapStart();
  cpu.ptr += size_t(first) * input_stride_;
  D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
  sd.Format = format;  // the scene's host format: float (HDR) or 8-bit, read as float4
  sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sd.Texture2D.MipLevels = 1;
  device_->CreateShaderResourceView(color, &sd, cpu);
  cpu.ptr += input_stride_;
  D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
  ud.Format = kOutputFormat;
  ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  device_->CreateUnorderedAccessView(target, nullptr, &ud, cpu);
  D3D12_GPU_DESCRIPTOR_HANDLE gpu = input_heap_->GetGPUDescriptorHandleForHeapStart();
  gpu.ptr += uint64_t(first) * input_stride_;

  const uint32_t size[2] = {render_width_, render_height_};
  ID3D12DescriptorHeap* heaps[] = {input_heap_.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(input_signature_.Get());
  list->SetPipelineState(input_pso_.Get());
  list->SetComputeRootDescriptorTable(0, gpu);
  list->SetComputeRoot32BitConstants(1, 2, size, 0);
  list->Dispatch((size[0] + 7) / 8, (size[1] + 7) / 8, 1);
}

bool Upscaler::WantsOpaqueSnapshot() const {
  return opaque_ && reactive_ && opaque_frame_ != frame_ && !post_scene() &&
         scene_passes_ + 1 == expected_pass_ && REXCVAR_GET(rexnative_fsr_reactive);
}

void Upscaler::SnapshotOpaque(CommandStream* list, RenderTargets& targets,
                              const SurfaceRegs& regs) {
  opaque_frame_ = frame_;  // once a frame, taken or not
  opaque_pass_ = 0;
  RenderTarget* color = targets.GetColor(regs, 0, SceneHeight());
  if (!color || color->samples != 1 || color->scale != scale()) return;
  const D3D12_RECT whole = {0, 0, LONG(SceneWidth()), LONG(SceneHeight())};
  targets.PrepareRead(list, color, whole);
  RenderTargets::Transition(list, color, kInputState);
  if (opaque_state_ != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
    const D3D12_RESOURCE_BARRIER b = TransitionBarrier(
        opaque_.Get(), opaque_state_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->ResourceBarrier(1, &b);
    opaque_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  }
  ClampInput(list, color->resource.Get(), color->srv_format, opaque_.Get(), kOpaqueViews);
  const D3D12_RESOURCE_BARRIER b = TransitionBarrier(opaque_.Get(), opaque_state_, kInputState);
  list->ResourceBarrier(1, &b);
  opaque_state_ = kInputState;
  opaque_pass_ = scene_passes_ + 1;
  REXNATIVE_LOG_FIRST(1, INFO,
                      "ReXNative upscaler: reactive mask from the scene before its blended draws "
                      "(pass {})",
                      opaque_pass_);
}

void Upscaler::NoteSceneColorResolve(uint32_t base, uint32_t extent_bytes) {
  if (!resolved_gbuffer_) return;
  if (gbuffer_dest_frame_ != frame_) {
    gbuffer_dest_frame_ = frame_;
    gbuffer_dest_count_ = 0;
  }
  for (uint32_t i = 0; i < gbuffer_dest_count_; ++i)
    if (gbuffer_dests_[i].lo == base) return;
  if (gbuffer_dest_count_ < gbuffer_dests_.size())
    gbuffer_dests_[gbuffer_dest_count_++] = {base, base + std::max(extent_bytes, 1u)};
}

bool Upscaler::ReadsGBuffer(uint32_t address) const {
  if (gbuffer_dest_frame_ != frame_) return false;
  for (uint32_t i = 0; i < gbuffer_dest_count_; ++i)
    if (address >= gbuffer_dests_[i].lo && address < gbuffer_dests_[i].hi) return true;
  return false;
}

bool Upscaler::GenerateReactive(CommandStream* list) {
  if (!reactive_ || opaque_frame_ != frame_ || opaque_pass_ == 0 ||
      opaque_pass_ != scene_passes_ || encoded_ || !REXCVAR_GET(rexnative_fsr_reactive))
    return false;
  // AMD's sample values (FidelityFX_FSR's fsrapirendermodule.cpp).
  ffxDispatchDescUpscaleGenerateReactiveMask g = {};
  g.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE_GENERATEREACTIVEMASK;
  g.colorOpaqueOnly = ffxApiGetResourceDX12(opaque_.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
  g.colorPreUpscale = ffxApiGetResourceDX12(input_.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
  g.outReactive =
      ffxApiGetResourceDX12(reactive_.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
  g.renderSize = {render_width_, render_height_};
  g.scale = 1.0f;
  g.cutoffThreshold = 0.2f;
  g.binaryValue = 0.9f;
  g.flags = FFX_UPSCALE_AUTOREACTIVEFLAGS_APPLY_TONEMAP |
            FFX_UPSCALE_AUTOREACTIVEFLAGS_APPLY_THRESHOLD |
            FFX_UPSCALE_AUTOREACTIVEFLAGS_USE_COMPONENTS_MAX;
  list->Call([this, g](CommandStream::List* l) mutable {
    g.commandList = l;
    if (const ffxReturnCode_t rc = api_->dispatch(&context_, &g.header); rc != FFX_API_RETURN_OK)
      REXNATIVE_WARN_ONCE("ReXNative upscaler: reactive mask dispatch failed ({})", rc);
  });
  return true;
}

void Upscaler::Time(CommandStream* list, GpuTimer::UpscalePhase phase) {
  if (timer_) timer_->Pass(list, GpuTimer::KeyUpscale(phase, render_width_, render_height_));
}

bool Upscaler::SetLevel(uint32_t level) {
  constexpr uint32_t n = DynamicResolution::kSteps;
  render_width_ = std::max(1u, (max_width_ * level + n / 2) / n);
  render_height_ = std::max(1u, (max_height_ * level + n / 2) / n);
  // AMD's recommended texture LOD bias for the render size (FSR docs, "Mipmap biasing"),
  // in the sampler key's quarter steps.
  const float ratio = std::min(float(render_width_) / float(OutputWidth()),
                               float(render_height_) / float(OutputHeight()));
  mip_bias_quarters_ = uint32_t(std::lround(-4.0f * (std::log2(ratio) - 1.0f)));
  const double display_ratio = double(OutputWidth()) / double(render_width_);
  phase_count_ = std::clamp(int32_t(std::ceil(8.0 * display_ratio * display_ratio)), 8, 64);
  return true;
}

bool Upscaler::CreateContext(bool debug) {
  debug_ = debug;
  reset_ = true;
  context_outdated_ = false;
  // DLSS creates its feature at the first evaluation for a setup (dlss.h). FSR's debug view has
  // no DLSS counterpart; the flag stays for the F8 key.
  if (backend_ == Backend::kDlss) return dlss_.available();
  if (context_) {
    api_->destroy(&context_, nullptr);
    context_ = nullptr;
  }
  ffxCreateBackendDX12Desc backend = {};
  backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
  backend.device = device_;
  ffxCreateContextDescUpscaleVersion version = {};
  version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
  version.header.pNext = &backend.header;
  version.version = FFX_UPSCALER_VERSION;
  // --rexnative_fsr_version: FSR 4 or 3.1 in place of the newest the GPU runs.
  ffxOverrideVersion override_version = {};
  override_version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
  override_version.header.pNext = &version.header;
  override_version.versionId = FsrVersionId();
  ffxCreateContextDescUpscale desc = {};
  desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
  desc.header.pNext = override_version.versionId ? &override_version.header : &version.header;
  // What the game's scene is (SceneKind, from the last Upscale): an HDR scene is linear light in
  // a float target before the game's exposure; an 8-bit one is the game's final colors.
  desc.flags = (kind_.hdr ? FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE
                          : 0u) |
               (kind_.depth_inverted ? FFX_UPSCALE_ENABLE_DEPTH_INVERTED : 0u) |
               (dynamic_ ? FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION : 0u) |
               (debug ? FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION : 0u);
  desc.maxRenderSize = {max_width_, max_height_};
  desc.maxUpscaleSize = {OutputWidth(), OutputHeight()};
  desc.fpMessage = Message;
  SetFfxWatermark(L"MLSR-WATERMARK", debug);
  if (const ffxReturnCode_t rc = api_->create(&context_, &desc.header, nullptr);
      rc != FFX_API_RETURN_OK) {
    REXLOG_ERROR("ReXNative upscaler: ffxCreateContext failed ({}){}", rc,
                 debug ? " with debug visualization" : "");
    context_ = nullptr;
    return false;
  }
  // The FidelityFX loader picks the newest upscaler this GPU runs: FSR 4 on AMD's, FSR 3.1
  // elsewhere.
  ffxQueryGetProviderVersion provider = {};
  provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
  const char* name = api_->query(&context_, &provider.header) == FFX_API_RETURN_OK &&
                             provider.versionName
                         ? provider.versionName
                         : "FSR";
  fsr4_ = name[0] == '4';
  REXLOG_INFO("ReXNative upscaler: {} {} from {}x{} to {}x{} ({}{})", name, FsrModeName(mode_),
              max_width_, max_height_, OutputWidth(), OutputHeight(),
              kind_.hdr ? "HDR" : "8-bit", kind_.depth_inverted ? ", reversed depth" : "");
  SetStatus(std::format("AMD FSR {} {}, {}x{} to {}x{}", name, FsrModeName(mode_), max_width_,
                        max_height_, OutputWidth(), OutputHeight()));
  return true;
}

bool Upscaler::PrepareEncodedOutput(const RenderTarget& like) {
  if (encoded_output_.resource && encoded_output_.resource_format == like.resource_format &&
      encoded_output_.view_format == like.view_format &&
      encoded_output_.width == output_.width && encoded_output_.height == output_.height)
    return true;
  encoded_output_.resource.Reset();
  if (!encoded_rtv_) {
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 1;
    if (Failed(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&encoded_rtv_)),
               "upscaler encoded output RTV heap"))
      return false;
  }
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC d = {};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = OutputWidth();
  d.Height = OutputHeight();
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.Format = like.resource_format;
  d.SampleDesc.Count = 1;
  d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                              D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                              IID_PPV_ARGS(&encoded_output_.resource)),
             "upscaler encoded output"))
    return false;
  encoded_output_.resource_format = like.resource_format;
  encoded_output_.view_format = like.view_format;
  encoded_output_.srv_format = like.srv_format;
  encoded_output_.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
  encoded_output_.width = output_.width;
  encoded_output_.height = output_.height;
  encoded_output_.scale = encoded_output_.size_scale = output_.scale;
  encoded_output_.samples = 1;
  encoded_output_.view = encoded_rtv_->GetCPUDescriptorHandleForHeapStart();
  D3D12_RENDER_TARGET_VIEW_DESC rv = {};
  rv.Format = like.view_format;
  rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
  device_->CreateRenderTargetView(encoded_output_.resource.Get(), &rv, encoded_output_.view);
  return true;
}

bool Upscaler::CreateTargets() {
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC d = {};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = OutputWidth();
  d.Height = OutputHeight();
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.Format = kOutputFormat;
  d.SampleDesc.Count = 1;
  d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                              IID_PPV_ARGS(&output_.resource)),
             "upscaler output"))
    return false;
  output_.resource_format = output_.view_format = output_.srv_format = kOutputFormat;
  output_.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  output_.width = SceneWidth();  // guest pixels, at out_scale_ host pixels each
  output_.height = SceneHeight();
  output_.scale = output_.size_scale = {{out_scale_, out_scale_}, {1, 1}};

  d.Width = max_width_;
  d.Height = max_height_;
  if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, input_state_, nullptr,
                                              IID_PPV_ARGS(&input_)),
             "upscaler input"))
    return false;
  // The reactive mask's (FSR only; a failure leaves FSR without one).
  opaque_.Reset();
  reactive_.Reset();
  opaque_frame_ = UINT64_MAX;
  if (backend_ == Backend::kFsr) {
    opaque_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_DESC r = d;
    r.Format = DXGI_FORMAT_R8_UNORM;
    if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, opaque_state_,
                                                nullptr, IID_PPV_ARGS(&opaque_)),
               "upscaler opaque scene") ||
        Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &r,
                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                IID_PPV_ARGS(&reactive_)),
               "upscaler reactive mask")) {
      opaque_.Reset();
      reactive_.Reset();
    }
  }

  d.Format = kMotionFormat;
  d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_CLEAR_VALUE zero = {};
  zero.Format = kMotionFormat;
  if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, motion_state_, &zero,
                                              IID_PPV_ARGS(&motion_)),
             "upscaler motion vectors"))
    return false;
  D3D12_DESCRIPTOR_HEAP_DESC hd = {};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  hd.NumDescriptors = 1;
  if (Failed(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&motion_rtv_)), "upscaler RTV heap"))
    return false;
  device_->CreateRenderTargetView(motion_.Get(), nullptr,
                                  motion_rtv_->GetCPUDescriptorHandleForHeapStart());
  return true;
}

bool Upscaler::BeginFrame(uint64_t frame, uint64_t gpu_busy_ns, uint64_t paced_ns) {
  frame_ = frame;
  // The scene's passes (SceneResolveDue): the last frame's last pass that wrote depth is this
  // one's.
  expect_depth_ = depth_pass_ != 0;
  expected_pass_ = expect_depth_ ? depth_pass_ : 1;
  scene_passes_ = depth_pass_ = most_depth_draws_ = pass_depth_draws_ = 0;
  resolved_depth_ = pass_drawn_ = pass_gbuffer_ = false;
  bool resized = false;
  if (dynamic_) {
    const uint32_t level = dynamic_->level();
    if (dynamic_->Update(gpu_busy_ns, paced_ns) != level) {
      SetLevel(dynamic_->level());  // without a phase count, the jitter keeps the old size's
      resized = true;
    }
    if (dynamic_->out_of_reach())
      REXNATIVE_WARN_ONCE(
          "ReXNative upscaler: {:.1f} ms of GPU time a frame at the least height "
          "({} rows), over the budget of a frame shown for {:.1f} ms; a lower frame "
          "rate or frame generation gives each frame more",
          dynamic_->run_ns() * 1e-6, render_height_, double(paced_ns) * 1e-6);
  }
  WatchFsrCost(frame, gpu_busy_ns, paced_ns);
  const uint64_t now = NowNs();
  frame_ms_ = last_ns_ ? float(double(now - last_ns_) * 1e-6) : 0.0f;
  last_ns_ = now;
  DlssJitter(frame, double(render_width_) / double(OutputWidth()), jitter_[0], jitter_[1]);
  return resized;
}

// FSR 4 costs a fixed time a frame whatever the render size (5.1 ms at 1120x584 on the Steam
// Deck, Diablo III's town, against FSR 3.1's 3.1 ms and the scene's own 4.1: about a third of a
// 60 fps frame). Where the GPU is busy for most of each frame's time, the newest (auto) gives way
// to FSR 3.1 for the rest of the run: kFsrWindows windows of kFsrWindowFrames frames in a row
// with the GPU busy for at least --rexnative_fsr_auto_share (0.9) of the paced frame time, after the first
// kFsrSettleFrames (loading, the first pipelines). Never back: the next run starts at FSR 4 again,
// and a game the GPU has room for never leaves it. A game held by its CPU (the GPU waiting for
// it, which gpu_busy_ns leaves out) keeps FSR 4: 3.1 would not make it faster.
void Upscaler::WatchFsrCost(uint64_t frame, uint64_t gpu_busy_ns, uint64_t paced_ns) {
  if (backend_ != Backend::kFsr || !fsr4_ || fsr_downgrade_ || !REXCVAR_GET(rexnative_fsr_auto) ||
      REXCVAR_GET(rexnative_fsr_version) != "auto" || !gpu_busy_ns || !paced_ns ||
      frame < kFsrSettleFrames)
    return;
  busy_sum_ns_ += gpu_busy_ns;
  paced_sum_ns_ += paced_ns;
  if (++busy_frames_ < kFsrWindowFrames) return;
  const double share = double(busy_sum_ns_) / double(paced_sum_ns_);
  const double busy_ms = double(busy_sum_ns_) * 1e-6 / busy_frames_;
  const double paced_ms = double(paced_sum_ns_) * 1e-6 / busy_frames_;
  busy_sum_ns_ = paced_sum_ns_ = 0;
  busy_frames_ = 0;
  busy_windows_ = share >= REXCVAR_GET(rexnative_fsr_auto_share) ? busy_windows_ + 1 : 0;
  if (busy_windows_ < kFsrWindows) return;
  fsr_downgrade_ = fsr_downgrade_due_ = true;
  g_fsr3_this_run.store(true, std::memory_order_relaxed);
  REXLOG_INFO("ReXNative upscaler: FSR 4 leaves the GPU no room (busy {:.1f} ms of each {:.1f} ms "
              "frame): FSR 3.1 for the rest of this run (--rexnative_fsr_auto)",
              busy_ms, paced_ms);
}

void Upscaler::NoteSceneResolve(const ResolveParams& p) {
  resolve_clear_flags_ = p.clear_flags;
  std::memcpy(resolve_clear_color_, p.clear_color, sizeof(resolve_clear_color_));
  resolve_clear_depth_ = p.clear_depth;
  resolve_clear_stencil_ = p.clear_stencil;
}

bool Upscaler::CreateCopyPass() {
  if (copy_signature_) return true;
  const D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
  D3D12_ROOT_PARAMETER param = {};
  param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  param.DescriptorTable = {1, &range};
  param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  const D3D12_ROOT_SIGNATURE_DESC rs = {1, &param, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ComPtr<ID3DBlob> blob, err;
  if (Failed(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err),
             "upscaler copy root signature serialize") ||
      Failed(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                          IID_PPV_ARGS(&copy_signature_)),
             "upscaler copy root signature"))
    return false;
  const pD3DCompile compile = D3DCompiler();
  ComPtr<ID3DBlob> error;
  if (!compile ||
      FAILED(compile(kCopyVs, sizeof(kCopyVs) - 1, "rexnative_upscaler_copy_vs.hlsl", nullptr,
                     nullptr, "main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &copy_vs_,
                     &error)) ||
      FAILED(compile(kCopyPs, sizeof(kCopyPs) - 1, "rexnative_upscaler_copy_ps.hlsl", nullptr,
                     nullptr, "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &copy_ps_,
                     &error))) {
    REXLOG_ERROR("ReXNative upscaler: copy shaders: {}",
                 error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
    copy_signature_.Reset();
    return false;
  }
  return true;
}

bool Upscaler::EnterPostScene(CommandStream* list, RenderTargets& targets, const SurfaceRegs& regs) {
  post_scene_frame_ = frame_;
  targets.SuspendSceneOverride(true);
  REXNATIVE_LOG_FIRST(1, INFO,
                      "ReXNative upscaler: the game draws into the scene's surface after its "
                      "upscaled resolve (post-processing, HUD): at the output's size from there");
  bool recorded = false;
  const uint32_t height = SceneHeight();
  // Color: the clear the upscaled resolve made, or the upscaled scene itself.
  if (resolve_clear_flags_ & 0x100) {
    ClearParams cp;
    cp.flags = 1;
    std::memcpy(cp.color, resolve_clear_color_, sizeof(cp.color));
    targets.Clear(list, regs, 1, false, height, cp);
  } else if (RenderTarget* color = targets.GetColor(regs, 0, height);
             color && CreateCopyPass()) {
    // An MSAA surface (Marble Blast Ultra's 2x scene, in two tiles side by side): the copy
    // writes each pixel's samples alike. Left out, the draws after the resolve (the marble, the
    // HUD) went over a full-size target that never had the scene: the level was black.
    RenderTarget& source = *Result();
    ComPtr<ID3D12PipelineState>& pso = copy_pso_[uint32_t(color->view_format) << 4 | color->samples];
    if (!pso) {
      D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
      pd.pRootSignature = copy_signature_.Get();
      pd.VS = {copy_vs_->GetBufferPointer(), copy_vs_->GetBufferSize()};
      pd.PS = {copy_ps_->GetBufferPointer(), copy_ps_->GetBufferSize()};
      pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      pd.SampleMask = UINT_MAX;
      pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
      pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
      pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      pd.NumRenderTargets = 1;
      pd.RTVFormats[0] = color->view_format;
      pd.SampleDesc.Count = color->samples;
      Failed(device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)), "upscaler copy PSO");
    }
    if (pso) {
      targets.PrepareRead(list, color, {0, 0, LONG(SceneWidth()), LONG(height)});
      RenderTargets::Transition(list, color, D3D12_RESOURCE_STATE_RENDER_TARGET);
      if (source.state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
        const D3D12_RESOURCE_BARRIER b = TransitionBarrier(
            source.resource.Get(), source.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &b);
        source.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
      }
      const uint32_t slot = uint32_t(frame_ % D3D12Device::kFramesInFlight) * kViewsPerFrame + 2;
      D3D12_CPU_DESCRIPTOR_HANDLE cpu = input_heap_->GetCPUDescriptorHandleForHeapStart();
      cpu.ptr += size_t(slot) * input_stride_;
      D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
      sd.Format = source.srv_format;
      sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      sd.Texture2D.MipLevels = 1;
      device_->CreateShaderResourceView(source.resource.Get(), &sd, cpu);
      D3D12_GPU_DESCRIPTOR_HANDLE gpu = input_heap_->GetGPUDescriptorHandleForHeapStart();
      gpu.ptr += uint64_t(slot) * input_stride_;
      ID3D12DescriptorHeap* heaps[] = {input_heap_.Get()};
      list->SetDescriptorHeaps(1, heaps);
      list->SetGraphicsRootSignature(copy_signature_.Get());
      list->SetPipelineState(pso.Get());
      list->SetGraphicsRootDescriptorTable(0, gpu);
      const D3D12_VIEWPORT vp = {0, 0, float(OutputWidth()), float(OutputHeight()), 0, 1};
      const D3D12_RECT sc = {0, 0, LONG(OutputWidth()), LONG(OutputHeight())};
      list->RSSetViewports(1, &vp);
      list->RSSetScissorRects(1, &sc);
      list->OMSetRenderTargets(1, &color->view, FALSE, nullptr);
      list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      list->DrawInstanced(3, 1, 0, 0);
      targets.MarkListStateLost();
      recorded = true;
    }
  }
  // Depth: the upscaled resolve's clear; otherwise the scene's own depth, which the EDRAM keeps
  // after a colour resolve, copied into the full-size target (the scene drew into the
  // upscaler's). Asura's Wrath draws its hero's lighting after the resolve, in additive layers
  // that test the scene pass's depth without writing any: against the full-size target's stale
  // depth every layer passed, his far arm and his insides over the near ones, and he looked
  // see-through (the stock renderer kept the depth: its layers behind failed).
  if (resolve_clear_flags_ & 0x200) {
    ClearParams cp;
    cp.flags = 0x30;
    cp.depth = resolve_clear_depth_;
    cp.stencil = resolve_clear_stencil_;
    targets.Clear(list, regs, 0, true, height, cp);
  } else if (scene_depth_) {
    if (RenderTarget* depth = targets.GetDepth(regs, height);
        depth && targets.CopyDepth(list, scene_depth_, depth)) {
      REXNATIVE_LOG_FIRST(1, INFO,
                          "ReXNative upscaler: the scene's depth copied to the full-size target "
                          "for the draws after the upscaled resolve");
      targets.MarkListStateLost();
      recorded = true;
    }
  }
  return recorded;
}

D3D12_CPU_DESCRIPTOR_HANDLE Upscaler::MotionTarget(CommandStream* list) {
  const D3D12_CPU_DESCRIPTOR_HANDLE rtv = motion_rtv_->GetCPUDescriptorHandleForHeapStart();
  if (motion_state_ != D3D12_RESOURCE_STATE_RENDER_TARGET) {
    const D3D12_RESOURCE_BARRIER b =
        TransitionBarrier(motion_.Get(), motion_state_, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->ResourceBarrier(1, &b);
    motion_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
  }
  if (motion_frame_ != frame_) {
    motion_frame_ = frame_;
    const float zero[4] = {};
    list->ClearRenderTargetView(rtv, zero, 0, nullptr);
  }
  return rtv;
}

Upscaler::SceneInputs Upscaler::ReadSceneInputs(CommandStream* list) {
  SceneInputs in;
  if (!upscaled_this_frame() || !scene_depth_) return in;
  RenderTargets::Transition(list, scene_depth_, kInputState);
  if (motion_state_ != kInputState) {
    const D3D12_RESOURCE_BARRIER b = TransitionBarrier(motion_.Get(), motion_state_, kInputState);
    list->ResourceBarrier(1, &b);
    motion_state_ = kInputState;
  }
  in.depth = scene_depth_->resource.Get();
  in.motion = motion_.Get();
  in.width = render_width_;
  in.height = render_height_;
  in.jitter_x = jitter_[0];
  in.jitter_y = jitter_[1];
  in.reset = scene_reset_;
  return in;
}

RenderTarget* Upscaler::Upscale(CommandStream* list, RenderTargets& targets,
                                const SurfaceRegs& regs) {
  if (upscaled_frame_ == frame_) return upscaled_ ? Result() : nullptr;
  upscaled_frame_ = frame_;
  upscaled_ = false;
  encoded_ = false;

  RenderTarget* color = targets.GetColor(regs, 0, SceneHeight());
  RenderTarget* depth = targets.GetDepth(regs, SceneHeight());
  if (!color || !depth || color->samples != 1 || depth->samples != 1 || color->scale != scale() ||
      depth->scale != scale()) {
    REXNATIVE_WARN_ONCE("ReXNative upscaler: unexpected scene targets (surface {:08X} color {:08X})",
                   regs.surface_info, regs.color_info[0]);
    return nullptr;
  }
  // A resolve that reads the scene through another format's EDRAM bits (RenderTargets::
  // ConvertedFrom): the upscaler takes the scene the game drew. Fed the bits of Asura's Wrath's
  // 2_10_10_10_FLOAT scene read as 2_10_10_10 (a log-like encoding, not colors), FSR 4 on the
  // Steam Deck made its first seconds of gameplay a noise of rainbow speckles.
  const D3D12_RECT whole = {0, 0, LONG(SceneWidth()), LONG(SceneHeight())};
  targets.PrepareRead(list, color, whole);
  RenderTarget* const read = color;  // the target the resolve reads, in the format it reads
  if (RenderTarget* drawn = targets.ConvertedFrom(color);
      drawn && drawn->samples == 1 && drawn->scale == scale() &&
      IsHdrColorFormat(drawn->guest_format) && !IsHdrColorFormat(color->guest_format) &&
      PrepareEncodedOutput(*color)) {
    color = drawn;
    encoded_ = true;
    REXNATIVE_LOG_FIRST(1, INFO,
                        "ReXNative upscaler: the scene is resolved as format {} over the format "
                        "{} the game drew: upscaled in that one, then converted",
                        read->guest_format, drawn->guest_format);
  }
  // The game's scene: HDR (a float target) or 8-bit, and its depth reversed (cleared to 0) or not.
  const SceneKind kind{IsHdrColorFormat(color->guest_format), depth->clear_depth < 0.5f};
  if (backend_ == Backend::kFsr && (!context_ || kind != kind_)) {
    // FSR's context is made for it: by the renderer, with the GPU idle (UpdateContext). This
    // frame keeps the scene as it is.
    wanted_kind_ = kind;
    context_outdated_ = true;
    encoded_ = false;
    return nullptr;
  }
  Time(list, GpuTimer::kUpscaleInput);
  const D3D12_RECT all = {0, 0, LONG(SceneWidth()), LONG(SceneHeight())};
  targets.PrepareRead(list, depth, all);  // the color's was read above

  D3D12_RESOURCE_BARRIER bs[5];
  uint32_t nb = 0;
  auto to = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES* state, D3D12_RESOURCE_STATES after) {
    if (*state == after) return;
    bs[nb++] = TransitionBarrier(r, *state, after);
    *state = after;
  };
  MotionTarget(list);  // cleared if no draw wrote motion this frame
  to(color->resource.Get(), &color->state, kInputState);
  to(depth->resource.Get(), &depth->state, kInputState);
  to(motion_.Get(), &motion_state_, kInputState);
  to(input_.Get(), &input_state_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  to(output_.resource.Get(), &output_.state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (nb) list->ResourceBarrier(nb, bs);
  ClampInput(list, color->resource.Get(), color->srv_format, input_.Get(), 0);
  nb = 0;
  to(input_.Get(), &input_state_, kInputState);
  list->ResourceBarrier(nb, bs);

  // A cut seen in the motion matches (NoteMotionMatch): three quarters of at least 64 compared
  // draws had no draw of their own mesh near them in the previous frame (Jet Set Radio's
  // camera moves left up to a third refused, its clipped polygons).
  if (match_frame_ == frame_ && matches_compared_ >= 64 &&
      4 * matches_refused_ > 3 * matches_compared_ && !reset_) {
    reset_ = true;
    if (++motion_cuts_ <= 8)
      REXLOG_INFO("ReXNative upscaler: camera cut at frame {} ({} of {} motion matches refused)",
                  frame_, matches_refused_, matches_compared_);
  }
  Time(list, GpuTimer::kUpscaleDispatch);
  // The motion target holds UV-space motion (current -> previous), so its scale to render pixels
  // is the render size, for DLSS as for FSR's motionVectorScale.
  if (backend_ == Backend::kDlss) {
    Dlss::Setup setup;
    setup.render_width = render_width_;
    setup.render_height = render_height_;
    setup.output_width = OutputWidth();
    setup.output_height = OutputHeight();
    setup.mode = mode_;
    setup.hdr = kind.hdr;
    setup.depth_inverted = kind.depth_inverted;
    setup.preset = REXCVAR_GET(rexnative_dlss_preset);
    Dlss::Frame f;
    f.color = input_.Get();
    f.depth = depth->resource.Get();
    f.motion = motion_.Get();
    f.output = output_.resource.Get();
    f.jitter_x = jitter_[0];
    f.jitter_y = jitter_[1];
    f.motion_scale_x = float(render_width_);
    f.motion_scale_y = float(render_height_);
    f.frame_ms = frame_ms_ > 0.0f ? frame_ms_ : 16.7f;
    f.reset = reset_;
    // On the stream's thread, which records the list: a failure shows from the next frame on.
    list->Call([this, setup, f](CommandStream::List* l) {
      if (!dlss_.Evaluate(l, setup, f)) REXNATIVE_WARN_ONCE("ReXNative upscaler: DLSS evaluation failed");
    });
  } else {
    const bool reactive = GenerateReactive(list);
    ffxDispatchDescUpscale d = {};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    if (reactive)
      d.reactive =
          ffxApiGetResourceDX12(reactive_.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    d.color = ffxApiGetResourceDX12(input_.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.depth = ffxApiGetResourceDX12(depth->resource.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.motionVectors = ffxApiGetResourceDX12(motion_.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.output = ffxApiGetResourceDX12(output_.resource.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    d.jitterOffset = {jitter_[0], jitter_[1]};
    d.motionVectorScale = {float(render_width_), float(render_height_)};
    d.renderSize = {render_width_, render_height_};
    d.upscaleSize = {OutputWidth(), OutputHeight()};
    // FSR 4 draws its debug view after the sharpening pass and crashes without it (OptiScaler's
    // FFXFeatureDx12::EvaluateInternal): the debug view takes the least sharpening.
    d.enableSharpening = debug_;
    d.sharpness = debug_ ? 0.01f : 0.0f;
    if (debug_) d.flags = FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW;
    d.frameTimeDelta = frame_ms_ > 0.0f ? frame_ms_ : 16.7f;
    d.preExposure = 1.0f;
    d.reset = reset_;
    d.cameraNear = camera_->near_plane;
    d.cameraFar = camera_->far_plane;
    d.cameraFovAngleVertical = camera_->fov_y;
    d.viewSpaceToMetersFactor = 1.0f;
    list->Call([this, d](CommandStream::List* l) mutable {
      d.commandList = l;
      if (const ffxReturnCode_t rc = api_->dispatch(&context_, &d.header); rc != FFX_API_RETURN_OK)
        REXNATIVE_WARN_ONCE("ReXNative upscaler: ffxDispatch failed ({})", rc);
    });
  }
  // This port: DLSS 5 on the HDR scene DLAA just made (neural.h's scene stage), before the
  // game's exposure, bloom, motion blur and tone mapping read it.
  if (neural_ && neural_->SceneWanted(dlss_)) {
    Time(list, GpuTimer::kUpscaleNeural);
    NeuralPass::SceneInputs in;
    in.depth = depth->resource.Get();
    in.depth_srv_format = depth->srv_format;
    in.motion = motion_.Get();
    in.render_width = render_width_;
    in.render_height = render_height_;
    in.frame_ms = frame_ms_ > 0.0f ? frame_ms_ : 16.7f;
    in.reset = reset_;
    neural_->ProcessScene(list, frame_, dlss_, &output_, OutputWidth(), OutputHeight(), in);
  }
  if (frame_generation_) {
    Time(list, GpuTimer::kUpscaleFrameGeneration);
    const FfxApiResource fg_depth =
        ffxApiGetResourceDX12(depth->resource.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    const FfxApiResource fg_motion =
        ffxApiGetResourceDX12(motion_.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    frame_generation_->Prepare(frame_, list, fg_depth, fg_motion, {render_width_, render_height_},
                               {jitter_[0], jitter_[1]},
                               {float(render_width_), float(render_height_)}, *camera_, frame_ms_,
                               reset_, kind.depth_inverted);
  }
  scene_depth_ = depth;
  scene_reset_ = reset_;
  reset_ = false;
  upscaled_ = true;
  output_.guest_format = color->guest_format;
  // The upscaled scene as the bits the game's resolve reads.
  if (encoded_) {
    encoded_output_.guest_format = read->guest_format;
    if (!targets.ConvertColor(list, &output_, color->guest_format, &encoded_output_,
                              read->guest_format)) {
      REXNATIVE_WARN_ONCE("ReXNative upscaler: the upscaled scene could not be converted to "
                          "format {}", read->guest_format);
      encoded_ = false;
    }
  }
  return Result();
}

}  // namespace rexnative
