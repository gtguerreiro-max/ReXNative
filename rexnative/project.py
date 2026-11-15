"""A runnable project for a game `rexnative prepare` made: the ReXGlue app around its generated
code, with ReXNative drawing it, built and laid out to run.

  python -m rexnative project <work>/<game> [--build] [--sdk-install DIR] [--sdk-src DIR]
         [--fidelityfx DIR] [--dlss-sdk DIR] [--dlss-nr DIR]

Writes into the game's work folder (beside its manifest, generated/ and layer/):
  CMakeLists.txt, src/main.cpp, src/<name>_app.h  what `rexglue init` writes, plus ReXNative
                                                  (rexnative_add with layer/, the settings HUD)
  <name>.toml                                     the run folder's configuration (kept once there)
With --build: configures and builds it (clang-cl, Ninja, the Visual Studio environment from
scripts/vcenv.cmd) in build/, then lays out run/: the executable, the SDK's and ReXNative's DLLs,
the configuration, and game/ (a junction to game-data/, which the app looks for beside it).
Nothing of the game goes anywhere else. The SDK paths default to $REXNATIVE_SDK_INSTALL,
$REXNATIVE_SDK_SRC, $REXNATIVE_FIDELITYFX, $REXNATIVE_DLSS_SDK and $REXNATIVE_DLSS_NR.
"""
import glob
import os
import re
import shutil
import subprocess

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # the ReXNative checkout

# The ports' code generation flags (FromDustDeck's and Iron Brigade's build scripts): AVX2
# (the Steam Deck has it), precise floating point as the console computes it.
FLAGS = ('/clang:-march=x86-64-v3 /clang:-mtune=generic /clang:-mprefer-vector-width=128 '
         '/clang:-ffp-model=precise /clang:-fno-math-errno /Zc:char8_t- -Wno-unknown-argument '
         '-Wno-unused-command-line-argument /D_CRT_SECURE_NO_WARNINGS')


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
# layer/ `rexnative prepare`'s. The SDK boilerplate is in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project({name} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

if(WIN32)
    add_executable({name} WIN32 src/main.cpp)
else()
    add_executable({name} src/main.cpp)
endif()

# The Xenos GPU plugin: ReXGlue's stock renderer. With ReXNative it only runs the GPU's fences
# and interrupts (--gpu_skip_draws), and it is the comparison renderer (--rexnative=false).
rexglue_setup_target({name} GPU_PLUGINS xenos)

set(REXNATIVE_DIR "{rexnative}" CACHE PATH "ReXNative")
set(REXNATIVE_REXGLUE_SDK_SRC "" CACHE PATH "The source tree the ReXGlue SDK was built from")
set(REXNATIVE_FIDELITYFX_SDK "" CACHE PATH "AMD FidelityFX SDK v2.3.0")
set(REXNATIVE_DLSS_SDK "" CACHE PATH "NVIDIA DLSS SDK")
set(REXNATIVE_DLSS_NR "" CACHE PATH "NVIDIA DLSS 5 headers")
include("${{REXNATIVE_DIR}}/renderer/rexnative.cmake")
rexnative_add({name}
    LAYER_DIR       "${{CMAKE_CURRENT_SOURCE_DIR}}/layer"
    REXGLUE_SDK_SRC "${{REXNATIVE_REXGLUE_SDK_SRC}}"
    FIDELITYFX_SDK  "${{REXNATIVE_FIDELITYFX_SDK}}"
    DLSS_SDK        "${{REXNATIVE_DLSS_SDK}}"
    DLSS_NR         "${{REXNATIVE_DLSS_NR}}")

if(MSVC)
    target_link_options({name} PRIVATE /MAP)
endif()
'''

MAIN = '''// {name}: recompiled with ReXGlue, drawn by ReXNative (written by `rexnative project`).

#include "generated/default/{name}_init.h"

#include "{name}_app.h"

REX_DEFINE_APP({name}, {cls}::Create)
'''

APP = '''// {name}: recompiled with ReXGlue, drawn by ReXNative (written by `rexnative project`).

#pragma once

#include <rex/rex_app.h>

#include "hud/hud.h"  // ReXNative's settings HUD

class {cls} : public rex::ReXApp {{
 public:
  using rex::ReXApp::ReXApp;

  // The settings HUD (F1, or Back+Start on the pad) writes the configuration file.
  void OnConfigurePaths(rex::PathConfig& paths) override {{
    rexnative::hud::SetConfigPath(paths.config_path);
  }}
  void OnCreateDialogs(rex::ui::ImGuiDrawer*) override {{ rexnative::hud::Initialize(window()); }}
  void OnShutdown() override {{ rexnative::hud::Shutdown(); }}

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {{
    return std::unique_ptr<{cls}>(new {cls}(ctx, "{name}", PPCImageConfig));
  }}
}};
'''

TOML = '''# {name} (ReXGlue), drawn by ReXNative. Read at every start; a command line flag
# (--name=value) overrides a setting for that session. Written by `rexnative project`.

# A window first: a new port is tested windowed.
window_width = 1280
window_height = 720
fullscreen = false

# ReXNative draws and presents the game; the Xenos plugin runs the GPU's fences and interrupts
# (rexnative = false: the stock renderer, to compare).
gpu_plugin = "xenos"
rexnative = true

