"""A runnable project for a game `rexnative prepare` made: the ReXGlue app around its generated
code, with ReXNative drawing it, built and laid out to run.

  python -m rexnative project <work>/<game> [--build] [--sdk-install DIR] [--sdk-src DIR]
         [--fidelityfx DIR] [--dlss-sdk DIR] [--dlss-nr DIR]

Writes into the game's work folder (beside its manifest, generated/ and layer/), for ReXGlue
v0.11:
  CMakeLists.txt, src/main.cpp,                   what `rexglue init` writes (find_package and
  src/<name>_app.h, src/<name>_app.cpp            rexglue_project, a TitleApp), plus ReXNative
                                                  (rexnative_add with layer/, renderer/src/host.h,
                                                  the settings HUD)
  src/import_stubs.cpp                            the imports the runtime lacks, as stubs
  <name>.toml                                     the run folder's configuration (kept once there)
The game's own host code, written by hand, is built with it: src/game_*.cpp (hooks of its guest
functions, REX_HOOK_RAW over __imp__sub_X), kept as they are; they stay in the work folder, with
the game's other files.
With --build: configures and builds it (clang++ from LLVM 20, $REXNATIVE_LLVM; Ninja; the Visual
Studio environment from scripts/vcenv.cmd) in build/, then lays out run/: the executable, the
DLLs the build staged (ReXGlue's modules, DLSS, FidelityFX), the configuration, game/ (a junction
to game-data/, which the app looks for beside it) and, with a title update installed
(title_update.py), title-update/ (its files, mounted as update:).
Nothing of the game goes anywhere else. The SDK paths default to $REXNATIVE_SDK_INSTALL,
$REXNATIVE_SDK_SRC, $REXNATIVE_FIDELITYFX, $REXNATIVE_DLSS_SDK and $REXNATIVE_DLSS_NR.
"""
import glob
import os
import re
import shutil
import subprocess
import time

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # the ReXNative checkout

# The ports' code generation flags (FromDustDeck's and Iron Brigade's build scripts): AVX2
# (the Steam Deck has it), precise floating point as the console computes it. ReXGlue v0.11
# builds with clang++ (the GNU driver) from LLVM 20 or later.
FLAGS = ('-march=x86-64-v3 -mtune=generic -mprefer-vector-width=128 -ffp-model=precise '
         '-fno-math-errno -fno-char8_t -D_CRT_SECURE_NO_WARNINGS')
# LLVM 20 (clang++, lld) for ReXGlue v0.11: $REXNATIVE_LLVM, else the first of these found.
LLVM_DIRS = [os.environ.get('REXNATIVE_LLVM', ''), r'F:\Tools\LLVM-20.1.8\bin',
             r'C:\Program Files\LLVM\bin']


def llvm_dir():
    for d in LLVM_DIRS:
        if d and os.path.exists(os.path.join(d, 'clang++.exe' if os.name == 'nt' else 'clang++')):
            return d
    raise SystemExit('LLVM 20 not found: set REXNATIVE_LLVM to its bin folder')


def manifest_name(work):
    """The [project] name of the work folder's codegen manifest."""
    for path in sorted(glob.glob(os.path.join(work, '*_manifest.toml'))):
        with open(path, encoding='utf-8') as f:
            m = re.search(r'^\s*name\s*=\s*"([^"]+)"', f.read(), re.M)
        if m:
            return m.group(1)
    raise SystemExit('no *_manifest.toml with a [project] name in %s (run `rexnative prepare`)' % work)


def _class_name(name):
    return ''.join(p[:1].upper() + p[1:] for p in re.split(r'[^A-Za-z0-9]+', name) if p) + 'App'


CMAKE = '''# {name}: recompiled with ReXGlue, drawn by ReXNative.
# Written by `python -m rexnative project` (ReXNative); generated/ is rexglue codegen's and
# layer/ `rexnative prepare`'s.

cmake_minimum_required(VERSION 3.25)
project({name} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(rexglue 0.11.0 REQUIRED CONFIG)
rexglue_project({name} OUTPUT_DIR generated/default)

# Host code: the app, the stubs of the imports the runtime lacks, and ReXNative (rexnative_add
# below) with the game's layer. OBJECT, not STATIC: an archive drops the hooks.
add_library({name}_common OBJECT src/{name}_app.cpp src/import_stubs.cpp{game_sources})
rexglue_setup_host_library({name}_common)

if(WIN32)
    add_executable({name} WIN32 src/main.cpp)
else()
    add_executable({name} src/main.cpp)
endif()
# ReXGlue's Xenos GPU driver: with ReXNative it only runs the GPU's fences and interrupts
# (gpu.xenos.skip_draws), and it is the comparison renderer (--rexnative=false, or F9).
rexglue_setup_target({name})
target_link_libraries({name} PRIVATE {name}_common)
# The hooks of the jump tables whose index the code overwrites before its bctr (src/cases_*.cpp,
# written with switch_tables*.toml when they are found), each in its binary's target.
if(EXISTS "${{CMAKE_CURRENT_SOURCE_DIR}}/src/cases_default.cpp")
    target_sources({name}_common PRIVATE src/cases_default.cpp)
endif()
{module_sources}
set(REXNATIVE_DIR "{rexnative}" CACHE PATH "ReXNative")
set(REXNATIVE_REXGLUE_SDK_SRC "" CACHE PATH "The source tree the ReXGlue SDK was built from")
set(REXNATIVE_FIDELITYFX_SDK "" CACHE PATH "AMD FidelityFX SDK v2.4.0")
set(REXNATIVE_DLSS_SDK "" CACHE PATH "NVIDIA DLSS SDK")
set(REXNATIVE_DLSS_NR "" CACHE PATH "NVIDIA DLSS 5 headers")
include("${{REXNATIVE_DIR}}/renderer/rexnative.cmake")
rexnative_add({name}_common
    EXECUTABLE      {name}
    LAYER_DIR       "${{CMAKE_CURRENT_SOURCE_DIR}}/layer"
    REXGLUE_SDK_SRC "${{REXNATIVE_REXGLUE_SDK_SRC}}"
    FIDELITYFX_SDK  "${{REXNATIVE_FIDELITYFX_SDK}}"
    DLSS_SDK        "${{REXNATIVE_DLSS_SDK}}"
    DLSS_NR         "${{REXNATIVE_DLSS_NR}}")
# The XDK's spin waits (db16cyc) sleep once they go on (renderer/src/spin_pause.h): after
# ReXGlue's own precompiled header, in the generated code only. (rexnative.cmake has set
# REXNATIVE_DIR to its own folder, renderer/.)
if(TARGET {name}_recompiled)
    target_precompile_headers({name}_recompiled PRIVATE "${{REXNATIVE_DIR}}/src/spin_pause.h")
endif()
# A game's own src/game_pch.h, when it has one, joins it after: macros over what codegen writes,
# for that game alone (Lazy Raiders: dcbz clears its 128-byte line).
if(TARGET {name}_recompiled AND EXISTS "${{CMAKE_CURRENT_SOURCE_DIR}}/src/game_pch.h")
    target_precompile_headers({name}_recompiled PRIVATE "${{CMAKE_CURRENT_SOURCE_DIR}}/src/game_pch.h")
endif()

# The link map (build/{name}.map) names the recompiled guest functions of a crash's offsets.
target_link_options({name} PRIVATE "LINKER:/MAP")
'''

