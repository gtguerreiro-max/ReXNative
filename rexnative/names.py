"""ReXNative's names for what came from PGR4Native: the renderer's options (cvars), namespace,
macros, log prefixes and game-side functions.

`rename(text)` turns code or text written with the old names into the new ones (`neutral` runs it
on a port's sources after its edits; it was run once on renderer/). `legacy_table(texts)` lists
every option the old texts define, with its new name and type, for
renderer/src/native/legacy_names.cpp, which keeps the old names working.
"""
import re

# The options people type, first: the switch that makes the renderer draw and present the
# game, and the one that opens it in a window beside the Xenos plugin.
RULES = [
    (r'(?<![A-Za-z0-9])pgr4_native_primary(?![A-Za-z0-9_])', 'rexnative'),
    (r'(?<![A-Za-z0-9])pgr4_native(?![A-Za-z0-9_-])', 'rexnative_beside_xenos'),
    (r'(?<![A-Za-z0-9])pgr4_native_', 'rexnative_'),
    (r'(?<![A-Za-z0-9])pgr4_d3d_', 'rexnative_d3d_'),
    (r'(?<![A-Za-z0-9])pgr4_render_height(?![A-Za-z0-9_])', 'rexnative_render_height'),
    (r'(?<![A-Za-z0-9])fromdust_trace_camera(?![A-Za-z0-9_])', 'rexnative_trace_camera'),
    (r'(?<![A-Za-z0-9])pgr4_dlss5', 'rexnative_dlss5'),
    (r'(?<![A-Za-z0-9])pgr4_shader_xlate', 'rexnative_shader_xlate'),
    (r'_pgr4_gfx', '_rexnative_gfx'),
    # Code.
    (r'pgr4::native', 'rexnative'),
    (r'namespace pgr4\s*\{\s*namespace native\s*\{', 'namespace rexnative {'),
    (r'(?<![A-Za-z0-9])PGR4_', 'REXNATIVE_'),
    (r'(?<![A-Za-z0-9])Pgr4Enable60Fps(?![A-Za-z0-9])', 'RexNativeGame60Fps'),
    (r'(?<![A-Za-z0-9])Pgr4LastCarStepNs(?![A-Za-z0-9])', 'RexNativeGameStepNs'),
    (r'(?<![A-Za-z0-9])FromDustWorldStepped(?![A-Za-z0-9])', 'RexNativeGameWorldStepped'),
    (r'(?<![A-Za-z0-9])Pgr4PresentationInterval(?![A-Za-z0-9])', 'RexNativePresentationInterval'),
    (r'(?<![A-Za-z0-9])Pgr4FrameRateUnlocked(?![A-Za-z0-9])', 'RexNativeFrameRateUnlocked'),
    (r'(?<![A-Za-z0-9])Pgr4MotionBlurStep(?![A-Za-z0-9])', 'RexNativeMotionBlurStep'),
    # What people see: log prefixes, option categories, D3D12 object names, the cache folder.
    (r'PGR4 native', 'ReXNative'),
    (r'PGR4 D3D', 'ReXNative D3D'),
    (r'L"pgr4 native ', 'L"rexnative '),
    (r', "PGR4",', ', "ReXNative",'),
    (r', "FromDust",(?=\s*\n?\s*"Log the scene camera)', ', "ReXNative",'),
    (r'"pgr4-native"', '"rexnative"'),
    (r'\\\\pgr4-native(?= and compile)', r'\\\\rexnative'),
]
_COMPILED = [(re.compile(a), b) for a, b in RULES]

DEFINE = re.compile(r'REXCVAR_DEFINE_(BOOL|INT32|INT64|UINT32|UINT64|DOUBLE|STRING|COMMAND)\(\s*'
                    r'([a-z0-9_]+)\s*,')
TYPES = {'BOOL': 'Boolean', 'INT32': 'Int32', 'INT64': 'Int64', 'UINT32': 'Uint32',
         'UINT64': 'Uint64', 'DOUBLE': 'Double', 'STRING': 'String', 'COMMAND': 'Command'}


def rename(text):
    for rx, new in _COMPILED:
        text = rx.sub(new, text)
    return text


def rename_name(name):
    """The new name of one option."""
    return rename(name)


def legacy_table(texts):
    """[(old name, new name, FlagType)] of every option the texts define under an old name."""
    out = {}
    for t in texts:
        for kind, name in DEFINE.findall(t):
            new = rename_name(name)
            if new != name:
                out[name] = (name, new, TYPES[kind])
    return sorted(out.values())


def legacy_cpp(table):
    rows = ''.join('    {"%s", "%s", rex::cvar::FlagType::%s},\n' % row for row in table)
    return LEGACY_CPP.replace('@ROWS@', rows.rstrip('\n'))


LEGACY_CPP = '''// ReXNative: the options' names before ReXNative renamed them (PGR4Native's pgr4_native_*,
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
@ROWS@
};

void Announce(const Legacy& l) {
  static std::mutex m;
  static std::set<std::string_view> said;
  std::lock_guard lock(m);
  if (said.insert(l.old_name).second)
    REXLOG_WARN("ReXNative: {} is now {} (the old name still works)", l.old_name, l.new_name);
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

const char* OldNameOf(std::string_view new_name) {
  for (const Legacy& l : kLegacy)
    if (new_name == l.new_name) return l.old_name;
  return nullptr;
}

}  // namespace rexnative
'''

LEGACY_H = '''// ReXNative: the options' old names (legacy_names.cpp).
#pragma once

#include <string_view>

namespace rexnative {

// The name an option had before ReXNative renamed it, or nullptr.
const char* OldNameOf(std::string_view new_name);

}  // namespace rexnative
'''
