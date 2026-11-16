"""From a game as it comes (a .7z/.zip/.rar of it, an STFS package, or a folder with default.xex)
to its ReXNative layer, unattended:

  1. extract (7-Zip for archives; STFS packages with stfs.py) into <work>/<game>/game-data;
  2. ReXGlue codegen of default.xex into <work>/<game>/generated/default, again and again: each
     unresolved branch target codegen reports becomes a function hint (as the ports'
     tools/gen_manifest.py do), until it succeeds;
  3. `adapt` into <work>/<game>/layer (game_layer.h, d3d_surface.inc, report.md).

Every step is skipped when its result is already there, so a run can be repeated or resumed.
The external programs run at below-normal priority (the PC stays usable) and nothing from the
game is run. <work>/<game>/prepare.json records each step's outcome; `survey` tabulates them.
"""
import json
import os
import re
import shutil
import subprocess
import sys
import time

from . import profile as prof, stfs, xex

BELOW_NORMAL = 0x00004000  # Windows process priority class
SEVEN_ZIP = [r'C:\Program Files\7-Zip\7z.exe', r'C:\Program Files (x86)\7-Zip\7z.exe', '7z']
ARCHIVES = ('.7z', '.zip', '.rar')
OPTIONS = {  # register-as-local codegen, as in PGR4Native, From Dust and Iron Brigade
    'cr_as_local': True, 'ctr_as_local': True, 'xer_as_local': True,
    'non_volatile_as_local': True, 'reserved_as_local': True,
}


def slug(name):
    """'Super Meat Boy (World) (XBLA).7z' -> 'super-meat-boy'."""
    base = os.path.basename(name)
    for ext in ARCHIVES:
        if base.lower().endswith(ext):
            base = base[:-len(ext)]
    base = re.sub(r'\s*\([^)]*\)', '', base)
    s = re.sub(r'[^a-z0-9]+', '-', base.lower()).strip('-')
    return s or 'game'


def _run(cmd, cwd=None):
    flags = BELOW_NORMAL if os.name == 'nt' else 0
    return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, errors='replace',
                          creationflags=flags)


def seven_zip():
    for c in SEVEN_ZIP:
        if os.path.isabs(c) and os.path.exists(c):
            return c
    return shutil.which('7z') or shutil.which('7za')


def extract_archive(src, dst, log):
    exe = seven_zip()
    if not exe:
        raise RuntimeError('7-Zip not found (needed for %s)' % src)
    os.makedirs(dst, exist_ok=True)
    log('  7-Zip: %s' % os.path.basename(src))
    r = _run([exe, 'x', '-y', '-o' + dst, src])
    if r.returncode != 0:
        raise RuntimeError('7-Zip failed on %s: %s' % (src, r.stdout[-500:] + r.stderr[-500:]))


def find_xex(root):
    """The shallowest default.xex under root, or None."""
    best = None
    for d, _, files in os.walk(root):
        for f in files:
            if f.lower() == 'default.xex':
                p = os.path.join(d, f)
                if best is None or p.count(os.sep) < best.count(os.sep):
                    best = p
    return best


def find_packages(root):
    out = []
    for d, _, files in os.walk(root):
        for f in files:
            p = os.path.join(d, f)
            if os.path.getsize(p) > 0xA000 and stfs.is_package(p):
                out.append(p)
    return out


def stage_game(source, work, log):
    """<work>/game-data with default.xex; returns facts about where it came from."""
    data = os.path.join(work, 'game-data')
    if os.path.exists(os.path.join(data, 'default.xex')):
        return {'reused': True}
    facts = {}
    root = source
    if os.path.isfile(source) and source.lower().endswith(ARCHIVES):
        root = os.path.join(work, 'archive')
        if not os.path.isdir(root) or not os.listdir(root):
            extract_archive(source, root, log)
    if os.path.isfile(root) and stfs.is_package(root):
        packages = [root]
    else:
        loose = find_xex(root)
        if loose:
            shutil.copytree(os.path.dirname(loose), data, dirs_exist_ok=True)
            facts['from'] = 'folder'
            return facts
        packages = find_packages(root)
    titles = []
    for p in packages:
        pkg = stfs.Package(p)
        try:
            kind = stfs.CONTENT_TYPES.get(pkg.content_type, '%08X' % pkg.content_type)
            titles.append({'package': os.path.relpath(p, work), 'title_id': '%08X' % pkg.title_id,
                           'type': kind, 'name': pkg.display_name})
            if pkg.content_type != 0x000D0000:
                continue  # title updates and DLC are not the game's executable
            log('  STFS %s: %s (%s)' % (os.path.basename(p), pkg.display_name, kind))
            n = pkg.extract(data)
            log('  %d files' % n)
        finally:
            pkg.close()
    facts['packages'] = titles
    if not os.path.exists(os.path.join(data, 'default.xex')):
        raise RuntimeError('no default.xex in %s (packages: %s)' % (source, titles))
    return facts