MAIN = '''// {name}: recompiled with ReXGlue, drawn by ReXNative (written by `rexnative project`).
#include "{name}_app.h"

REX_TITLE_APP({cls})
'''

APP_H = '''// {name}: recompiled with ReXGlue, drawn by ReXNative (written by `rexnative project`).
#pragma once

#include <rex/app/title_app.h>

class {cls} : public rex::app::TitleApp {{
 public:
  {cls}();

 protected:
  rex::Result<void> OnConfigure(rex::SettingsRegistry& settings) override;
  rex::Result<void> OnInit() override;
  rex::Result<void> OnTitleLaunch(rex::kernel::TitleRuntime& title) override;
  void OnShutdown() override;
}};
'''

APP_CPP = '''// {name}: recompiled with ReXGlue, drawn by ReXNative (written by `rexnative project`).
#include "{name}_app.h"

#include <cstdio>
#include <filesystem>
#include <string>

#include <rex/cvar.h>
#include <rex/kernel/guest_memory.h>
#include <rex/kernel/module.h>
#include <rex/kernel/title_runtime.h>
#include <rex/logging/log_category.h>

#include "generated/default/{name}_init.h"
#include "host.h"     // ReXNative's connection to the app
#include "hud/hud.h"  // ReXNative's settings HUD

#ifndef WIN32_LEAN_AND_MEAN  // the SDK's headers may define it
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

REXCVAR_DEFINE_STRING(rexnative_dump_image, "", "ReXNative",
                      "Write the loaded default.xex image to this file when it is loaded "
                      "(rexnative project --find-functions reads it)");

namespace {{

// The folder of the executable: the game's files, its title update and the player's data are
// beside it (game/, title-update/, userdata/).
std::filesystem::path ExeDir() {{
  wchar_t path[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
  return std::filesystem::path(std::wstring(path, n)).parent_path();
}}

}}  // namespace

{cls}::{cls}() : TitleApp("{name}", kPPCImageConfig) {{}}

rex::Result<void> {cls}::OnConfigure(rex::SettingsRegistry& settings) {{
  rexnative::host::Configure();
  const std::filesystem::path dir = ExeDir();
  std::error_code ec;
  if (std::filesystem::is_directory(dir / "game", ec))
    (void)settings.SetText(rex::SettingLayer::kTitle, "title.game_root", (dir / "game").string());
  if (std::filesystem::is_directory(dir / "title-update", ec))
    (void)settings.SetText(rex::SettingLayer::kTitle, "title.update_root",
                           (dir / "title-update").string());
  (void)settings.SetText(rex::SettingLayer::kTitle, "app.user_root", (dir / "userdata").string());
  return TitleApp::OnConfigure(settings);
}}

rex::Result<void> {cls}::OnInit() {{
  if (auto r = TitleApp::OnInit(); !r) return r;
  rexnative::host::Attach(*this);
  rexnative::hud::Initialize();
  return {{}};
}}

// --rexnative_dump_image: the image as loaded (decrypted, decompressed), before any guest code
// runs. Each XEX DLL module first, loaded for it without its entry point (only in that run, which
// `project --find-functions` stops once the image is written), into <file>.<module>.bin.
rex::Result<void> {cls}::OnTitleLaunch(rex::kernel::TitleRuntime& title) {{
  if (const std::string path = REXCVAR_GET(rexnative_dump_image); !path.empty()) {{
    auto dump = [&](uint32_t base, uint32_t size, const std::string& file) {{
      auto image = title.GetGuestMemory().GetReadOnlySpan(base, size);
      if (std::FILE* f = image ? std::fopen(file.c_str(), "wb") : nullptr) {{
        std::fwrite(image->data(), 1, image->size(), f);
        std::fclose(f);
        REXLOG_INFO("image dump: {{:08X}}+{{:X}} -> {{}}", base, size, file);
      }}
    }};
{module_dumps}    dump(uint32_t(REX_IMAGE_BASE), uint32_t(REX_IMAGE_SIZE), path);
  }}
  rexnative::host::TitleLaunch(title);
  return TitleApp::OnTitleLaunch(title);
}}

void {cls}::OnShutdown() {{
  rexnative::hud::Shutdown();
  rexnative::host::Detach();
  TitleApp::OnShutdown();
}}
'''

TOML = '''# {name} (ReXGlue), drawn by ReXNative. Read at every start; a command line option
# (--section.name=value) overrides a setting for that session. Written by `rexnative project`.

# A window first: a new port is tested windowed.
[ui]
window_width = 1280
window_height = 720
fullscreen = false

# The full game's license (XBLA titles check it; 0 runs the trial).
[kernel]
license_mask = 1

# No cap on the memory the title commits: the runtime's 480 MB stopped Syndicate's and Diablo
# III's threads at start (Thread creation failed: C0000017) and the game went no further.
[runtime]
title_memory_mb = 0

# ReXNative draws and presents the game; ReXGlue's Xenos GPU driver runs the GPU's fences and
# interrupts (enabled = false: the stock renderer, to compare).
[rexnative]
enabled = true
'''


def _write(path, text, keep=False):
    if keep and os.path.exists(path):
        return False
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    return True


def _module_targets(work):
    """{generated folder name: CMake target} of the DLL modules, from the dll_targets.cmake codegen
    writes (`add_library(<name>_CRASH_DLL SHARED ${DLL_SOURCES_CRASH_DLL})` after the include of
    generated/crash_dll/sources.cmake)."""
    path = os.path.join(work, 'generated', 'default', 'dll_targets.cmake')
    if not os.path.exists(path):
        return {}
    text = open(path, encoding='utf-8').read()
    var = dict((v, d) for d, v in re.findall(
        r'include\(generated/([^/]+)/sources\.cmake\)\s*set\(DLL_SOURCES_(\w+) ', text))
    return {var[v]: t for t, v in re.findall(r'add_library\((\S+) SHARED \$\{DLL_SOURCES_(\w+)\}',
                                              text) if v in var}


def _module_cases_cmake(work):
    """CMake lines that build each DLL module's src/cases_<module>.cpp into its DLL, when there."""
    out = ''
    for module, target in sorted(_module_targets(work).items()):
        out += ('if(TARGET %s AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/cases_%s.cpp")\n'
                '    target_sources(%s PRIVATE src/cases_%s.cpp)\nendif()\n' % (
                    target, module, target, module))
    return out


