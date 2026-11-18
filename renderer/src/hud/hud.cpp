// ReXNative's HUD, the settings menu (hud.h). The option table, the live/start-up split and the
// configuration file writer follow PGR4Native's games/pgr4/src/pgr4_settings.cpp; the page is an
// ImGui window (From Dust's port), so it needs nothing from the game's own menus.
#include "hud/hud.h"

#include "game_layer.h"
#include "native/dlss5_looks.h"
#include "native/legacy_names.h"
#include "native/overlay.h"
#include "native/renderer.h"

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include "host.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>


REXCVAR_DEFINE_BOOL(rexnative_hud_hint, true, "ReXNative",
                    "Show how to open the settings menu (Back + Start, F1; View + Menu on the "
                    "Steam Deck) for the first seconds");

namespace rexnative::hud {
namespace {

// The menu's window title: the game's name (game_layer.h), then a fixed ImGui id.
const char* const kTitle = [] {
  static const std::string title = std::string(rexnative::game::kGameName) + " settings##rexnative_hud";
  return title.c_str();
}();

using rexnative::NativeRenderer;

struct Choice {
  std::string label;
  std::string value;
};

// One cvar's value for a choice; `text` values are TOML strings in the configuration file.
struct Assignment {
  std::string_view cvar;
  std::string value;
  bool text = false;
};
using Assignments = std::vector<Assignment>;

struct Option {
  std::string_view page, id, label;
  std::vector<Choice> choices;
  bool restart;                                  // taken at start-up only
  std::string (*read)();                         // the value running now
  Assignments (*assign)(const std::string& v);   // its cvars
  void (*applied)() = nullptr;                   // after a live option's cvars were set
  const char* help = nullptr;
  bool (*shown)() = nullptr;                     // null: always; false hides the row
};

std::string Flag(std::string_view cvar) { return rex::cvar::GetFlagByName(cvar); }
int FlagInt(std::string_view cvar) { return std::atoi(Flag(cvar).c_str()); }

const std::vector<Option>& Options();
const Option& Find(std::string_view id) { return *std::ranges::find(Options(), id, &Option::id); }

// The choice whose value is nearest the number `n`.
std::string Nearest(const Option& o, double n) {
  const auto d = [n](const Choice& c) { return std::abs(std::atof(c.value.c_str()) - n); };
  return std::ranges::min(o.choices, {}, d).value;
}

Assignments Bool(std::string_view cvar, const std::string& v) { return {{cvar, v}}; }

// NVIDIA's options (DLSS, DLSS 5) only on an NVIDIA GPU, or while the GPU is not known yet: the
// Steam Deck's 800 lines then hold the rest without scrolling.
bool NvidiaGpu() {
  const uint32_t vendor = NativeRenderer::GpuVendor();
  return vendor == 0 || vendor == 0x10DE;
}

// The display refresh the native renderer paces the frames on (60 Hz: the Steam Deck's panel
// and most displays; the frame rate choices are its divisors).
int RefreshRate() { return 60; }

std::vector<Option> MakeOptions() {
  const std::vector<Choice> on_off = {{"Off", "false"}, {"On", "true"}};
  std::vector<Option> o;
  // Which renderer draws the game: live (the renderer switch, as F9, at the next Swap) and kept
  // for the next start (rexnative.classic: the Xenos plugin from the first frame). The value is
  // the choice, which F9 keeps in step; the automatic hand-over (rexnative.auto_handover) does
  // not change it. Under vkd3d-proton too, as the window keeps ReXNative's swap chain
  // (classic_frames.h).
  o.push_back({"Display", "renderer", "Renderer",
               {{"Native (ReXNative)", "native"}, {"Classic (Xenia)", "classic"}}, false,
               [] { return std::string(Flag("rexnative_classic") == "true" ? "classic" : "native"); },
               [](const std::string& v) {
                 return Bool("rexnative_classic", v == "classic" ? "true" : "false");
               },
               [] {
                 if (NativeRenderer* r = NativeRenderer::Get())
                   r->RequestRenderer(Flag("rexnative_classic") != "true");
               },
               "Native: ReXNative, with the settings below. Classic: Xenia's renderer, for a "
               "game the native one does not draw right yet. At once, and kept."});
  o.push_back({"Display", "antialiasing", "Antialiasing",
               {{"Off", "1"},
                {"MSAA 2x (console)", "2"},
                {"MSAA 4x", "4"},
                {"MSAA 8x", "8"},
                {"DLSS / FSR (Upscaler)", "upscaler"}},
               false,
               [] {
                 if (Flag("rexnative_upscaler") == "true") return std::string("upscaler");
                 const int msaa = FlagInt("rexnative_msaa");
                 return msaa == 1 || msaa == 4 || msaa == 8 ? std::to_string(msaa)
                                                            : std::string("2");
               },
               [](const std::string& v) {
                 if (v == "upscaler") return Assignments{{"rexnative_upscaler", "true"}};
                 return Assignments{{"rexnative_upscaler", "false"}, {"rexnative_msaa", v}};
               },
               [] {
                 if (NativeRenderer* r = NativeRenderer::Get()) r->RequestAntialiasing();
               },
               "The 3D scene's multisampling (most games draw 2x on the console), or the "
               "upscaler's temporal anti-aliasing (the Upscaler options below)."});
  o.push_back({"Display", "upscaling", "Screen scaling", {{"Bilinear", "false"}, {"AMD FSR 1", "true"}},
               false, [] { return Flag("rexnative_fsr"); },
               [](const std::string& v) { return Bool("rexnative_fsr", v); }, nullptr,
               "How the game's picture is scaled to the screen. FSR 1 keeps edges sharp; "
               "bilinear is softer."});
  o.push_back({"Display", "sharpening", "FSR sharpening",
               {{"Soft", "1"}, {"Medium", "0.5"}, {"Sharp", "0.2"}, {"Sharpest", "0"}}, false,
               [] { return Nearest(Find("sharpening"), std::atof(Flag("rexnative_fsr_sharpness").c_str())); },
               [](const std::string& v) { return Assignments{{"rexnative_fsr_sharpness", v}}; },
               nullptr, "How much FSR 1 sharpens the scaled picture (RCAS)."});
  o.push_back({"Display", "anisotropy", "Texture filtering",
               {{"Game's", "1"}, {"2x", "2"}, {"4x", "4"}, {"8x", "8"}, {"16x", "16"}}, true,
               [] { return Nearest(Find("anisotropy"), FlagInt("rexnative_anisotropy")); },
               [](const std::string& v) { return Assignments{{"rexnative_anisotropy", v}}; },
               nullptr, "Anisotropic filtering: sharper ground seen at grazing angles."});
  std::vector<Choice> frame_rates;
  for (int n = 1; n <= 2; ++n)
    frame_rates.push_back({std::to_string(RefreshRate() / n) + " fps", std::to_string(n)});
  o.push_back({"Display", "frame_rate", "Frame rate", std::move(frame_rates), false,
               [] { return Nearest(Find("frame_rate"), FlagInt("rexnative_present_interval")); },
               [](const std::string& v) {
                 return Assignments{{"rexnative_present_interval", v}};
               },
               [] {
                 if (NativeRenderer* r = NativeRenderer::Get()) r->RequestFrameRate();
               },
               "How often a frame is shown. Whether the game itself runs faster depends on "
               "the game."});
  o.push_back({"Display", "frame_latency", "Frame latency", {{"1", "1"}, {"2", "2"}, {"3", "3"}},
               false,
               [] { return Nearest(Find("frame_latency"), FlagInt("rexnative_frame_latency")); },
               [](const std::string& v) {
                 return Assignments{{"rexnative_frame_latency", v}};
               },
               nullptr,
               "Frames the game may run ahead of the screen: 1 responds soonest (the least input "
               "delay), 3 is the steadiest when the GPU is near its limit. Takes effect at once."});
  // The scene's upscaler (upscaler.h): NVIDIA DLSS (dlss.h) or AMD FSR 4 / 3.1, the mode, and
  // FSR frame generation (frame_generation.h). They work on the scene surface (game_layer.h's
  // kSceneSurface, or learned at run time: scene.h). The live ones make the upscaler again at
  // the next frame (RequestAntialiasing); the page's first line says what runs (UpscalerStatus).
  const auto remake = [] {
    if (NativeRenderer* r = NativeRenderer::Get()) r->RequestAntialiasing();
  };
  o.push_back({"Upscaler", "upscaler", "Upscaler", on_off, false,
               [] { return Flag("rexnative_upscaler"); },
               [](const std::string& v) { return Bool("rexnative_upscaler", v); }, remake,
               "Temporal anti-aliasing and upscaling of the 3D scene, in place of its MSAA."});
  o.push_back({"Upscaler", "upscaler_backend", "Upscaler type",
               {{"Automatic", "auto"}, {"NVIDIA DLSS", "dlss"}, {"AMD FSR", "fsr"}}, false,
               [] { return Flag("rexnative_upscaler_backend"); },
               [](const std::string& v) {
                 return Assignments{{"rexnative_upscaler_backend", v, true}};
               },
               remake,
               "Automatic: DLSS on an NVIDIA RTX GPU, FSR on the others (the Steam Deck, AMD, "
               "Intel). FSR runs on every GPU.",
               NvidiaGpu});
  o.push_back({"Upscaler", "fsr_version", "FSR version",
               {{"Newest", "auto"}, {"FSR 4", "4"}, {"FSR 3.1", "3"}}, false,
               [] { return Flag("rexnative_fsr_version"); },
               [](const std::string& v) { return Assignments{{"rexnative_fsr_version", v, true}}; },
               remake,
               "FSR 4 (AI, sharper) needs an AMD GPU and AMD's driver library (on the Steam Deck, "
               "GE-Proton fetches it); FSR 3.1 is lighter and runs everywhere."});
  o.push_back({"Upscaler", "upscaler_mode", "Mode",
               {{"Native AA (DLAA)", "native"},
                {"Quality", "quality"},
                {"Balanced", "balanced"},
                {"Performance", "performance"},
                {"Ultra performance", "ultra"}},
               false,
               [] {
                 // --rexnative_upscaler_mode, or the older --rexnative_dlss_mode it falls back on.
                 std::string m = Flag("rexnative_upscaler_mode");
                 if (m.empty()) m = Flag("rexnative_dlss_mode");
                 return m == "dlaa" || m.empty() ? std::string("native") : m;
               },
               [](const std::string& v) {
                 return Assignments{{"rexnative_upscaler_mode", v, true}};
               },
               remake,
               "Native AA keeps the game's resolution and only anti-aliases; the others render "
               "smaller (faster) and rebuild the picture."});
  o.push_back({"Upscaler", "frame_generation", "Frame generation (FSR)", on_off, true,
               [] { return Flag("rexnative_frame_generation"); },
               [](const std::string& v) { return Bool("rexnative_frame_generation", v); },
               nullptr,
               "AMD FSR frame generation: a generated frame before each of the game's, for twice "
               "the frames shown. Needs the upscaler; best from 30 fps up."});
  o.push_back({"DLSS 5", "dlss5", "DLSS 5 neural rendering", on_off, false,
               [] { return Flag("rexnative_dlss5"); },
               [](const std::string& v) { return Bool("rexnative_dlss5", v); }, nullptr,
               "NVIDIA DLSS 5 on the scene, with the upscaler on: an RTX GPU, its driver and "
               "nvngx_dlssnr.dll (beside the executable or in the common folder).",
               NvidiaGpu});
  // The looks (native/dlss5_looks.h): one set of DLSS 5's settings per kind of game, the game's
  // own pick by default (python -m rexnative project); live.
  std::vector<Choice> looks;
  for (const rexnative::Dlss5Look& l : rexnative::kDlss5Looks) looks.push_back({l.label, l.id});
  looks.push_back({"Custom", "custom"});
  o.push_back({"DLSS 5", "dlss5_look", "DLSS 5 look", std::move(looks), false,
               [] {
                 const std::string v = Flag("rexnative_dlss5_look");
                 return rexnative::FindDlss5Look(v) ? v : std::string("custom");
               },
               [](const std::string& v) {
                 return Assignments{{"rexnative_dlss5_look", v, true}};
               },
               nullptr,
               "Cinematic: realistic games. Cinematic, 2 passes: racing and realistic, heavier. "
               "Natural: nearer the game's colours. Stylized: cartoon or painterly 3D, its hues "
               "kept. Cel-shaded / 2D: gentle, flat colours kept. Strong: the most change.",
               NvidiaGpu});
  o.push_back({"DLSS 5", "dlss5_intensity", "DLSS 5 intensity",
               {{"Low", "0.5"}, {"Medium", "0.75"}, {"Full", "1"}, {"Strong", "1.25"}}, false,
               [] {
                 return Nearest(Find("dlss5_intensity"),
                                std::atof(Flag("rexnative_dlss5_intensity").c_str()));
               },
               [](const std::string& v) {
                 return Assignments{{"rexnative_dlss5_intensity", v}};
               },
               nullptr, "How strongly DLSS 5 changes the picture, on top of the look's own.",
               NvidiaGpu});
  o.push_back({"Developer", "occlusion", "Occlusion queries", on_off, true,
               [] { return Flag("rexnative_occlusion"); },
               [](const std::string& v) { return Bool("rexnative_occlusion", v); }, nullptr,
               "The game's visibility tests counted on the GPU. Off: everything drawn; on can "
               "hide objects for a frame in games that read results in the same frame."});
  o.push_back({"Developer", "log_level", "Log level",
               {{"Errors", "error"}, {"Warnings", "warn"}, {"Info", "info"}, {"Debug", "debug"}},
               false, [] { return Flag("log_level"); },
               [](const std::string& v) { return Assignments{{"log_level", v, true}}; },
               nullptr,
               "How much the game writes to its log (the logs folder beside the executable): Info "
               "normally, Debug for a bug report."});
  return o;
}

const std::vector<Option>& Options() {
  static const std::vector<Option> options = MakeOptions();
  return options;
}

std::mutex g_mutex;  // the configuration file
std::filesystem::path g_file;
// Start-up options chosen since the start, by id: their cvars keep the running values.
std::map<std::string_view, std::string> g_pending;

std::string Value(const Option& o) {
  if (const auto it = g_pending.find(o.id); it != g_pending.end()) return it->second;
  return o.read();
}

void Set(const Option& o, const std::string& v) {
  if (o.restart) {
    if (v == o.read())
      g_pending.erase(o.id);
    else
      g_pending[o.id] = v;
    return;
  }
  for (const Assignment& a : o.assign(v))
    if (!rex::cvar::SetFlagByName(a.cvar, a.value))
      REXLOG_ERROR("ReXNative HUD: cannot set {} to {}", a.cvar, a.value);
  if (o.applied) o.applied();
}

std::string_view Trim(std::string_view s) {
  const size_t first = s.find_first_not_of(" \t");
  if (first == std::string_view::npos) return {};
  return s.substr(first, s.find_last_not_of(" \t") - first + 1);
}

// The `# comment` ending a `name = value` line from `from` on, outside a string, with the blanks
// before it; empty without one.
std::string_view Comment(std::string_view line, size_t from) {
  char quote = 0;
  for (size_t i = from; i < line.size(); ++i) {
    const char c = line[i];
    if (quote) {
      if (c == quote && !(quote == '"' && line[i - 1] == '\\')) quote = 0;
    } else if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '#') {
      const size_t text = line.substr(0, i).find_last_not_of(" \t");
      return line.substr(text == std::string_view::npos ? 0 : text + 1);
    }
  }
  return {};
}

struct Entry {
  std::string_view name, value;
};
std::optional<Entry> Parse(std::string_view line) {
  line = Trim(line);
  const size_t eq = line.find('=');
  if (line.starts_with('#') || line.starts_with('[') || eq == std::string_view::npos) return {};
  const Entry e{Trim(line.substr(0, eq)),
                Trim(line.substr(eq + 1, line.size() - eq - 1 - Comment(line, eq + 1).size()))};
  if (e.name.empty() || e.value.empty()) return {};
  return e;
}

using Lines = std::vector<std::pair<std::string, std::string>>;

// Where a setting goes in the configuration file, as ReXGlue v0.11 writes it: its full name's
// table and key ("rexnative_occlusion" -> [rexnative] occlusion, "log_level" -> [log] level).
struct Place {
  std::string table, key;
};
Place PlaceOf(std::string_view cvar) {
  const std::string full = rexnative::settings::FullName(cvar);
  const size_t dot = full.rfind('.');
  if (dot == std::string::npos) return {"", full};
  return {full.substr(0, dot), full.substr(dot + 1)};
}

// The table a `[name]` header line opens (its comment and a quoted name allowed:
// `[log] # note`, `["log"]`), "[[name]]" for an array of tables (no setting's table, but still
// where the table before it ends), or nullopt for any other line.
std::optional<std::string> TableOf(std::string_view line) {
  line = Trim(line);
  line = Trim(line.substr(0, line.size() - Comment(line, 0).size()));
  if (!line.starts_with('[') || !line.ends_with(']')) return {};
  const bool array = line.starts_with("[[") && line.ends_with("]]") && line.size() >= 4;
  std::string name(Trim(array ? line.substr(2, line.size() - 4) : line.substr(1, line.size() - 2)));
  if (name.size() >= 2 && (name.front() == '"' || name.front() == '\'') && name.back() == name.front())
    name = name.substr(1, name.size() - 2);
  return array ? "[[" + name + "]]" : name;
}

// How many brackets a value leaves open (its strings and comment aside): a multi-line array
// (`key = [` up to its `]`) is one value, after which a line may go.
int OpenBrackets(std::string_view value) {
  value = value.substr(0, value.size() - Comment(value, 0).size());
  int depth = 0;
  char quote = 0;
  for (size_t i = 0; i < value.size(); ++i) {
    const char c = value[i];
    if (quote) {
      if (c == quote && !(quote == '"' && i && value[i - 1] == '\\')) quote = 0;
    } else if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '[') {
      ++depth;
    } else if (c == ']') {
      --depth;
    }
  }
  return depth;
}