# The full game's license (XBLA titles check it; 0 runs the trial).
license_mask = 1
'''


def _write(path, text, keep=False):
    if keep and os.path.exists(path):
        return False
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    return True


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
    _write(os.path.join(work, 'CMakeLists.txt'), CMAKE.format(name=name, rexnative=rexnative))
    _write(os.path.join(work, 'src', 'main.cpp'), MAIN.format(name=name, cls=cls))
    _write(os.path.join(work, 'src', '%s_app.h' % name), APP.format(name=name, cls=cls))
    _write(os.path.join(work, '%s.toml' % name), TOML.format(name=name), keep=True)
    return name


def _need(path, what, env):
    if not path or not os.path.isdir(path):
        raise SystemExit('%s not found (%r): pass it or set %s' % (what, path, env))
    return os.path.abspath(path)


def build(work, sdk_install, sdk_src, fidelityfx, dlss_sdk, dlss_nr, jobs=0, log=print):
    """Configures, builds and lays out run/; returns the executable's path."""
    work = os.path.abspath(work)
    name = manifest_name(work)
    sdk_install = _need(sdk_install, 'the ReXGlue SDK install', 'REXNATIVE_SDK_INSTALL')
    sdk_src = _need(sdk_src, 'the ReXGlue SDK source tree', 'REXNATIVE_SDK_SRC')
    fidelityfx = _need(fidelityfx, 'the FidelityFX SDK', 'REXNATIVE_FIDELITYFX')
    dlss_sdk = _need(dlss_sdk, 'the DLSS SDK', 'REXNATIVE_DLSS_SDK')
    dlss_nr = _need(dlss_nr, 'the DLSS 5 headers', 'REXNATIVE_DLSS_NR')
    vcenv = os.path.join(HERE, 'scripts', 'vcenv.cmd')
    bdir = os.path.join(work, 'build')
    jobs = jobs or max(1, (os.cpu_count() or 2) - 2)
    configure = ('cmake -S "{w}" -B "{b}" -G Ninja -DCMAKE_BUILD_TYPE=Release '
                 '-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_LINKER=lld-link '
                 '"-DCMAKE_PREFIX_PATH={sdk}" -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF '
                 '"-DREXNATIVE_REXGLUE_SDK_SRC={src}" "-DREXNATIVE_FIDELITYFX_SDK={ffx}" '
                 '"-DREXNATIVE_DLSS_SDK={dlss}" "-DREXNATIVE_DLSS_NR={nr}"').format(
        w=work, b=bdir, sdk=sdk_install, src=sdk_src, ffx=fidelityfx, dlss=dlss_sdk, nr=dlss_nr)
    steps = [('configure', configure), ('build', 'cmake --build "%s" -j %d' % (bdir, jobs))]
    os.makedirs(bdir, exist_ok=True)
    for what, cmd in steps:
        logf = os.path.join(bdir, '%s.log' % what)
        # A batch file: cmd's own quoting rules, not Python's argument escaping.
        script = os.path.join(bdir, '%s.cmd' % what)
        with open(script, 'w', encoding='utf-8', newline='\r\n') as f:
            f.write('@echo off\ncall "%s" >nul || exit /b 1\nset "CFLAGS=%s"\nset "CXXFLAGS=%s"\n'
                    '%s\n' % (vcenv, FLAGS, FLAGS, cmd))
        log('%s: %s (log: %s)' % (name, what, logf))
        with open(logf, 'w', encoding='utf-8', errors='replace') as out:
            r = subprocess.run(['cmd', '/d', '/c', script], stdout=out, stderr=subprocess.STDOUT)
        if r.returncode:
            with open(logf, encoding='utf-8', errors='replace') as f:
                tail = f.read()[-3000:]
            raise SystemExit('%s failed (%d):\n%s' % (what, r.returncode, tail))
    # run/: what the app needs beside it.
    run = os.path.join(work, 'run')
    os.makedirs(run, exist_ok=True)
    exe = os.path.join(bdir, '%s.exe' % name)
    for f in [exe, os.path.join(bdir, '%s.pdb' % name)]:
        if os.path.exists(f):
            shutil.copy2(f, run)
    for pattern in ('nvngx_*.dll', 'amd_fidelityfx_*_dx12.dll'):
        for f in glob.glob(os.path.join(bdir, pattern)):
            shutil.copy2(f, run)
    for f in glob.glob(os.path.join(sdk_install, 'bin', '*.dll')):
        shutil.copy2(f, run)
    toml = os.path.join(work, '%s.toml' % name)
    if os.path.exists(toml) and not os.path.exists(os.path.join(run, '%s.toml' % name)):
        shutil.copy2(toml, run)
    game = os.path.join(run, 'game')
    if not os.path.exists(game):
        src = os.path.join(work, 'game-data')
        if os.name == 'nt':
            subprocess.run(['cmd', '/d', '/c', 'mklink', '/J', game, src], capture_output=True)
        else:
            os.symlink(src, game)
    log('%s: %s' % (name, os.path.join(run, '%s.exe' % name)))
    return os.path.join(run, '%s.exe' % name)


# What the runtime logs when the game reaches code codegen did not make a function of: a thread
# started there, or an indirect call (a function pointer, a virtual method).
MISSING = [re.compile(r'No function registered at ([0-9A-Fa-f]{8})'),
           re.compile(r'unregistered function at guest address 0x([0-9A-Fa-f]{8})')]


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
        p = subprocess.Popen([exe, '--log_file=' + logf, '--fullscreen=false',
                              '--window_width=1280', '--window_height=720'], cwd=run,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            code = p.wait(timeout=seconds)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
            code = None
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
        log('%s: run %d: missing %s' % (name, rnd, ', '.join('%08X' % a for a in new)))
        for a in new:
            hints[a] = 0
        with open(os.path.join(work, 'hints.txt'), 'w', encoding='utf-8', newline='\n') as f:
            f.write(''.join('0x%08X 0x%X\n' % (a, s) for a, s in sorted(hints.items())))
        added += new
        done = os.path.join(work, 'codegen.ok')
        if os.path.exists(done):
            os.remove(done)
        prepare.codegen(work, name, rexglue, 40, log)
        build(work, *build_args, log=log)
    return added