def _module_dumps(work):
    """C++ that loads each DLL module (without its entry point) and dumps its image beside the
    executable's (the app's OnTitleLaunch, under --rexnative_dump_image): <file>.<module>.bin, as
    prepare.module_image names it."""
    from . import functions, prepare
    out = ''
    for m in prepare.modules(work):
        module = m['out'].split('/')[-1]
        gen = os.path.join(work, *m['out'].split('/'))
        try:
            base, size = functions.bounds(gen)[:2]
        except SystemExit:
            base, size = m['span']
        # game:\DLL\CRASH.DLL.XEX, as a C++ string literal
        guest = ('game:\\' + m['guest_path'].replace('/', '\\')).replace('\\', '\\\\')
        out += ('    if (title.GetModuleRegistry().LoadUserModule("%s", false)) {\n'
                '      dump(0x%08Xu, 0x%Xu,\n'
                '           std::filesystem::path(path).replace_extension().string() + ".%s.bin");\n'
                '    } else {\n'
                '      REXLOG_WARN("image dump: %s not loaded");\n'
                '    }\n' % (guest, base, size, module, guest))
    return out


def write(work):
    """Writes the project files into `work`; returns the project's name."""
    work = os.path.abspath(work)
    name = manifest_name(work)
    if not os.path.exists(os.path.join(work, 'generated', 'default', '%s_init.h' % name)):
        raise SystemExit('no generated/default/%s_init.h in %s (codegen not done)' % (name, work))
    if not os.path.exists(os.path.join(work, 'layer', 'game_layer.h')):
        raise SystemExit('no layer/game_layer.h in %s (rexnative prepare not done)' % work)
    if '_' in name:
        # generated/rexglue.cmake names the manifest and the PCH with the words run together
        # (jetsetradio_pch.h), the sources keep the '_' (jet_set_radio_pch.h): no build.
        raise SystemExit('project name %r has a "_": run `rexnative prepare` on this game again '
                         '(it renames the project to one word and runs codegen again)' % name)
    cls = _class_name(name)
    rexnative = HERE.replace('\\', '/')
    game_sources = ''.join(' src/' + os.path.basename(f) for f in
                           sorted(glob.glob(os.path.join(work, 'src', 'game_*.cpp'))))
    _write(os.path.join(work, 'CMakeLists.txt'),
           CMAKE.format(name=name, rexnative=rexnative, game_sources=game_sources,
                        module_sources=_module_cases_cmake(work)))
    _write(os.path.join(work, 'src', 'main.cpp'), MAIN.format(name=name, cls=cls))
    # ReXGlue v0.11 writes its own crash report (logs/), and the generated sources.cmake replaced
    # generated/rexglue.cmake.
    for old in (os.path.join(work, 'src', 'crash_report.cpp'),
                os.path.join(work, 'generated', 'rexglue.cmake')):
        if os.path.exists(old):
            os.remove(old)
    write_import_stubs(work, [])
    _write(os.path.join(work, 'src', '%s_app.h' % name), APP_H.format(name=name, cls=cls))
    _write(os.path.join(work, 'src', '%s_app.cpp' % name),
           APP_CPP.format(name=name, cls=cls, module_dumps=_module_dumps(work)))
    _write(os.path.join(work, '%s.toml' % name), TOML.format(name=name), keep=True)
    return name


def _need(path, what, env):
    if not path or not os.path.isdir(path):
        raise SystemExit('%s not found (%r): pass it or set %s' % (what, path, env))
    return os.path.abspath(path)


def _lower_priority():
    """A build or a game run on Linux (the Steam Deck) at a lower priority: the Deck stays
    responsive while it works (Windows uses BELOW_NORMAL_PRIORITY_CLASS for the same)."""
    try:
        os.nice(10)
    except OSError:
        pass


def game_command(exe, args):
    """The command that starts a game's Windows executable: itself on Windows; elsewhere through
    $REXNATIVE_RUNNER, a command that runs Windows programs (on the Deck: Proton, with a prefix
    of its own; ReXNativeDeck's kit/bin/rexdeck-run)."""
    if os.name == 'nt':
        return [exe] + list(args)
    runner = os.environ.get('REXNATIVE_RUNNER', '')
    if not runner:
        raise SystemExit('set REXNATIVE_RUNNER to a command that runs Windows programs '
                         '(on Linux the game runs under Proton or Wine)')
    import shlex
    return shlex.split(runner) + [exe] + list(args)


def start_game(exe, args, cwd, **kw):
    """Starts the game (game_command) in a process group of its own on Linux, so stop_game ends
    the runner and what it started."""
    if os.name != 'nt':
        kw.setdefault('start_new_session', True)
    return subprocess.Popen(game_command(exe, args), cwd=cwd, **kw)


def stop_game(p, grace=10):
    """Ends a game start_game started: on Linux the runner first gets SIGTERM (it ends what it
    started under Proton), then the whole group SIGKILL."""
    if p.poll() is not None:
        return
    if os.name == 'nt':
        p.kill()
        p.wait()
        return
    import signal
    for sig, wait in ((signal.SIGTERM, grace), (signal.SIGKILL, None)):
        try:
            os.killpg(p.pid, sig)
        except (ProcessLookupError, PermissionError):
            break
        try:
            p.wait(timeout=wait)
            return
        except subprocess.TimeoutExpired:
            continue
    p.wait()


# The runtime DLLs every game shares (renderer/src/native/common_dir.h): NVIDIA's DLSS and DLSS 5,
# AMD's FidelityFX. One copy in <work>/common (beside the games' folders) instead of one in each
# game's build/ and run/ (413 MB each).
COMMON_DLLS = ('nvngx_dlss.dll', 'nvngx_dlssnr.dll', 'amd_fidelityfx_loader_dx12.dll',
               'amd_fidelityfx_upscaler_dx12.dll', 'amd_fidelityfx_framegeneration_dx12.dll')


def common_dir(work):
    """<work>/common for the game folder <work>/<game>."""
    return os.path.join(os.path.dirname(os.path.abspath(work)), 'common')


def _put(src, folder):
    """Copies src into folder. A file there the game holds open (a session of it still running:
    Windows lets nobody overwrite or delete a running executable or a loaded DLL, but lets it be
    renamed) is moved aside as <name>.old-<n> first; _sweep removes those once they are free."""
    dst = os.path.join(folder, os.path.basename(src))
    try:
        shutil.copy2(src, dst)
        return
    except PermissionError:
        if not os.path.exists(dst):
            raise
    for n in range(100):
        aside = '%s.old-%d' % (dst, n)
        if not os.path.exists(aside):
            os.rename(dst, aside)
            break
    else:
        raise PermissionError('%s is in use and %s.old-* are full' % (dst, dst))
    shutil.copy2(src, dst)