// `set` written into the configuration file in place. Every line that sets a cvar takes its
// value, keeping its comment: its key in its table (how ReXGlue writes the file: the Deck's
// files have [rexnative] frame_latency and [log] level), and its old flat names before the
// first table (the command line's, which older menus wrote). Without one, a cvar whose value is
// not its default gets a line at the end of its table, or a new table under the menu's comment.
// Before, only the flat names were looked for: a value the file sets in a table won at the next
// start over the flat line the menu added (or that the menu left out, a default).
bool Write(const Lines& set) {
  std::lock_guard lock(g_mutex);
  if (g_file.empty()) return false;
  std::vector<std::string> lines;
  bool crlf = false;
  {
    std::ifstream f(g_file, std::ios::binary);
    for (std::string line; std::getline(f, line);) {
      if (line.ends_with('\r')) {
        line.pop_back();
        crlf = true;
      }
      lines.push_back(std::move(line));
    }
  }
  constexpr std::string_view kAdded = "# Set in the settings menu (F1, ReXNative).";
  for (const auto& [cvar, value] : set) {
    const Place place = PlaceOf(cvar);
    const std::string full = place.table.empty() ? place.key : place.table + "." + place.key;
    const char* old = rexnative::OldNameOf(cvar);
    // The lines that set it, and the last line of its table.
    std::vector<size_t> found;
    std::optional<size_t> table_end;
    std::string table;
    // Keys before the first table that are its table's dotted keys (`log.file = ...`): a [log]
    // table after them would not parse, so the setting goes beside them.
    bool dotted_root = false;
    int depth = 0;  // brackets a multi-line array left open
    for (size_t i = 0; i < lines.size(); ++i) {
      if (depth > 0) {  // a line of the array above: the value goes on
        depth += OpenBrackets(lines[i]);
        if (table == place.table && !place.table.empty()) table_end = i;
        continue;
      }
      if (std::optional<std::string> t = TableOf(lines[i])) {
        table = std::move(*t);
        if (table == place.table) table_end = i;
        continue;
      }
      const std::optional<Entry> e = Parse(lines[i]);
      if (!e) continue;
      depth = std::max(0, OpenBrackets(e->value));
      if (table.empty() && !place.table.empty() && e->name.starts_with(place.table + "."))
        dotted_root = true;
      if (table == place.table && !place.table.empty()) table_end = i;
      const bool flat = table.empty() &&
                        (e->name == cvar || e->name == full || (old && e->name == old));
      if (flat || (table == place.table && e->name == place.key)) found.push_back(i);
    }
    for (const size_t i : found) {
      std::string& line = lines[i];
      const size_t eq = line.find('=');
      line = std::string(Trim(std::string_view(line).substr(0, eq))) + " = " + value +
             std::string(Comment(line, eq + 1));
    }
    if (!found.empty()) continue;
    const rex::cvar::FlagEntry* flag = rex::cvar::GetFlagInfo(cvar);
    if (flag && ('"' + flag->default_value + '"' == value || flag->default_value == value))
      continue;
    if (place.table.empty() || (dotted_root && !table_end)) {
      // Before the first table: a key of no table, or a dotted key beside its table's others.
      size_t at = 0;
      while (at < lines.size() && !TableOf(lines[at])) ++at;
      lines.insert(lines.begin() + std::ptrdiff_t(at), (place.table.empty() ? place.key : full) +
                                                           " = " + value);
    } else if (table_end) {
      lines.insert(lines.begin() + std::ptrdiff_t(*table_end + 1), place.key + " = " + value);
    } else {
      if (!lines.empty() && !Trim(lines.back()).empty()) lines.emplace_back();
      lines.emplace_back(kAdded);
      lines.push_back("[" + place.table + "]");
      lines.push_back(place.key + " = " + value);
    }
  }
  std::string toml;
  for (const std::string& line : lines) toml += line + (crlf ? "\r\n" : "\n");
  const std::filesystem::path temp = std::filesystem::path(g_file) += ".new";
  if (std::ofstream f(temp, std::ios::binary | std::ios::trunc); !(f << toml)) {
    REXLOG_ERROR("ReXNative HUD: cannot write {}", temp.string());
    return false;
  }
  std::error_code ec;
  std::filesystem::rename(temp, g_file, ec);
  if (ec) {
    REXLOG_ERROR("ReXNative HUD: cannot write {}: {}", g_file.string(), ec.message());
    return false;
  }
  return true;
}

