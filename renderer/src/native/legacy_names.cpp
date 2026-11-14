// ReXNative: the options' names before ReXNative renamed them (PGR4Native's pgr4_native_*,
// pgr4_d3d_*, pgr4_render_height; From Dust's fromdust_trace_camera). Each old name is
// registered as well and passes its value on to the new one, so configuration files and command
// lines written for the hand-made ports keep working; the first use of each logs its new name.
// The table is written by rexnative/names.py (legacy_table / legacy_cpp).
#include "legacy_names.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <mutex>
#include <set>
#include <string>
#include <string_view>

namespace rexnative {
namespace {

struct Legacy {
  const char* old_name;
  const char* new_name;
  rex::cvar::FlagType type;
};

constexpr Legacy kLegacy[] = {
    {"fromdust_trace_camera", "rexnative_trace_camera", rex::cvar::FlagType::Boolean},
    {"pgr4_d3d_control", "rexnative_d3d_control", rex::cvar::FlagType::String},
    {"pgr4_d3d_frame_csv", "rexnative_d3d_frame_csv", rex::cvar::FlagType::String},
    {"pgr4_d3d_skip", "rexnative_d3d_skip", rex::cvar::FlagType::String},
    {"pgr4_d3d_stats_frames", "rexnative_d3d_stats_frames", rex::cvar::FlagType::Int32},
    {"pgr4_d3d_tap", "rexnative_d3d_tap", rex::cvar::FlagType::Boolean},
    {"pgr4_d3d_tap_check", "rexnative_d3d_tap_check", rex::cvar::FlagType::Boolean},
    {"pgr4_d3d_trace_dir", "rexnative_d3d_trace_dir", rex::cvar::FlagType::String},
    {"pgr4_native", "rexnative_beside_xenos", rex::cvar::FlagType::Boolean},
    {"pgr4_native_anisotropy", "rexnative_anisotropy", rex::cvar::FlagType::Int32},
    {"pgr4_native_async_pipelines", "rexnative_async_pipelines", rex::cvar::FlagType::Boolean},
    {"pgr4_native_bool_specialize", "rexnative_bool_specialize", rex::cvar::FlagType::Boolean},
    {"pgr4_native_capture_burst", "rexnative_capture_burst", rex::cvar::FlagType::Int32},
    {"pgr4_native_capture_dir", "rexnative_capture_dir", rex::cvar::FlagType::String},
    {"pgr4_native_capture_frame", "rexnative_capture_frame", rex::cvar::FlagType::Int32},
    {"pgr4_native_capture_hold_ms", "rexnative_capture_hold_ms", rex::cvar::FlagType::Int32},
    {"pgr4_native_capture_xenos", "rexnative_capture_xenos", rex::cvar::FlagType::Boolean},
    {"pgr4_native_classic", "rexnative_classic", rex::cvar::FlagType::Boolean},
    {"pgr4_native_coherency_check", "rexnative_coherency_check", rex::cvar::FlagType::Boolean},
    {"pgr4_native_csv", "rexnative_csv", rex::cvar::FlagType::String},
    {"pgr4_native_debug_keys", "rexnative_debug_keys", rex::cvar::FlagType::Boolean},
    {"pgr4_native_display_aspect", "rexnative_display_aspect", rex::cvar::FlagType::Double},
    {"pgr4_native_dlss5", "rexnative_dlss5", rex::cvar::FlagType::Boolean},
    {"pgr4_native_dlss5_automask", "rexnative_dlss5_automask", rex::cvar::FlagType::Int32},
    {"pgr4_native_dlss5_colour", "rexnative_dlss5_colour", rex::cvar::FlagType::Double},
    {"pgr4_native_dlss5_compare", "rexnative_dlss5_compare", rex::cvar::FlagType::Boolean},
    {"pgr4_native_dlss5_intensity", "rexnative_dlss5_intensity", rex::cvar::FlagType::Double},
    {"pgr4_native_dlss5_log_white", "rexnative_dlss5_log_white", rex::cvar::FlagType::Boolean},
    {"pgr4_native_dlss5_max_ratio", "rexnative_dlss5_max_ratio", rex::cvar::FlagType::Double},
    {"pgr4_native_dlss5_menus", "rexnative_dlss5_menus", rex::cvar::FlagType::Boolean},
    {"pgr4_native_dlss5_meter_band", "rexnative_dlss5_meter_band", rex::cvar::FlagType::Double},
    {"pgr4_native_dlss5_meter_down", "rexnative_dlss5_meter_down", rex::cvar::FlagType::Double},
    {"pgr4_native_dlss5_meter_up", "rexnative_dlss5_meter_up", rex::cvar::FlagType::Double},
    {"pgr4_native_dlss5_passes", "rexnative_dlss5_passes", rex::cvar::FlagType::Int32},
    {"pgr4_native_dlss5_paused", "rexnative_dlss5_paused", rex::cvar::FlagType::Boolean},
    {"pgr4_native_dlss5_preset", "rexnative_dlss5_preset", rex::cvar::FlagType::Int32},
    {"pgr4_native_dlss5_scale", "rexnative_dlss5_scale", rex::cvar::FlagType::Double},
    {"pgr4_native_dlss5_skip_composites", "rexnative_dlss5_skip_composites", rex::cvar::FlagType::String},
    {"pgr4_native_dlss5_split", "rexnative_dlss5_split", rex::cvar::FlagType::Int32},
    {"pgr4_native_dlss5_stability", "rexnative_dlss5_stability", rex::cvar::FlagType::Double},
    {"pgr4_native_dlss5_stage", "rexnative_dlss5_stage", rex::cvar::FlagType::String},
    {"pgr4_native_dlss5_structure", "rexnative_dlss5_structure", rex::cvar::FlagType::Double},
    {"pgr4_native_dlss5_style", "rexnative_dlss5_style", rex::cvar::FlagType::Int32},
    {"pgr4_native_dlss5_tone", "rexnative_dlss5_tone", rex::cvar::FlagType::Double},
    {"pgr4_native_dlss5_transfer", "rexnative_dlss5_transfer", rex::cvar::FlagType::Double},
    {"pgr4_native_dlss5_view", "rexnative_dlss5_view", rex::cvar::FlagType::Int32},
    {"pgr4_native_dlss5_white", "rexnative_dlss5_white", rex::cvar::FlagType::Double},
    {"pgr4_native_dlss_mode", "rexnative_dlss_mode", rex::cvar::FlagType::String},
    {"pgr4_native_dlss_preset", "rexnative_dlss_preset", rex::cvar::FlagType::Int32},
    {"pgr4_native_draw_log_frames", "rexnative_draw_log_frames", rex::cvar::FlagType::Int32},
    {"pgr4_native_draw_log_from", "rexnative_draw_log_from", rex::cvar::FlagType::Int32},
    {"pgr4_native_dred", "rexnative_dred", rex::cvar::FlagType::Boolean},
    {"pgr4_native_dynamic_depth_bias", "rexnative_dynamic_depth_bias", rex::cvar::FlagType::Boolean},
    {"pgr4_native_frame_generation", "rexnative_frame_generation", rex::cvar::FlagType::Boolean},
    {"pgr4_native_frame_latency", "rexnative_frame_latency", rex::cvar::FlagType::Int32},
    {"pgr4_native_fsr", "rexnative_fsr", rex::cvar::FlagType::Boolean},
    {"pgr4_native_fsr_debug", "rexnative_fsr_debug", rex::cvar::FlagType::Boolean},
    {"pgr4_native_fsr_sharpness", "rexnative_fsr_sharpness", rex::cvar::FlagType::Double},
    {"pgr4_native_generic_pipelines", "rexnative_generic_pipelines", rex::cvar::FlagType::Boolean},
    {"pgr4_native_glitch_catch", "rexnative_glitch_catch", rex::cvar::FlagType::String},
    {"pgr4_native_gpu_serialize", "rexnative_gpu_serialize", rex::cvar::FlagType::Boolean},
    {"pgr4_native_gpu_timing", "rexnative_gpu_timing", rex::cvar::FlagType::Boolean},
    {"pgr4_native_gpu_timing_shaders", "rexnative_gpu_timing_shaders", rex::cvar::FlagType::Boolean},
    {"pgr4_native_hdr", "rexnative_hdr", rex::cvar::FlagType::Boolean},
    {"pgr4_native_hdr_paper_white", "rexnative_hdr_paper_white", rex::cvar::FlagType::Int32},
    {"pgr4_native_hdr_peak_nits", "rexnative_hdr_peak_nits", rex::cvar::FlagType::Int32},
    {"pgr4_native_height", "rexnative_height", rex::cvar::FlagType::Int32},
    {"pgr4_native_latency_margin_us", "rexnative_latency_margin_us", rex::cvar::FlagType::Int32},
    {"pgr4_native_low_latency", "rexnative_low_latency", rex::cvar::FlagType::Boolean},
    {"pgr4_native_luma_probe", "rexnative_luma_probe", rex::cvar::FlagType::Boolean},
    {"pgr4_native_mark_audit", "rexnative_mark_audit", rex::cvar::FlagType::Int32},
    {"pgr4_native_menu_fps", "rexnative_menu_fps", rex::cvar::FlagType::Int32},
    {"pgr4_native_msaa", "rexnative_msaa", rex::cvar::FlagType::Int32},
    {"pgr4_native_ngx_log", "rexnative_ngx_log", rex::cvar::FlagType::Int32},
    {"pgr4_native_occlusion", "rexnative_occlusion", rex::cvar::FlagType::Boolean},
    {"pgr4_native_occlusion_trace", "rexnative_occlusion_trace", rex::cvar::FlagType::String},
    {"pgr4_native_occlusion_window_ms", "rexnative_occlusion_window_ms", rex::cvar::FlagType::Int32},
    {"pgr4_native_pipeline_cache", "rexnative_pipeline_cache", rex::cvar::FlagType::Boolean},
    {"pgr4_native_pipeline_threads", "rexnative_pipeline_threads", rex::cvar::FlagType::Int32},
    {"pgr4_native_pipeline_wait", "rexnative_pipeline_wait", rex::cvar::FlagType::Boolean},
    {"pgr4_native_present_interval", "rexnative_present_interval", rex::cvar::FlagType::Int32},
    {"pgr4_native_primary", "rexnative", rex::cvar::FlagType::Boolean},
    {"pgr4_native_profile", "rexnative_profile", rex::cvar::FlagType::Boolean},
    {"pgr4_native_render_thread", "rexnative_render_thread", rex::cvar::FlagType::Boolean},
    {"pgr4_native_renderer_switch", "rexnative_renderer_switch", rex::cvar::FlagType::Boolean},
    {"pgr4_native_replace_draws", "rexnative_replace_draws", rex::cvar::FlagType::Boolean},
    {"pgr4_native_resolution_scale", "rexnative_resolution_scale", rex::cvar::FlagType::Int32},
    {"pgr4_native_resolve_dump", "rexnative_resolve_dump", rex::cvar::FlagType::String},
    {"pgr4_native_resolve_selftest", "rexnative_resolve_selftest", rex::cvar::FlagType::Boolean},
    {"pgr4_native_resolve_specialize", "rexnative_resolve_specialize", rex::cvar::FlagType::Boolean},
    {"pgr4_native_shader_dump_dir", "rexnative_shader_dump_dir", rex::cvar::FlagType::String},
    {"pgr4_native_submit_draws", "rexnative_submit_draws", rex::cvar::FlagType::Int32},
    {"pgr4_native_texture_capture_order", "rexnative_texture_capture_order", rex::cvar::FlagType::Boolean},
    {"pgr4_native_texture_memory_hard_mb", "rexnative_texture_memory_hard_mb", rex::cvar::FlagType::Int32},
    {"pgr4_native_texture_memory_soft_lifetime_s", "rexnative_texture_memory_soft_lifetime_s", rex::cvar::FlagType::Int32},
    {"pgr4_native_texture_memory_soft_mb", "rexnative_texture_memory_soft_mb", rex::cvar::FlagType::Int32},
    {"pgr4_native_texture_trace", "rexnative_texture_trace", rex::cvar::FlagType::String},
    {"pgr4_native_track_verify", "rexnative_track_verify", rex::cvar::FlagType::Boolean},
    {"pgr4_native_upscaler", "rexnative_upscaler", rex::cvar::FlagType::Boolean},
    {"pgr4_native_upscaler_height", "rexnative_upscaler_height", rex::cvar::FlagType::Int32},
    {"pgr4_native_upscaler_min_height", "rexnative_upscaler_min_height", rex::cvar::FlagType::Int32},
    {"pgr4_native_upscaler_target_fps", "rexnative_upscaler_target_fps", rex::cvar::FlagType::Int32},
    {"pgr4_native_upscaler_width", "rexnative_upscaler_width", rex::cvar::FlagType::Int32},
    {"pgr4_native_watch_trace", "rexnative_watch_trace", rex::cvar::FlagType::String},
    {"pgr4_native_width", "rexnative_width", rex::cvar::FlagType::Int32},
    {"pgr4_render_height", "rexnative_render_height", rex::cvar::FlagType::Int32},
};

// Old names are set while the command line and the configuration file are read, before the log
// is up: noted here and logged by LogOldNames (NativeRenderer::Initialize).
std::mutex g_mutex;
std::set<std::string_view> g_used;

void Announce(const Legacy& l) {
  std::lock_guard lock(g_mutex);
  g_used.insert(l.old_name);
}

struct Registrar {
  Registrar() {
    for (const Legacy& l : kLegacy) {
      rex::cvar::FlagEntry e;
      e.name = l.old_name;
      e.type = l.type;
      e.category = "ReXNative (old names)";
      e.description = std::string("Old name of --") + l.new_name;
      // As the command line sets it: above the configuration file, below the settings menu.
      e.setter = [&l](std::string_view v) {
        Announce(l);
        rex::cvar::SetFlagFromCommandLine(l.new_name, v);
        return true;
      };
      // Never "modified": the old name is not written back anywhere.
      e.getter = [] { return std::string(); };
      e.command_callback = [](std::string_view) {};
      e.default_value = "";
      rex::cvar::RegisterFlag(std::move(e));
    }
  }
};
const Registrar g_registrar;

}  // namespace

void LogOldNames() {
  std::lock_guard lock(g_mutex);
  for (const Legacy& l : kLegacy)
    if (g_used.count(l.old_name))
      REXLOG_WARN("ReXNative: {} is now {} (the old name still works; {} = {})", l.old_name,
                  l.new_name, l.new_name, rex::cvar::GetFlagByName(l.new_name));
  g_used.clear();
}

const char* OldNameOf(std::string_view new_name) {
  for (const Legacy& l : kLegacy)
    if (new_name == l.new_name) return l.old_name;
  return nullptr;
}

}  // namespace rexnative