def _drop(path):
    """Removes path, or moves it aside like _put when the game holds it open."""
    try:
        os.remove(path)
    except PermissionError:
        for n in range(100):
            aside = '%s.old-%d' % (path, n)
            if not os.path.exists(aside):
                os.rename(path, aside)
                return
        raise


def _sweep(folder):
    """The files _put and _drop moved aside, gone once nothing holds them any more."""
    for f in glob.glob(os.path.join(folder, '*.old-*')):
        try:
            os.remove(f)
        except OSError:
            pass


def build(work, sdk_install, sdk_src, fidelityfx, dlss_sdk, dlss_nr, jobs=0, log=print):
    """Configures, builds and lays out run/; returns the executable's path."""
    work = os.path.abspath(work)
    name = manifest_name(work)
    sdk_install = _need(sdk_install, 'the ReXGlue SDK install', 'REXNATIVE_SDK_INSTALL')
    sdk_src = _need(sdk_src, 'the ReXGlue SDK source tree', 'REXNATIVE_SDK_SRC')
    fidelityfx = _need(fidelityfx, 'the FidelityFX SDK', 'REXNATIVE_FIDELITYFX')
    dlss_sdk = _need(dlss_sdk, 'the DLSS SDK', 'REXNATIVE_DLSS_SDK')
    dlss_nr = _need(dlss_nr, 'the DLSS 5 headers', 'REXNATIVE_DLSS_NR')
    if os.name == 'nt':
        vcenv = os.path.join(HERE, 'scripts', 'vcenv.cmd')
        llvm = llvm_dir()
        compilers = '-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ '
    else:
        # A Windows build made on Linux (the Steam Deck): clang for the MSVC ABI with the MSVC
        # CRT and the Windows SDK, as the toolchain file sets them up (ReXNativeDeck's kit).
        toolchain = os.environ.get('REXNATIVE_TOOLCHAIN_FILE', '')
        if not os.path.isfile(toolchain):
            raise SystemExit('the CMake toolchain file for Windows builds not found (%r): set '
                             'REXNATIVE_TOOLCHAIN_FILE' % toolchain)
        compilers = '"-DCMAKE_TOOLCHAIN_FILE=%s" ' % os.path.abspath(toolchain)
    bdir = os.path.join(work, 'build')
    jobs = jobs or max(1, (os.cpu_count() or 2) - 2)
    # A build folder of the old SDK (clang-cl, generated/rexglue.cmake) cannot be reused.
    cache = os.path.join(bdir, 'CMakeCache.txt')
    if os.path.exists(cache):
        with open(cache, encoding='utf-8', errors='replace') as f:
            old_sdk = 'clang-cl' in f.read()
        if old_sdk:
            shutil.rmtree(bdir)
    # REXNATIVE_DIR on the command line: the CMakeLists' cached value never replaces the one an
    # earlier configure stored, so a build run from another checkout compiled the first one's
    # renderer without a word. rexglue_DIR likewise: find_package keeps the SDK it found first,
    # whatever CMAKE_PREFIX_PATH says later, so a game moved to another SDK (a newer ReXGlue)
    # kept the old one's headers. SDL3_DIR too: rexglue's package finds SDL3 (find_dependency),
    # whose cached folder stayed the old SDK's while that folder was there.
    sdl3 = os.path.join(sdk_install, 'cmake')
    sdl3_dir = ('"-DSDL3_DIR=%s" ' % sdl3.replace('\\', '/')
                if os.path.exists(os.path.join(sdl3, 'SDL3Config.cmake')) else '')
    configure = ('cmake -S "{w}" -B "{b}" -G Ninja -DCMAKE_BUILD_TYPE=Release ' + compilers +
                 '"-DCMAKE_PREFIX_PATH={sdk}" -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF '
                 '"-Drexglue_DIR={pkg}" ' + sdl3_dir +
                 '"-DREXNATIVE_DIR={rx}" '
                 '"-DREXNATIVE_REXGLUE_SDK_SRC={src}" "-DREXNATIVE_FIDELITYFX_SDK={ffx}" '
                 '"-DREXNATIVE_DLSS_SDK={dlss}" "-DREXNATIVE_DLSS_NR={nr}" '
                 '"-DREXNATIVE_COMMON_DIR={common}"').format(
        w=work, b=bdir, sdk=sdk_install, rx=HERE.replace('\\', '/'), src=sdk_src, ffx=fidelityfx,
        pkg=os.path.join(sdk_install, 'lib', 'cmake', 'rexglue').replace('\\', '/'),
        dlss=dlss_sdk, nr=dlss_nr, common=common_dir(work).replace(os.sep, '/'))
    # A precompiled header of the generated code made before spin_pause.h joined it: CMake writes
    # the new header list, but Ninja does not compile the generated code again by itself, so the
    # old one goes (and with it every object built on it).
    pch_dir = os.path.join(bdir, 'CMakeFiles', '%s_recompiled.dir' % name)
    pch_list = os.path.join(pch_dir, 'cmake_pch.hxx')
    if os.path.exists(pch_list):
        with open(pch_list, encoding='utf-8', errors='replace') as f:
            listed = f.read()
        has_spin = 'spin_pause.h' in listed
        # The same when the game's src/game_pch.h came or went.
        game_pch = os.path.exists(os.path.join(work, 'src', 'game_pch.h'))
        if not has_spin or game_pch != ('game_pch.h' in listed):
            for old in glob.glob(os.path.join(pch_dir, 'cmake_pch.hxx*')):
                os.remove(old)
            log('%s: the generated code is compiled again (%s its precompiled header)' % (
                name, 'spin_pause.h joins' if not has_spin else
                'src/game_pch.h joins' if game_pch else 'src/game_pch.h leaves'))
    steps = [('configure', configure), ('build', 'cmake --build "%s" -j %d' % (bdir, jobs))]
    os.makedirs(bdir, exist_ok=True)
    for what, cmd in steps:
        logf = os.path.join(bdir, '%s.log' % what)
        log('%s: %s (log: %s)' % (name, what, logf))
        t0 = time.time()
        if os.name == 'nt':
            # A batch file: cmd's own quoting rules, not Python's argument escaping.
            script = os.path.join(bdir, '%s.cmd' % what)
            with open(script, 'w', encoding='utf-8', newline='\r\n') as f:
                f.write('@echo off\ncall "%s" >nul || exit /b 1\nset "PATH=%s;%%PATH%%"\n'
                        'set "CFLAGS=%s"\nset "CXXFLAGS=%s"\n%s\n' % (vcenv, llvm, FLAGS, FLAGS, cmd))
            args, env, nice = ['cmd', '/d', '/c', script], None, None
        else:
            import shlex
            args, env = shlex.split(cmd), dict(os.environ, CFLAGS=FLAGS, CXXFLAGS=FLAGS)
            nice = _lower_priority
        with open(logf, 'w', encoding='utf-8', errors='replace') as out:
            r = subprocess.run(args, stdout=out, stderr=subprocess.STDOUT, env=env,
                               preexec_fn=nice)
        from . import prepare
        prepare.spent('build', time.time() - t0, 1 if what == 'build' else 0)  # configure: its time only
        if r.returncode:
            with open(logf, encoding='utf-8', errors='replace') as f:
                text = f.read()
            labels = sorted(set(int(a, 16) for a in UNDECLARED_LABEL.findall(text)))
            if labels:
                sites = [(f, int(n or m), int(a, 16))
                         for f, n, m, a in UNDECLARED_LABEL_AT.findall(text)]
                raise LabelsMissing(labels, sites)
            # A precompiled header made before the SDK's headers were installed again: clang
            # refuses it rather than rebuilding it. It goes and the build runs again.
            stale = sorted(set(STALE_PCH.findall(text)))
            if stale and not getattr(build, '_pch_retry', False):
                for pch in stale:
                    if os.path.exists(pch):
                        os.remove(pch)
                log('%s: %d precompiled headers older than the SDK made again' % (name, len(stale)))
                build._pch_retry = True
                try:
                    return build(work, sdk_install, sdk_src, fidelityfx, dlss_sdk, dlss_nr, jobs,
                                 log)
                finally:
                    build._pch_retry = False
            imports = sorted(set(UNDEFINED_IMPORT.findall(text)))
            if imports and write_import_stubs(work, imports):
                log('%s: %d imports the runtime lacks, as stubs: %s' % (
                    name, len(imports), ' '.join(imports)))
                return build(work, sdk_install, sdk_src, fidelityfx, dlss_sdk, dlss_nr, jobs, log)
            raise SystemExit('%s failed (%d):\n%s' % (what, r.returncode, text[-3000:]))
    # run/: what the app needs beside it.
    run = os.path.join(work, 'run')
    os.makedirs(run, exist_ok=True)
    _sweep(run)
    exe = os.path.join(bdir, '%s.exe' % name)
    for f in [exe, os.path.join(bdir, '%s.pdb' % name)]:
        if os.path.exists(f):
            _put(f, run)
    # Every DLL the build staged beside the executable: ReXGlue's modules and drivers, SDL3,
    # NVIDIA's DLSS and AMD's FidelityFX; the ReXGlue DLLs of an older SDK go (v0.10's
    # rexruntime.dll and rexgpu-xenos.dll, its FidelityFX runtime).
    # NVIDIA's and AMD's (COMMON_DLLS) are in the common folder the renderer looks in (<work>/common,
    # renderer/src/native/common_dir.h), once for every game: a copy an older build left goes.
    staged = {os.path.basename(f).lower() for f in glob.glob(os.path.join(bdir, '*.dll'))}
    shared = os.path.isdir(common_dir(work))
    for f in glob.glob(os.path.join(run, '*.dll')):
        n = os.path.basename(f).lower()
        if (n not in staged and (n.startswith('rex') or n == 'amd_fidelityfx_dx12.dll')) or \
                (shared and n in COMMON_DLLS):
            _drop(f)
    for f in glob.glob(os.path.join(bdir, '*.dll')):
        _put(f, run)
    toml = os.path.join(work, '%s.toml' % name)
    if os.path.exists(toml) and not os.path.exists(os.path.join(run, '%s.toml' % name)):
        shutil.copy2(toml, run)
    # The DLSS 5 look that suits the game (dlss5.py), unless the player chose one in the HUD.
    from . import dlss5
    dlss5.apply(work, [toml, os.path.join(run, '%s.toml' % name)], log=log)
    # Settings that make the picture the console's where an emulated detail differs (tweaks.py).
    from . import tweaks
    tweaks.apply(work, [toml, os.path.join(run, '%s.toml' % name)], log=log)
    # game/ -> game-data/; title-update/ -> the installed title update's files (title_update.py),
    # which the app mounts as update: when it finds the folder beside it.
    links = [('game', os.path.join(work, 'game-data')),
             ('title-update', os.path.join(work, 'title-update', 'content'))]
    for link, src in links:
        dst = os.path.join(run, link)
        if os.path.exists(dst) or not os.path.isdir(src):
            continue
        if os.name == 'nt':
            subprocess.run(['cmd', '/d', '/c', 'mklink', '/J', dst, src], capture_output=True)
        else:
            os.symlink(src, dst)
    stamp(run, sdk_install)
    log('%s: %s' % (name, os.path.join(run, '%s.exe' % name)))
    return os.path.join(run, '%s.exe' % name)