void Save(const Option& o) {
  Lines lines;
  for (const Assignment& a : o.assign(Value(o)))
    lines.emplace_back(a.cvar, a.text ? '"' + std::string(a.value) + '"' : a.value);
  Write(lines);
}


// ---------------------------------------------------------------------------------------------
// The menu. Dear ImGui keeps one current context in rexruntime.dll, which ReXGlue's UI thread
// switches to its own in its window event handlers, so the menu's context is only used on the UI
// thread too: the native renderer's Swap (guest thread) asks it for a frame (CallInUIThread) and
// draws the triangles of the last one over the game (native/overlay.h). The game keeps running
// behind the menu; while it is up the game's pad reads see a neutral pad.

using rexnative::OverlayDraw;
using rexnative::OverlayFont;
using rexnative::OverlayFrame;
using rexnative::OverlayVertex;

constexpr uint16_t kPadUp = 0x0001, kPadDown = 0x0002, kPadLeft = 0x0004, kPadRight = 0x0008,
                   kPadStart = 0x0010, kPadBack = 0x0020, kPadA = 0x1000, kPadB = 0x2000;
constexpr uint16_t kChord = kPadStart | kPadBack;
// A lone Back or Start is held back from the game this long, in case it begins the chord.
constexpr auto kChordWait = std::chrono::milliseconds(120);
// A lone Back or Start released within kChordWait is shown to the game this long afterwards.
constexpr auto kTapReplay = std::chrono::milliseconds(100);