def ident_of(name):
    """'jet-set-radio' -> 'jetsetradio': the [project] name (letters and digits, not
    starting with a digit)."""
    ident = re.sub(r'[^a-z0-9]', '', name.lower()) or 'game'
    return 'game' + ident if ident[0].isdigit() else ident


def _hints(work):
    p = os.path.join(work, 'hints.txt')
    out = {}
    if os.path.exists(p):
        for line in open(p, encoding='utf-8'):
            parts = line.split('#')[0].split()
            if parts:
                out[int(parts[0], 16)] = int(parts[1], 16) if len(parts) > 1 else 0
    return out


def _write_manifest(work, name):
    lines = ['# ReXGlue codegen manifest written by ReXNative (rexnative prepare): edit hints.txt,',
             '# not this file.', '', '[project]', 'name = "%s"' % name, 'sdk_version = "0.10.1"',
             'game_root = "game-data"', '', '[entrypoint]', 'file_path = "game-data/default.xex"',
             'out_directory_path = "generated/default"']
    # Jump tables codegen cut short, given whole ([[switch_tables]]), the setjmp / longjmp the
    # game calls (setjmp_address, longjmp_address) and its XAPI fiber functions ([rexcrt]), all
    # from `rexnative project --find-functions` (functions.jump_tables, .setjmp_longjmp,
    # .xapi_fibers).
    includes = [f for f in ('switch_tables.toml', 'setjmp.toml', 'crt.toml')
                if os.path.exists(os.path.join(work, f))]
    lines.append('includes = [%s]' % ', '.join('"%s"' % f for f in includes))
    lines += ['%s = %s' % (k, str(v).lower()) for k, v in OPTIONS.items()]
    hints = _hints(work)
    if hints:
        lines.append('[entrypoint.functions]')
        for a, s in sorted(hints.items()):
            lines.append('0x%08X = { size = 0x%X }' % (a, s) if s else '0x%08X = {}' % a)
    path = os.path.join(work, '%s_manifest.toml' % name)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    return path


def codegen(work, name, rexglue, max_rounds, log):
    gen = os.path.join(work, 'generated', 'default')
    done = os.path.join(work, 'codegen.ok')
    # Reused only when made under this name (prepare's older runs used names with '_').
    if (os.path.exists(done) and os.path.isdir(gen) and
            os.path.exists(os.path.join(gen, '%s_init.h' % name))):
        return {'reused': True, 'rounds': 0}
    for old in os.listdir(work):
        if old.endswith('_manifest.toml') and old != '%s_manifest.toml' % name:
            os.remove(os.path.join(work, old))
    if os.path.exists(done):
        # Made under another name: codegen writes it again (the hints stay), and its
        # generated/rexglue.cmake, written once, is rendered again under this name.
        os.remove(done)
        stale = os.path.join(work, 'generated', 'rexglue.cmake')
        if os.path.exists(stale):
            os.remove(stale)
    for rnd in range(max_rounds):
        manifest = _write_manifest(work, name)
        t0 = time.time()
        r = _run([rexglue, 'codegen', os.path.basename(manifest)], cwd=work)
        text = r.stdout + r.stderr
        with open(os.path.join(work, 'codegen.log'), 'w', encoding='utf-8') as f:
            f.write(text)
        if r.returncode == 0:
            open(done, 'w').write('round %d\n' % rnd)
            log('  codegen: done (round %d, %.0f s)' % (rnd, time.time() - t0))
            return {'rounds': rnd + 1, 'hints': len(_hints(work))}
        targets = [int(t, 16) for t in re.findall(r'^\s+(0x[0-9A-Fa-f]+) from 0x[0-9A-Fa-f]+:',
                                                    text, re.M)]
        hints = _hints(work)
        new = [t for t in targets if t not in hints]
        if not new:
            tail = text.strip().splitlines()[-12:]
            raise RuntimeError('codegen failed with nothing to hint:\n' + '\n'.join(tail))
        for t in new:
            hints[t] = 0
        with open(os.path.join(work, 'hints.txt'), 'w', encoding='utf-8', newline='\n') as f:
            f.write(''.join('0x%08X 0x%X\n' % (a, s) for a, s in sorted(hints.items())))
        log('  codegen round %d: +%d hints (%.0f s)' % (rnd, len(new), time.time() - t0))
    raise RuntimeError('codegen still failing after %d rounds' % max_rounds)