STAMP = 'rexnative-build.json'


def sdk_identity(sdk_install):
    """The SDK install as built: a hash of the name, size and time of its codegen tool and DLLs
    (bin/), which change together when the SDK is built again with new patches."""
    import hashlib
    h = hashlib.sha1()
    b = os.path.join(sdk_install, 'bin')
    for f in sorted(glob.glob(os.path.join(b, '*.dll')) + glob.glob(os.path.join(b, 'rexglue.exe'))):
        st = os.stat(f)
        h.update(('%s %d %d;' % (os.path.basename(f).lower(), st.st_size, int(st.st_mtime))).encode())
    return h.hexdigest()[:16]


def stamp(run, sdk_install):
    """run/rexnative-build.json: what this build was made with, the ReXNative commit of this
    checkout (and whether its renderer/, rexnative/ or sdk/ had changes not committed) and the
    SDK install, so a game built before a fix can be told from one built after it."""
    import json
    import time
    info = {'built': int(time.time()), 'rexnative': None, 'rexnative_date': None, 'dirty': None,
            'checkout': HERE, 'sdk_install': sdk_install, 'sdk': None}
    try:
        out = subprocess.run(['git', '-C', HERE, 'log', '-1', '--format=%H %ct'], capture_output=True,
                             text=True, timeout=30).stdout.split()
        if len(out) == 2:
            info['rexnative'], info['rexnative_date'] = out[0], int(out[1])
        st = subprocess.run(['git', '-C', HERE, 'status', '--porcelain', '--untracked-files=no', '--',
                             'renderer', 'rexnative', 'sdk'], capture_output=True, text=True,
                            timeout=30).stdout
        info['dirty'] = bool(st.strip())
    except (OSError, subprocess.SubprocessError, ValueError):
        pass
    try:
        info['sdk'] = sdk_identity(sdk_install)
    except OSError:
        pass
    with open(os.path.join(run, STAMP), 'w', encoding='utf-8') as f:
        json.dump(info, f, indent=1)