std::atomic<bool> g_open{false};
bool g_ready = false;  // Initialize ran (the app's UI is up)
ImGuiContext* g_context = nullptr;  // UI thread only

// The back buffer the frames are built for (guest thread -> UI thread).
std::atomic<uint32_t> g_display_width{0}, g_display_height{0};
std::atomic<bool> g_frame_queued{false};
std::mutex g_frame_mutex;
OverlayFrame g_frame;  // the last frame built, g_frame_mutex held
bool g_frame_ready = false;

std::mutex g_font_mutex;
OverlayFont g_font;

// The pads' buttons and pad 0's left stick, raw (guest thread -> UI thread).
std::atomic<uint32_t> g_pad_buttons[4];
// The game's pad reads reach FilterPad (the layer hooks XInputGetState): the chord works.
std::atomic<bool> g_pad_seen{false};
std::atomic<int32_t> g_pad_lx{0}, g_pad_ly{0};

// Makes the menu's context current for the scope (UI thread).
class Scope {
 public:
  Scope() : previous_(ImGui::GetCurrentContext()) { ImGui::SetCurrentContext(g_context); }
  ~Scope() { ImGui::SetCurrentContext(previous_); }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  ImGuiContext* previous_;
};

void Toggle() {
  const bool open = !g_open.load(std::memory_order_relaxed);
  g_open.store(open, std::memory_order_relaxed);
  REXLOG_INFO("ReXNative HUD {}", open ? "opened" : "closed");
}