def prepare(source, work_root, rexglue, refs, run_adapt, log=print, max_rounds=40):
    name = slug(source)
    work = os.path.join(work_root, name)
    os.makedirs(work, exist_ok=True)
    status_path = os.path.join(work, 'prepare.json')
    status = {'source': os.path.abspath(source), 'name': name, 'steps': {}}
    if os.path.exists(status_path):
        status.update(json.load(open(status_path, encoding='utf-8')))

    def save():
        with open(status_path, 'w', encoding='utf-8', newline='\n') as f:
            json.dump(status, f, indent=1)

    log('== %s' % name)
    try:
        status['steps']['extract'] = stage_game(source, work, log)
        info = xex.read(os.path.join(work, 'game-data', 'default.xex'))
        lib, ver = xex.d3d_library(info)
        status['xex'] = {'title_id': info['title_id'], 'version': info['version'],
                         'pe_name': info['pe_name'], 'd3d': [lib, ver], 'libs': info['libs']}
        log('  %s %s, D3D %s %s' % (info['title_id'], info['pe_name'], lib, ver))
        save()
        if not lib:
            raise RuntimeError('no XDK D3D library in default.xex (not a D3D9 title?)')
        # One word: ReXGlue names the generated sources after the [project] name as it is
        # (jet_set_radio_init.h) but generated/rexglue.cmake after its words run together
        # (jetsetradio_manifest.toml, jetsetradio_pch.h), so a name with '_' does not build.
        ident = ident_of(name)
        status['steps']['codegen'] = codegen(work, ident, rexglue, max_rounds, log)
        save()
        target = {'name': name, 'title': name, 'xex': os.path.join(work, 'game-data', 'default.xex'),
                  'generated': os.path.join(work, 'generated', 'default')}
        res = run_adapt(target, refs, os.path.join(work, 'layer'), log=lambda *a: None)
        found = {k: '%08X' % v['address'] for k, v in res['functions'].items() if v.get('address')}
        status['steps']['adapt'] = {
            'primary': res['primary'],
            'functions': found,
            'confidence': {k: v.get('confidence') for k, v in res['functions'].items()
                           if v.get('address')},
            'surface': len(res['surface']),
            'resolve_abi': (res.get('resolve_abi') or {}).get('abi'),
            'clear_color': ('%08X' % res['clear_color']['address']) if res.get('clear_color') else None,
        }
        status['ok'] = True
        log('  adapt: %d of %d functions, reference %s' % (len(found), len(res['functions']),
                                                          res['primary']))
    except Exception as e:  # recorded, so a batch goes on with the next game
        status['ok'] = False
        status['error'] = str(e)
        log('  FAILED: %s' % e)
    save()
    return status


KEY = ['Swap', 'Resolve', 'DrawIndexedVertices', 'DrawVertices', 'DrawVerticesUP',
       'BeginVertices', 'Clear', 'ShaderLoad', 'MakeSpace']


def survey(work_root):
    """Markdown table of every prepared game under work_root."""
    rows = []
    for name in sorted(os.listdir(work_root)):
        p = os.path.join(work_root, name, 'prepare.json')
        if not os.path.exists(p):
            continue
        s = json.load(open(p, encoding='utf-8'))
        x = s.get('xex') or {}
        a = (s.get('steps') or {}).get('adapt') or {}
        f = a.get('functions') or {}
        missing = [k for k in KEY if k not in f]
        # BeginIndexedVertices: the game draws with inline indices (DrawIndexedVerticesUP),
        # found by position (adapt.indexed_up); absent where the linker dropped it.
        indexed_up = f.get('BeginIndexedVertices')
        rows.append('| %s | %s | %s | %s | %s | %s | %s | %s |' % (
            name, x.get('title_id', ''), ' '.join(v for v in (x.get('d3d') or []) if v) or '—',
            a.get('primary', '—'), ('%d/%d' % (len(f), len(prof.FUNCTIONS))) if f else '—',
            ', '.join(missing) if f else (' '.join(s.get('error', '').split())[:90] or '—'),
            indexed_up or '—', 'ok' if s.get('ok') else 'FAILED'))
    head = ['| Game | Title | XDK D3D | Reference | Functions | Key hooks missing | '
            'Inline indices | Status |',
            '|---|---|---|---|---|---|---|---|']
    return '\n'.join(head + rows) + '\n'