# Kernel / XAM imports the runtime does not export (the link fails on them): stubs in the game,
# with what each returns. The Xbox Live Vision camera (3D Ultra Minigolf Adventures): not
# connected.
UNDEFINED_IMPORT = re.compile(r"undefined symbol: __declspec\(dllimport\) _(\w+)")
STUB_RETURNS = [('XUsbcam', 0x8007048F)]  # ERROR_DEVICE_NOT_CONNECTED as an HRESULT


def write_import_stubs(work, names):
    """src/import_stubs.cpp: a stub for each import in `names` (and those already there)."""
    path = os.path.join(work, 'src', 'import_stubs.cpp')
    have = []
    if os.path.exists(path):
        with open(path, encoding='utf-8') as f:
            have = re.findall(r'REX_EXPORT_STUB_RETURN\(__imp__(\w+),', f.read())
    allnames = sorted(set(have) | set(names))
    lines = ['// Imports the runtime does not export, as stubs (written by `rexnative project` from the',
             '// link errors). Each logs a warning and returns its value.', '',
             '#include <rex/ppc/hook.h>', '#include <rex/logging/log_category.h>', '']
    for n in allnames:
        value = next((v for prefix, v in STUB_RETURNS if n.startswith(prefix)), 0)
        lines.append('REX_EXPORT_STUB_RETURN(__imp__%s, 0x%X)' % (n, value))
    _write(path, '\n'.join(lines) + '\n')
    return [n for n in names if n not in have]


# A branch codegen made a goto to a label of another function (code the compiler shares between
# functions, such as a common epilogue): the target becomes a function of its own.
UNDECLARED_LABEL = re.compile(r"use of undeclared label 'loc_([0-9A-F]{8})'")
UNDECLARED_LABEL_AT = re.compile(
    r"^(\S.*?\.cpp)(?:\((\d+),\d+\)|:(\d+):\d+): error: use of undeclared label "
    r"'loc_([0-9A-F]{8})'", re.M)
DEFINE_FUNC = re.compile(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)')
STALE_PCH = re.compile(r"has been modified since the precompiled header '([^']+\.pch)'")


class LabelsMissing(Exception):
    def __init__(self, labels, sites=()):
        super().__init__('undeclared labels: ' + ' '.join('%08X' % a for a in labels))
        self.labels = labels
        self.sites = list(sites)  # (generated file, line, label)


def _function_at_line(path, line):
    """The guest function whose DEFINE_REX_FUNC holds line `line` of a generated file."""
    found = None
    with open(path, encoding='utf-8', errors='replace') as f:
        for n, text in enumerate(f, 1):
            if n > line:
                break
            m = DEFINE_FUNC.match(text)
            if m:
                found = int(m.group(1), 16)
    return found


def _grow_functions(work, hints, sites, log, name):
    """A branch to a label found not to be a function start (not-starts.txt: as a function it split
    a loop): the function that branches there ends too early, codegen having taken its end from
    pdata, and grows to the next known function start after the label. Minecraft's 82B09098
    branches to 82B094E0 and 82B094E4: as hints they split it, without them they were labels of
    no function, and the rounds went back and forth."""
    from . import listing
    tree = listing.load(os.path.join(work, 'generated', 'default'))
    starts = sorted(set(tree.funcs) | set(hints))
    grown = []
    for path, line, label in sites:
        f = _function_at_line(path, line)
        nxt = [x for x in starts if x > label]
        if f is None or f > label or not nxt:
            continue
        # The next start past the label that is not itself one of the hints the span covers (as
        # _merge_split_functions does, the hints inside the span go: they were pieces of it).
        inside = [h for h in hints if f < h <= label]
        nxt = [x for x in nxt if x not in hints or x > label]
        if not nxt:
            continue
        size = nxt[0] - f
        if hints.get(f, 0) >= size and not inside:
            continue
        for h in inside:
            del hints[h]
        hints[f] = size
        grown.append((f, nxt[0]))
    if grown:
        log('%s: %d functions grown over labels past their end: %s' % (
            name, len(grown), ' '.join('%08X-%08X' % g for g in sorted(set(grown))[:8])))
    return bool(grown)


def _merge_split_functions(work, hints, sites, log, name):
    """A branch to a label that is already a function: a hint split one function in two (a loop
    whose body codegen reported apart, Perfect Dark's 82450488 and 8245049C). The two become one
    again: the hints inside the span go and the first start gets the span's size."""
    from . import listing
    tree = listing.load(os.path.join(work, 'generated', 'default'))
    merged = []
    for path, line, label in sites:
        f = _function_at_line(path, line)
        if f is None or f not in tree.funcs or label not in tree.funcs:
            continue
        ends = [max(a for a, _ in tree.funcs[x]) + 4 for x in (f, label) if tree.funcs[x]]
        start, end = min(f, label), max(ends)
        for h in [h for h in hints if start < h < end]:
            del hints[h]
        hints[start] = max(hints.get(start, 0), end - start)
        merged.append((start, end))
    if merged:
        log('%s: %d functions split by a hint joined again: %s' % (
            name, len(merged), ' '.join('%08X-%08X' % m for m in sorted(set(merged))[:8])))
    return bool(merged)


def build_fixing(work, rexglue, build_args, jobs=0, rounds=10, log=print):
    """build(), and while the generated code jumps to labels of other functions, those targets
    become hints, codegen runs and the build again (up to `rounds` times)."""
    from . import prepare
    name = manifest_name(work)
    # Generated code from before the share_registers check (prepare.live_in_nonvolatiles) gets it.
    prepare.codegen(work, name, rexglue, 40, log)
    for _ in range(rounds):
        try:
            return build(work, *build_args, jobs=jobs, log=log)
        except LabelsMissing as e:
            hints = prepare._hints(work)
            known_false = prepare.not_starts(work)
            new = [a for a in e.labels if a not in hints and a not in known_false]
            split = [s for s in e.sites if s[2] in hints]
            joined = _merge_split_functions(work, hints, split, log, name) if split else False
            outside = [s for s in e.sites if s[2] in known_false and s[2] not in hints]
            grown = _grow_functions(work, hints, outside, log, name) if outside else False
            if not new and not joined and not grown:
                raise SystemExit('%s: %s, already hints' % (name, e))
            if new:
                log('%s: %d branch targets in other functions become functions: %s' % (
                    name, len(new), ' '.join('%08X' % a for a in new[:12])))
            for a in new:
                hints[a] = 0
            _write_hints(work, hints)
            done = os.path.join(work, 'codegen.ok')
            if os.path.exists(done):
                os.remove(done)
            prepare.codegen(work, name, rexglue, 40, log)
    raise SystemExit('%s: still undeclared labels after %d rounds' % (name, rounds))