// The first of the fonts found, at a size the Deck's 7" screen reads from arm's length.
void AddFont(ImGuiIO& io, float size) {
  std::filesystem::path fonts = "C:\\Windows\\Fonts";
  if (const char* windir = std::getenv("WINDIR")) fonts = std::filesystem::path(windir) / "Fonts";
  ImFontConfig config;
  config.OversampleH = 2;
  config.OversampleV = 1;
  static const ImWchar ranges[] = {0x0020, 0x00FF, 0};
  // segoeui: Windows; arial (Liberation Sans) and tahoma: Proton's and Wine's.
  for (const char* name : {"segoeui.ttf", "arial.ttf", "tahoma.ttf"}) {
    const std::filesystem::path path = fonts / name;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) continue;
    if (io.Fonts->AddFontFromFileTTF(path.string().c_str(), size, &config, ranges)) {
      REXLOG_INFO("ReXNative HUD font: {}", path.string());
      return;
    }
  }
  config.SizePixels = size;
  io.Fonts->AddFontDefault(&config);
}

// The style at 26 px text (the Deck's 800 lines); BuildFont scales it with the font.
ImGuiStyle g_base_style;
float g_font_size = 0.0f;  // the atlas's, UI thread
constexpr float kBaseFont = 26.0f;

// The atlas and the style at `size` px text, the font handed to the overlay.
void BuildFont(float size) {
  ImGuiIO& io = ImGui::GetIO();
  io.Fonts->Clear();
  AddFont(io, size);
  unsigned char* pixels = nullptr;
  int width = 0, height = 0;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
  io.Fonts->SetTexID(ImTextureID(1));
  {
    std::lock_guard lock(g_font_mutex);
    g_font.rgba.assign(pixels, pixels + size_t(width) * size_t(height) * 4);
    g_font.width = uint32_t(width);
    g_font.height = uint32_t(height);
    ++g_font.version;
  }
  ImGuiStyle& style = ImGui::GetStyle();
  style = g_base_style;
  style.ScaleAllSizes(1.6f * size / kBaseFont);
  style.FontSizeBase = size;
  g_font_size = size;
}

void CreateContext() {
  g_context = ImGui::CreateContext();  // keeps ReXGlue's context current
  Scope scope;
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  io.BackendPlatformName = "rexnative_hud";
  io.BackendRendererName = "rexnative_overlay";
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
  io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
  ImGuiStyle& style = g_base_style;
  ImGui::StyleColorsDark(&style);
  style.WindowRounding = 6.0f;
  style.FrameRounding = 4.0f;
  style.WindowBorderSize = 0.0f;
  style.WindowTitleAlign = ImVec2(0.5f, 0.5f);
  ImVec4* c = style.Colors;
  c[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.06f, 0.05f, 0.90f);
  c[ImGuiCol_TitleBgActive] = ImVec4(0.45f, 0.28f, 0.10f, 1.0f);
  c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive];
  c[ImGuiCol_Header] = ImVec4(0.55f, 0.35f, 0.12f, 0.45f);
  c[ImGuiCol_HeaderHovered] = ImVec4(0.65f, 0.42f, 0.15f, 0.70f);
  c[ImGuiCol_HeaderActive] = ImVec4(0.75f, 0.50f, 0.18f, 0.85f);
  c[ImGuiCol_NavCursor] = ImVec4(1.0f, 0.78f, 0.35f, 1.0f);
  c[ImGuiCol_Button] = c[ImGuiCol_Header];
  c[ImGuiCol_ButtonHovered] = c[ImGuiCol_HeaderHovered];
  c[ImGuiCol_ButtonActive] = c[ImGuiCol_HeaderActive];
  c[ImGuiCol_Separator] = ImVec4(0.55f, 0.40f, 0.20f, 0.6f);
  BuildFont(kBaseFont);
}

// Left and Right (the D-pad, the left stick, the arrow keys) since the last frame: the steps of
// the focused row's value (Row), which they never reach as ImGui keys. ImGui's navigation takes
// them first, as moves: in the menu's one column ImGui 1.92 moved the focus to the row below, and
// IsKeyPressed then saw nothing. UI thread.
int g_step = 0;

// The pads' buttons since the last frame as ImGui gamepad keys (Left and Right as g_step);
// `reset`: the ones held now (the chord that opened the menu) are not presses.
void FeedPad(ImGuiIO& io, bool reset) {
  static uint32_t fed = 0;
  static int stick = 0;  // the left stick's side past half its travel: -1, 0 or 1
  uint32_t buttons = 0;
  for (const auto& b : g_pad_buttons) buttons |= b.load(std::memory_order_relaxed);
  if (reset) fed = buttons;
  static const std::pair<uint32_t, ImGuiKey> kKeys[] = {
      {kPadUp, ImGuiKey_GamepadDpadUp},   {kPadDown, ImGuiKey_GamepadDpadDown},
      {kPadStart, ImGuiKey_GamepadStart}, {kPadBack, ImGuiKey_GamepadBack},
      {0x0100, ImGuiKey_GamepadL1},       {0x0200, ImGuiKey_GamepadR1},
      {kPadA, ImGuiKey_GamepadFaceDown},  {kPadB, ImGuiKey_GamepadFaceRight},
      {0x4000, ImGuiKey_GamepadFaceLeft}, {0x8000, ImGuiKey_GamepadFaceUp}};
  for (const auto& [bit, key] : kKeys)
    if ((buttons ^ fed) & bit) io.AddKeyEvent(key, (buttons & bit) != 0);
  const uint32_t pressed = buttons & ~fed;
  if (pressed & kPadLeft) --g_step;
  if (pressed & kPadRight) ++g_step;
  fed = buttons;
  // The left stick as a D-pad, past XInput's dead zone (7849 of 32767).
  const auto travel = [](int32_t v) {
    return std::clamp((std::abs(float(v)) - 7849.0f) / (32767.0f - 7849.0f), 0.0f, 1.0f);
  };
  const int32_t lx = g_pad_lx.load(), ly = g_pad_ly.load();
  const int side = travel(lx) > 0.5f ? (lx < 0 ? -1 : 1) : 0;
  if (side && side != stick && !reset) g_step += side;
  stick = side;
  const float f = travel(ly);
  io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, ly < 0 && f > 0.5f, ly < 0 ? f : 0.0f);
  io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, ly > 0 && f > 0.5f, ly > 0 ? f : 0.0f);
}

