// The Linux build's DLSS (dlss.h): none. NVIDIA's NGX SDK is a Windows library, and the Steam Deck
// has no NVIDIA GPU; DLSS and DLSS 5 report themselves unavailable, the jitter and the modes'
// scales stay (the upscaler's other backends use them).
#include "native/dlss.h"

#include "native/log.h"

#include <rex/logging.h>

#include <algorithm>
#include <cmath>

namespace rexnative {

namespace {

float Halton(uint32_t index, uint32_t base) {
  float f = 1.0f, r = 0.0f;
  while (index > 0) {
    f /= float(base);
    r += f * float(index % base);
    index /= base;
  }
  return r;
}

}  // namespace

Dlss::Mode Dlss::ModeFromName(std::string_view name) {
  if (name == "quality") return Mode::kQuality;
  if (name == "balanced") return Mode::kBalanced;
  if (name == "performance") return Mode::kPerformance;
  if (name == "ultra" || name == "ultra_performance") return Mode::kUltraPerformance;
  return Mode::kDlaa;
}

const char* Dlss::Name(Mode mode) {
  switch (mode) {
    case Mode::kQuality: return "DLSS Quality";
    case Mode::kBalanced: return "DLSS Balanced";
    case Mode::kPerformance: return "DLSS Performance";
    case Mode::kUltraPerformance: return "DLSS Ultra Performance";
    default: return "DLAA";
  }
}

double Dlss::RenderScale(Mode mode) {
  switch (mode) {
    case Mode::kQuality: return 2.0 / 3.0;
    case Mode::kBalanced: return 0.58;
    case Mode::kPerformance: return 0.5;
    case Mode::kUltraPerformance: return 1.0 / 3.0;
    default: return 1.0;
  }
}

void DlssJitter(uint64_t index, double render_scale, float& x, float& y) {
  const double ratio = 1.0 / std::max(render_scale, 0.01);
  const uint32_t phases = std::clamp(uint32_t(std::ceil(8.0 * ratio * ratio)), 8u, 64u);
  const uint32_t i = uint32_t(index % phases) + 1;
  x = Halton(i, 2) - 0.5f;
  y = Halton(i, 3) - 0.5f;
}

Dlss::~Dlss() = default;

void Dlss::Retire(NVSDK_NGX_Handle*) {}

void Dlss::ReleaseRetired() {}

void Dlss::Release() {}

bool Dlss::Initialize(ID3D12Device* device) {
  if (!tried_) {
    tried_ = true;
    device_ = device;
    REXLOG_INFO("ReXNative: DLSS not available (no NGX on Linux)");
  }
  return false;
}

bool Dlss::EvaluateNeural(ID3D12GraphicsCommandList*, const NeuralFrame&, uint32_t) { return false; }

bool Dlss::Evaluate(ID3D12GraphicsCommandList*, const Setup&, const Frame&) { return false; }

}  // namespace rexnative