def _write_hints(work, hints):
    with open(os.path.join(work, 'hints.txt'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(''.join('0x%08X 0x%X\n' % (a, s) for a, s in sorted(hints.items())))


def _codegen_and_build(work, name, rexglue, build_args, log, before_build=None):
    """before_build: called between the codegen run and the build (`prepare --project` makes the
    layer again there, over the functions codegen now knows, so one build compiles both)."""
    from . import prepare
    done = os.path.join(work, 'codegen.ok')
    if os.path.exists(done):
        os.remove(done)
    prepare.codegen(work, name, rexglue, 40, log)
    if before_build:
        before_build()
    build_fixing(work, rexglue, build_args, log=log)


def find_functions(work, rexglue, build_args, seconds=60, log=print, before_build=None):
    """Dumps the loaded image (a run with --rexnative_dump_image, stopped once it is written),
    finds the function starts codegen missed (functions.py), adds them to hints.txt, gives the
    jump tables codegen cut short (switch_tables.toml) and the addresses setjmp and longjmp are
    called at (setjmp.toml), runs codegen and builds again (before_build: called between the
    two). Returns the addresses added."""
    import time
    from . import functions, prepare
    work = os.path.abspath(work)
    name = manifest_name(work)
    run = os.path.join(work, 'run')
    image = os.path.join(run, 'image.bin')
    if os.path.exists(image):
        os.remove(image)
    p = start_game(os.path.join(run, '%s.exe' % name), [
        '--rexnative_dump_image=' + image, '--fullscreen=false', '--window_width=1280',
        '--window_height=720'], cwd=run, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    t0 = time.time()
    try:
        while time.time() - t0 < seconds and p.poll() is None:
            if os.path.exists(image) and os.path.getsize(image) > 0:
                time.sleep(1)  # written in one go; a moment for it to be closed
                break
            time.sleep(0.5)
    finally:
        stop_game(p)
        prepare.spent('game', time.time() - t0)
    if not os.path.exists(image):
        raise SystemExit('%s: no image written (%s)' % (name, image))
    hints = prepare._hints(work)
    gen = os.path.join(work, 'generated', 'default')
    # Hints inside the register save / restore helpers (an older find_functions, or a run's
    # report) go: as functions they break the helpers' calls (functions.save_restore_helpers).
    helpers = functions.helper_ranges(gen, image)
    bad = [a for a in hints if any(h0 <= a < h1 for h0, h1 in helpers)]
    for a in bad:
        del hints[a]
    if bad:
        log('%s: %d hints inside the register save / restore helpers removed: %s' % (
            name, len(bad), ' '.join('%08X' % a for a in sorted(bad))))
    # Not the hints a codegen run showed to split a function (prepare.not_starts).
    refused = prepare.not_starts(work)
    new = [a for a in functions.find(gen, image, hints) if a not in refused]
    log('%s: %d function starts codegen missed%s' % (
        name, len(new), (': ' + ' '.join('%08X' % a for a in new[:12]) +
                         (' ...' if len(new) > 12 else '')) if new else ''))
    # Jump tables codegen cut short (it traps on the cases it left out): given whole.
    tables = find_switch_tables(work)
    if tables:
        log('%s: %d jump tables codegen cut short, given whole: %s' % (
            name, len(tables), ' '.join('%08X (%d)' % (a, n) for a, n in tables[:8])))
    # setjmp / longjmp: calls to them made the host's (functions.setjmp_longjmp).
    setjmps, longjmps, _ = functions.setjmp_longjmp(gen, image)
    jumps = write_setjmp(work, setjmps, longjmps)
    if jumps:
        log('%s: setjmp at %s, longjmp at %s' % (
            name, ' '.join('%08X' % a for a in setjmps), ' '.join('%08X' % a for a in longjmps)))
    # XAPI fibers: the SDK's host-fiber versions in their place (functions.xapi_fibers).
    fibers = functions.xapi_fibers(gen)
    crt = write_crt(work, fibers)
    if crt:
        log('%s: XAPI fibers given to the SDK: %s' % (
            name, ' '.join('%s %08X' % kv for kv in fibers.items())))
    if new or bad or tables or jumps or crt:
        for a in new:
            hints[a] = 0
        _write_hints(work, hints)
        _codegen_and_build(work, name, rexglue, build_args, log, before_build)
    return new


def write_setjmp(work, setjmps, longjmps):
    """Writes <work>/setjmp.toml (the manifest includes it) with the addresses the game calls
    setjmp and longjmp at; True when it changed. Both or neither: a host longjmp needs the host
    setjmp that filled its jmp_buf (ppc_longjmp aborts on a guest jmp_buf it does not know)."""
    path = os.path.join(work, 'setjmp.toml')
    if not setjmps or not longjmps:
        return False
    text = '\n'.join([
        '# The addresses the game calls setjmp and longjmp at (a body and the thunks to it),',
        '# found by ReXNative (rexnative project --find-functions): codegen makes those calls the',
        "# host's setjmp / longjmp. Included by the manifest.",
        'setjmp_address = [%s]' % ', '.join('0x%08X' % a for a in setjmps),
        'longjmp_address = [%s]' % ', '.join('0x%08X' % a for a in longjmps), ''])
    if os.path.exists(path):
        with open(path, encoding='utf-8') as f:
            if f.read() == text:
                return False
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    return True


def write_crt(work, functions_found):
    """Writes <work>/crt.toml (the manifest includes it): ReXGlue's [shim] table (v0.11; [rexcrt]
    before), the game's functions the SDK replaces with its own (the XAPI fibers); True when it
    changed."""
    path = os.path.join(work, 'crt.toml')
    if not functions_found:
        return False
    text = '\n'.join([
        "# The game's XAPI functions the ReXGlue SDK replaces with its own (kernel/crt), found by",
        '# ReXNative (rexnative project --find-functions). Included by the manifest.',
        '[shim]'] + ['%s = 0x%08X' % kv for kv in functions_found.items()] + [''])
    if os.path.exists(path):
        with open(path, encoding='utf-8') as f:
            if f.read() == text:
                return False
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    return True


def switch_tables_file(work, binary='default'):
    """The [[switch_tables]] of a binary: switch_tables.toml for default.xex (the entrypoint's
    includes), switch_tables.<module>.toml for a DLL module (its own includes)."""
    return os.path.join(work, 'switch_tables.toml' if binary == 'default' else
                        'switch_tables.%s.toml' % binary)


def cases_source(work, binary='default'):
    """The hooks of a binary's tables whose index is overwritten before the bctr (C++, built into
    that binary's target: the executable's for default.xex, the module's DLL for a module)."""
    return os.path.join(work, 'src', 'cases_%s.cpp' % binary)


def write_switch_tables(work, found, binary='default'):
    """Adds the tables (functions.jump_tables) to the binary's switch_tables file (the manifest
    includes it); returns the new ones as [(bctr address, entries)]. A table whose index is
    overwritten before its bctr (by_target) is a switch on the register the target was loaded
    into, with a [[midasm_hook]] at the bctr that turns that target back into its index (the
    first entry that holds it; any other value, the switch's default, which traps). That
    register is the compiler's temporary for mtctr, dead after the bctr."""
    path = switch_tables_file(work, binary)
    have = {}
    if os.path.exists(path):
        with open(path, encoding='utf-8') as f:
            text = f.read()
        hooked = {int(a, 16) for a in re.findall(
            r'\[\[midasm_hook\]\]\s*address = 0x([0-9A-Fa-f]+)', text)}
        for block in text.split('[[switch_tables]]')[1:]:
            block = block.split('[[midasm_hook]]')[0]
            a = re.search(r'address = 0x([0-9A-Fa-f]+)', block)
            r = re.search(r'register = (\d+)', block)
            labels = re.findall(r'0x([0-9A-Fa-f]{8})', block.split('labels', 1)[-1])
            if a and r:
                addr = int(a.group(1), 16)
                have[addr] = (int(r.group(1)), [int(x, 16) for x in labels], addr in hooked)
    found = [(t[0], t[1], t[2], t[3] if len(t) > 3 else False) for t in found]
    new = [(a, reg, t, hook) for a, reg, t, hook in found if a not in have]
    if not new:
        return []
    for a, reg, t, hook in new:
        have[a] = (reg, t, hook)
    lines = ['# Jump tables ReXGlue codegen cut short, given whole (written by ReXNative:',
             '# rexnative project --find-functions). Included by the manifest.', '']
    for a in sorted(have):
        reg, t, hook = have[a]
        lines += ['[[switch_tables]]', 'address = 0x%08X' % a, 'register = %d' % reg,
                  'labels = [%s]' % ', '.join('0x%08X' % x for x in t), '']
        if hook:
            lines += ['[[midasm_hook]]', 'address = 0x%08X' % a,
                      'name = "rexnative_case_%08X"' % a, 'registers = ["r%d"]' % reg, '']
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines))
    hooks = [(a, have[a][1]) for a in sorted(have) if have[a][2]]
    if hooks:
        src = ['// The jump tables whose index the code overwrites before its bctr (li + addis: the',
               "// table address in the load's second operand). The switch is on the loaded target;",
               '// the hook at the bctr turns it back into its index. Written by ReXNative',
               '// (project.write_switch_tables) from %s.' % os.path.basename(path),
               '#include <cstddef>', '#include <cstdint>', '', '#include <rex/ppc/context.h>', '',
               'namespace {', '',
               'void CaseIndex(PPCRegister& r, const uint32_t* targets, size_t count) {',
               '  for (size_t i = 0; i < count; ++i) {',
               '    if (r.u32 == targets[i]) {',
               '      r.u64 = i;',
               '      return;',
               '    }',
               '  }',
               '  r.u64 = count;  // not one of the cases: the default, which traps',
               '}', '', '}  // namespace', '']
        for a, t in hooks:
            src += ['void rexnative_case_%08X(PPCRegister& r) {' % a,
                    '  static constexpr uint32_t kTargets[] = {%s};' % ', '.join(
                        '0x%08X' % x for x in t),
                    '  CaseIndex(r, kTargets, sizeof(kTargets) / sizeof(kTargets[0]));', '}', '']
        os.makedirs(os.path.join(work, 'src'), exist_ok=True)
        with open(cases_source(work, binary), 'w', encoding='utf-8', newline='\n') as f:
            f.write('\n'.join(src))
    return [(a, len(t)) for a, reg, t, hook in new]


def find_switch_tables(work):
    """functions.jump_tables over default.xex's generated code and each DLL module's, with their
    loaded images (run/image.bin, run/image.<module>.bin, from --rexnative_dump_image), added to
    their switch_tables files. Returns the new ones as [(bctr address, entries)]."""
    from . import functions, prepare
    new = []
    for binary, gen, image in prepare.table_sources(work):
        new += write_switch_tables(work, functions.jump_tables(gen, image), binary)
    return new


# What the runtime logs when the game reaches code codegen did not make a function of: a thread
# started there, or an indirect call (a function pointer, a virtual method).
MISSING = [re.compile(r'No function registered at ([0-9A-Fa-f]{8})'),
           re.compile(r'unregistered function at guest address 0x([0-9A-Fa-f]{8})'),
           re.compile(r'Unresolved call from 0x[0-9A-Fa-f]+ to 0x([0-9A-Fa-f]{8})')]


def discover(work, rexglue, build_args, rounds=30, seconds=40, log=print):
    """Runs the game windowed, adds each function it reports missing to hints.txt, runs codegen
    and builds again, until a run lasts `seconds` without one (or `rounds` runs). Returns the
    addresses added."""
    from . import prepare
    work = os.path.abspath(work)
    name = manifest_name(work)
    run = os.path.join(work, 'run')
    exe = os.path.join(run, '%s.exe' % name)
    added = []
    for rnd in range(rounds):
        logf = os.path.join(run, 'discover.log')
        if os.path.exists(logf):
            os.remove(logf)
        t0 = time.time()
        p = start_game(exe, ['--log_file=' + logf, '--fullscreen=false', '--window_width=1280',
                             '--window_height=720'], cwd=run, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        try:
            code = p.wait(timeout=seconds)
        except subprocess.TimeoutExpired:
            stop_game(p)
            code = None
        prepare.spent('game', time.time() - t0)
        text = ''
        if os.path.exists(logf):
            with open(logf, encoding='utf-8', errors='replace') as f:
                text = f.read()
        found = []
        for pat in MISSING:
            for m in pat.finditer(text):
                a = int(m.group(1), 16)
                if a not in found:
                    found.append(a)
        hints = prepare._hints(work)
        new = [a for a in found if a not in hints]
        if not new:
            log('%s: run %d %s, nothing missing' % (
                name, rnd, 'still running after %d s' % seconds if code is None
                else 'exited 0x%08X' % (code & 0xFFFFFFFF)))
            return added
        # Codegen can drop a hint again (one that would split a function it already has): then
        # the next run reports the same address, and another codegen and build change nothing.
        # Burnout Crash went round 12 times for 8A601E1C, about 1 h 40. Only new addresses
        # are worth a round.
        again = [a for a in new if a in added]
        if len(again) == len(new):
            log('%s: run %d: missing %s again, which codegen did not keep as a function start: '
                'discovery stops here' % (name, rnd, ', '.join('%08X' % a for a in again)))
            return added
        new = [a for a in new if a not in added]
        log('%s: run %d: missing %s' % (name, rnd, ', '.join('%08X' % a for a in new)))
        for a in new:
            hints[a] = 0
        _write_hints(work, hints)
        added += new
        _codegen_and_build(work, name, rexglue, build_args, log)
    return added