bool Pressed(std::initializer_list<ImGuiKey> keys) {
  for (ImGuiKey k : keys)
    if (ImGui::IsKeyPressed(k, true)) return true;
  return false;
}

// One option as a row: its label, and its value between arrows. Left and Right (D-pad, stick,
// keys) step the value; A, Enter or a click (a tap) step it forward, a right click back.
void Row(const Option& o, const Option** focused) {
  const std::string value = Value(o), running = o.read();
  size_t shown = 0;
  for (size_t i = 0; i < o.choices.size(); ++i)
    if (o.choices[i].value == value) shown = i;
  std::string text = o.choices[shown].label;
  if (o.restart && o.choices[shown].value != running) text += " *";
  ImGui::PushID(o.id.data(), o.id.data() + o.id.size());
  const float height = ImGui::GetFrameHeight();
  const bool clicked = ImGui::Selectable("##row", false, 0, ImVec2(0.0f, height));
  const bool is_focused = ImGui::IsItemFocused();
  const bool hovered = ImGui::IsItemHovered();
  const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const float pad = ImGui::GetStyle().FramePadding.x;
  const float y = min.y + (height - ImGui::GetFontSize()) * 0.5f;
  draw->AddText(ImVec2(min.x + pad, y), ImGui::GetColorU32(ImGuiCol_Text),
                std::string(o.label).c_str());
  const std::string arrows = "<   " + text + "   >";
  const float w = ImGui::CalcTextSize(arrows.c_str()).x;
  draw->AddText(ImVec2(max.x - pad - w, y),
                is_focused || hovered ? IM_COL32(255, 210, 120, 255) : IM_COL32(230, 220, 200, 255),
                arrows.c_str());
  int step = 0;
  if (is_focused && g_step) {
    step = g_step < 0 ? -1 : 1;
    g_step = 0;
  }
  if (clicked) step = 1;
  if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) step = -1;
  if (step) {
    const size_t n = o.choices.size();
    const size_t next = (shown + n + (step > 0 ? 1 : n - 1)) % n;
    Set(o, o.choices[next].value);
    Save(o);
  }
  if (is_focused || (hovered && !*focused)) *focused = &o;
  ImGui::PopID();
}

// The menu's window; false when it was closed.
bool DrawMenu(ImGuiIO& io, bool appearing) {
  const float width = std::min(760.0f * g_font_size / kBaseFont, io.DisplaySize.x - 40.0f);
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
  if (appearing) ImGui::SetNextWindowFocus();
  bool open = true;
  ImGui::Begin(kTitle, &open,
               ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoSavedSettings);
  const Option* focused = nullptr;
  std::string_view page;
  bool first = true;
  // The rows scroll in a region that leaves the help and the footer on the screen: the Steam
  // Deck's 800 lines (720 on a TV) hold fewer rows than the menu has. The region's height: the
  // screen less the window's title, padding and a margin, less what follows the rows (two lines
  // of help, the footer's button, the separators and spacing between them).
  const ImGuiStyle& style = ImGui::GetStyle();
  const float below = style.ItemSpacing.y * 6.0f + 2.0f +
                      ImGui::GetTextLineHeightWithSpacing() * 2.0f + ImGui::GetFrameHeight();
  const float rows_max = io.DisplaySize.y - style.WindowPadding.y * 4.0f -
                         ImGui::GetFrameHeight() - below;
  ImGui::SetNextWindowSizeConstraints(
      ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, std::max(rows_max, ImGui::GetFrameHeight() * 3.0f)));
  ImGui::BeginChild("##rows", ImVec2(0.0f, 0.0f),
                    ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_NavFlattened);
  for (const Option& o : Options()) {
    if (o.shown && !o.shown()) continue;
    if (o.page != page) {
      page = o.page;
      ImGui::SeparatorText(std::string(page).c_str());
      if (page == "Upscaler") {
        // What runs now: the upscaler and FSR version the GPU took, its mode and sizes.
        const std::string status = NativeRenderer::UpscalerStatus();
        ImGui::TextDisabled("Now: %s",
                            !status.empty() ? status.c_str()
                            : Flag("rexnative_upscaler") == "true"
                                ? "waiting for the game's 3D scene, or unavailable (see the log)"
                                : "off");
      }
    }
    if (first && appearing) ImGui::SetKeyboardFocusHere();
    first = false;
    Row(o, &focused);
  }
  g_step = 0;  // no row was focused to take it
  ImGui::EndChild();
  ImGui::Spacing();
  ImGui::Separator();
  // The focused option's help (the pad has no hover tooltips), in a space kept for two lines so
  // the window does not change size as the focus moves.
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.80f, 0.70f, 1.0f));
  ImGui::PushTextWrapPos(0.0f);
  const float top = ImGui::GetCursorPosY();
  ImGui::TextUnformatted(focused && focused->help ? focused->help : "");
  const float lines = top + ImGui::GetTextLineHeightWithSpacing() * 2.0f;
  if (ImGui::GetCursorPosY() < lines) ImGui::SetCursorPosY(lines);
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
  ImGui::Separator();
  ImGui::TextDisabled("%s", g_pending.empty() ? "D-pad: choose and change     B: close"
                                              : "* after a restart     B: close");
  ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize("Close").x -
                  ImGui::GetStyle().FramePadding.x * 2.0f);
  if (ImGui::Button("Close")) open = false;
  ImGui::End();
  // B or Escape closes (the rows open no popups for them to close first).
  if (Pressed({ImGuiKey_GamepadFaceRight, ImGuiKey_Escape})) open = false;
  return open;
}

// The start-up hint (--rexnative_hud_hint): from the first frame presented, for kHintTime,
// fading out over its last second. g_hint_end: steady_clock ticks, 0 before the first frame.
constexpr auto kHintTime = std::chrono::seconds(15);
std::atomic<int64_t> g_hint_end{0};

float HintAlpha(std::chrono::steady_clock::time_point now) {
  const int64_t end = g_hint_end.load(std::memory_order_relaxed);
  if (!end) return 0.0f;
  const std::chrono::steady_clock::time_point at{std::chrono::steady_clock::duration(end)};
  return std::clamp(std::chrono::duration<float>(at - now).count(), 0.0f, 1.0f);
}

// On the Steam Deck (Steam sets SteamDeck=1 for its games) the pad's Back and Start are View and
// Menu and there is no F1; without the pad hook only F1 opens the menu.
bool SteamDeck() {
  static const bool deck = [] {
    const char* v = std::getenv("SteamDeck");
    return v && std::strcmp(v, "1") == 0;
  }();
  return deck;
}

const char* HintText() {
  if (!g_pad_seen.load(std::memory_order_relaxed))
    return SteamDeck() ? "Settings: keyboard F1" : "Settings:  F1";
  return SteamDeck() ? "Settings:  View + Menu" : "Settings:  Back + Start   (F1)";
}

void DrawHint(ImGuiIO& io, float alpha) {
  const float margin = ImGui::GetStyle().WindowPadding.x;
  ImGui::SetNextWindowPos(ImVec2(margin, io.DisplaySize.y - margin), ImGuiCond_Always,
                          ImVec2(0.0f, 1.0f));
  ImGui::SetNextWindowBgAlpha(0.65f);
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
  ImGui::Begin("##rexnative_hint", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                   ImGuiWindowFlags_NoFocusOnAppearing);
  ImGui::TextUnformatted(HintText());
  ImGui::End();
  ImGui::PopStyleVar();
}

void BuildFrame() {
  g_frame_queued.store(false, std::memory_order_relaxed);
  static bool was_open = false;
  const auto now = std::chrono::steady_clock::now();
  const bool menu = g_open.load(std::memory_order_relaxed);
  const float hint = menu ? 0.0f : HintAlpha(now);
  if (!g_context || (!menu && hint <= 0.0f)) {
    was_open = false;
    return;
  }
  Scope scope;
  ImGuiIO& io = ImGui::GetIO();
  static auto last = std::chrono::steady_clock::now();
  io.DisplaySize = ImVec2(float(g_display_width.load()), float(g_display_height.load()));
  if (io.DisplaySize.x <= 0 || io.DisplaySize.y <= 0) return;
  // Text 26 px on the Deck's 1280x800, as tall for its height elsewhere (47 at 1440p).
  const float font = std::clamp(std::round(io.DisplaySize.y * (kBaseFont / 800.0f)), 16.0f, 72.0f);
  if (font != g_font_size) BuildFont(font);
  const bool appearing = menu && !was_open;
  was_open = menu;
  if (appearing) io.ClearInputKeys();
  io.DeltaTime = std::clamp(std::chrono::duration<float>(now - last).count(), 1e-4f, 0.25f);
  last = now;
  if (menu) FeedPad(io, appearing);
  ImGui::NewFrame();
  bool open = true;
  if (menu)
    open = DrawMenu(io, appearing);
  else
    DrawHint(io, hint);
  ImGui::Render();
  OverlayFrame frame;
  const ImDrawData* data = ImGui::GetDrawData();
  for (const ImDrawList* list : data->CmdLists) {
    const uint32_t first_vertex = uint32_t(frame.vertices.size());
    const uint32_t first_index = uint32_t(frame.indices.size());
    for (const ImDrawVert& v : list->VtxBuffer)
      frame.vertices.push_back({v.pos.x, v.pos.y, v.uv.x, v.uv.y, v.col});
    for (const ImDrawIdx i : list->IdxBuffer) frame.indices.push_back(uint16_t(i));
    for (const ImDrawCmd& cmd : list->CmdBuffer) {
      if (cmd.UserCallback || !cmd.ElemCount) continue;
      frame.draws.push_back({{int32_t(cmd.ClipRect.x), int32_t(cmd.ClipRect.y),
                              int32_t(std::ceil(cmd.ClipRect.z)),
                              int32_t(std::ceil(cmd.ClipRect.w))},
                             cmd.ElemCount,
                             first_index + cmd.IdxOffset,
                             first_vertex + cmd.VtxOffset});
    }
  }
  {
    std::lock_guard lock(g_frame_mutex);
    g_frame = std::move(frame);
    g_frame_ready = true;
  }
  if (menu && !open) {
    was_open = false;
    g_open.store(false, std::memory_order_relaxed);
    REXLOG_INFO("ReXNative HUD closed");
  }
}

// The window's mouse, touch and keys while the menu is up (UI thread), ahead of ReXGlue's
// listeners (host.h). F1 goes on to its key binding, which closes the menu.
bool Usable() { return g_context && g_open.load(std::memory_order_relaxed); }
// Window pixels to back buffer pixels (the swapchain can differ from the window).
float ScaleX(float x) {
  uint32_t w = 0, h = 0;
  rexnative::host::WindowSize(&w, &h);
  return w ? x * float(g_display_width.load()) / float(w) : x;
}
float ScaleY(float y) {
  uint32_t w = 0, h = 0;
  rexnative::host::WindowSize(&w, &h);
  return h ? y * float(g_display_height.load()) / float(h) : y;
}
bool Key(const rex::InputEvent& e, bool down) {
  using rex::VirtualKey;
  if (!Usable() || e.key == VirtualKey::kF1) return false;
  ImGuiKey key = ImGuiKey_None;
  switch (e.key) {
    // The keypad's arrows too (also what SendKeys' arrows arrive as: no extended-key flag).
    case VirtualKey::kUp:
    case VirtualKey::kNumpad8: key = ImGuiKey_UpArrow; break;
    case VirtualKey::kDown:
    case VirtualKey::kNumpad2: key = ImGuiKey_DownArrow; break;
    case VirtualKey::kLeft:
    case VirtualKey::kNumpad4:
      if (down) --g_step;  // the value's step, not ImGui's navigation (g_step)
      return true;
    case VirtualKey::kRight:
    case VirtualKey::kNumpad6:
      if (down) ++g_step;
      return true;
    case VirtualKey::kReturn: key = ImGuiKey_Enter; break;
    case VirtualKey::kSpace: key = ImGuiKey_Space; break;
    case VirtualKey::kEscape: key = ImGuiKey_Escape; break;
    case VirtualKey::kTab: key = ImGuiKey_Tab; break;
    default: break;
  }
  if (key != ImGuiKey_None) {
    Scope scope;
    ImGui::GetIO().AddKeyEvent(key, down);
  }
  return true;
}
// down: 1 pressed, 0 released, -1 a move.
bool Mouse(const rex::InputEvent& e, int down) {
  if (!Usable()) return false;
  Scope scope;
  ImGuiIO& io = ImGui::GetIO();
  io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
  io.AddMousePosEvent(ScaleX(e.x), ScaleY(e.y));
  if (down >= 0) {
    int button = -1;
    if (e.button == rex::MouseButton::kLeft) button = 0;
    if (e.button == rex::MouseButton::kRight) button = 1;
    if (button >= 0) io.AddMouseButtonEvent(button, down != 0);
  }
  return true;
}
bool OnInput(rex::InputEvent& e) {
  using Type = rex::InputEvent::Type;
  switch (e.type) {
    case Type::kKeyDown: return Key(e, true);
    case Type::kKeyUp: return Key(e, false);
    case Type::kKeyChar: return Usable();
    case Type::kButtonDown: return Mouse(e, 1);
    case Type::kButtonUp: return Mouse(e, 0);
    case Type::kMotion: return Mouse(e, -1);
    case Type::kWheel: {
      if (!Usable()) return false;
      Scope scope;
      ImGui::GetIO().AddMouseWheelEvent(0.0f, e.scroll_y);
      return true;
    }
    default: return false;
  }
}
constexpr int kInputOrder = 128;  // ahead of the game's own handlers

// Per pad: the Back+Start chord and what the game is kept from seeing.
struct PadState {
  bool latched = false;  // the chord toggled the menu: Back and Start hidden until released
  uint16_t lone = 0;     // a lone Back or Start, hidden since lone_at in case it begins the chord
  std::chrono::steady_clock::time_point lone_at{}, replay_until{};
  uint16_t replay = 0;  // a lone tap released while hidden, shown to the game until replay_until
  uint16_t hidden = 0;  // buttons held when the menu closed, hidden until released
  bool was_open = false;
};

}  // namespace

void SetConfigPath(const std::filesystem::path& config) {
  std::lock_guard lock(g_mutex);
  g_file = config;
}

void Initialize() {
  if (g_file.empty()) {
    // The app's configuration file (app.config_file), unless the game's app set another. Its
    // value resolved: the text is "{app.root}/{app.name}.toml" on ReXGlue v0.11, which no file
    // is (every setting the menu changed, log level and occlusion queries among them, was lost:
    // "cannot write {app.root}/{app.name}.toml.new").
    const std::filesystem::path config = rexnative::settings::Path("app.config_file");
    if (!config.empty()) SetConfigPath(config);
  }
  g_ready = true;
  rexnative::host::RunOnUIThread([] { CreateContext(); });
  rexnative::host::AddInputHandler(&g_ready, kInputOrder, OnInput);
  rexnative::host::RegisterBind("bind_rexnative_hud", "F1",
                                "ReXNative: the settings menu (pad: Back+Start)",
                                [] { Toggle(); });
}

void Shutdown() {
  rexnative::host::UnregisterBind("bind_rexnative_hud");
  g_open.store(false);
  rexnative::host::RemoveInputHandler(&g_ready);
  g_ready = false;
  if (g_context) {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGui::DestroyContext(g_context);
    ImGui::SetCurrentContext(previous == g_context ? nullptr : previous);
    g_context = nullptr;
  }
}

bool MenuOpen() { return g_open.load(std::memory_order_relaxed); }

bool FilterPad(uint32_t user, uint8_t* pad) {
  static PadState states[4];
  static std::mutex lock;
  if (user >= 4) return false;
  std::lock_guard guard(lock);
  PadState& s = states[user];
  const auto now = std::chrono::steady_clock::now();
  const uint16_t buttons = uint16_t(pad[0] << 8 | pad[1]);
  g_pad_buttons[user].store(buttons, std::memory_order_relaxed);
  if (!g_pad_seen.exchange(true, std::memory_order_relaxed))
    REXLOG_INFO("ReXNative HUD: the game's pad reads pass here (Back+Start opens the menu)");
  if (user == 0) {
    g_pad_lx.store(int16_t(pad[4] << 8 | pad[5]), std::memory_order_relaxed);
    g_pad_ly.store(int16_t(pad[6] << 8 | pad[7]), std::memory_order_relaxed);
  }
  uint16_t out = buttons;
  const uint16_t chord = buttons & kChord;
  if (s.latched) {
    if (!chord) s.latched = false;
    out &= uint16_t(~kChord);
  } else if (chord == kChord) {
    s.latched = true;
    s.lone = 0;
    s.replay = 0;
    out &= uint16_t(~kChord);
    if (g_context) Toggle();
  } else if (chord) {
    if (s.lone != chord) {
      s.lone = chord;
      s.lone_at = now;
    }
    if (now - s.lone_at < kChordWait) out &= uint16_t(~kChord);
  } else {
    if (s.lone && now - s.lone_at < kChordWait) {
      s.replay = s.lone;
      s.replay_until = now + kTapReplay;
    }
    s.lone = 0;
  }
  if (s.replay) {
    if (now < s.replay_until)
      out |= s.replay;
    else
      s.replay = 0;
  }
  const bool open = g_open.load(std::memory_order_relaxed);
  if (s.was_open && !open) s.hidden = buttons;  // e.g. the B that closed the menu
  s.was_open = open;
  s.hidden &= buttons;
  out &= uint16_t(~s.hidden);
  if (open) {
    std::memset(pad, 0, 12);  // the game sees a neutral pad behind the menu
    return true;
  }
  pad[0] = uint8_t(out >> 8);
  pad[1] = uint8_t(out);
  return false;
}

}  // namespace rexnative::hud

namespace rexnative {

bool OverlayFrameFor(uint32_t width, uint32_t height, OverlayFrame* frame) {
  using namespace rexnative::hud;
  const auto now = std::chrono::steady_clock::now();
  if (!g_hint_end.load(std::memory_order_relaxed) && REXCVAR_GET(rexnative_hud_hint))
    g_hint_end.store((now + kHintTime).time_since_epoch().count(), std::memory_order_relaxed);
  if (!g_ready || (!g_open.load(std::memory_order_relaxed) && HintAlpha(now) <= 0.0f)) {
    std::lock_guard lock(g_frame_mutex);
    g_frame_ready = false;
    return false;
  }
  g_display_width.store(width);
  g_display_height.store(height);
  if (!g_frame_queued.exchange(true) && !rexnative::host::PostToUIThread(BuildFrame))
    g_frame_queued.store(false);
  std::lock_guard lock(g_frame_mutex);
  if (!g_frame_ready) return false;
  *frame = g_frame;
  return true;
}

bool OverlayFontIfChanged(uint32_t version, OverlayFont* font) {
  using namespace rexnative::hud;
  std::lock_guard lock(g_font_mutex);
  if (!g_font.version || g_font.version == version) return false;
  *font = g_font;
  return true;
}

}  // namespace rexnative
