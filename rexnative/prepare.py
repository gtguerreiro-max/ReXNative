"""From a game as it comes (a .7z/.zip/.rar of it, an STFS package, or a folder with default.xex)
to its ReXNative layer, unattended:

  1. extract (7-Zip for archives; STFS packages with stfs.py) into <work>/<game>/game-data, each
     part's progress logged as `    N% (X MB/s)` lines (meter.py) the Manager shows;
  2. ReXGlue codegen of default.xex into <work>/<game>/generated/default, again and again: each
     unresolved branch target codegen reports becomes a function hint (as the ports'
     tools/gen_manifest.py do), until it succeeds;
  3. `adapt` into <work>/<game>/layer (game_layer.h, d3d_surface.inc, report.md).

Every step is skipped when its result is already there, so a run can be repeated or resumed.
The external programs run at below-normal priority (the PC stays usable) and nothing from the
game is run. <work>/<game>/prepare.json records each step's outcome; `survey` tabulates them.
"""
import bisect
import glob
import json
import locale
import os
import re
import shutil
import struct
import subprocess
import sys
import time

from . import meter, profile as prof, stfs, xex, xiso

BELOW_NORMAL = 0x00004000  # Windows process priority class
SEVEN_ZIP = [r'C:\Program Files\7-Zip\7z.exe', r'C:\Program Files (x86)\7-Zip\7z.exe', '7z']
ARCHIVES = ('.7z', '.zip', '.rar')
IMAGES = ('.iso',)  # disc images (xiso.py)
OPTIONS = {  # register-as-local codegen, as in PGR4Native, From Dust and Iron Brigade
    'cr_as_local': True, 'ctr_as_local': True, 'xer_as_local': True,
    'non_volatile_as_local': True, 'reserved_as_local': True,
}


def options(work):
    """OPTIONS with the game's own (codegen-options.txt in its work folder, lines `name = false`):
    a game whose code reads its callers' saved registers from the stack needs them saved there.
    With non_volatile_as_local ReXGlue drops the calls to the save and restore helpers
    (__savegprlr_N, __restgprlr_N), so neither those registers nor the return address reach the
    guest stack: Burnout Crash's garbage collector walks the stack frames and read stale words
    there as object pointers (a crash in its mark phase about 40 s in)."""
    out = dict(OPTIONS)
    p = os.path.join(work, 'codegen-options.txt')
    if os.path.exists(p):
        for line in open(p, encoding='utf-8'):
            k, eq, v = line.split('#')[0].partition('=')
            if eq and k.strip() in out:
                out[k.strip()] = v.strip().lower() in ('1', 'true', 'yes', 'on')
    return out


VECTOR_MATH = ('vmaddfp', 'vnmsubfp', 'vmsum3fp128', 'vmsum4fp128', 'vrefp', 'vrsqrtefp',
               'vexptefp', 'fres', 'frsqrte')


def vector_math(work):
    """The game's choice of ReXGlue's exact vector math ([vector_math] in the manifest), from
    vector-math.txt in its work folder (lines `vmsum4fp128 = exact`); every other instruction
    keeps ReXGlue's default, "fast". Forza Horizon's collisions need vmsum3fp128 and vmsum4fp128
    exact: the fast dot product (the host's dpps) gives an infinity where the console's overflows
    to a NaN, and the game's cars and destructible objects then pass through each other, as they
    did in Xenia until its dot product gave the NaN too (xenia aa284307, February 2020)."""
    out = {}
    p = os.path.join(work, 'vector-math.txt')
    if os.path.exists(p):
        for line in open(p, encoding='utf-8'):
            k, eq, v = line.split('#')[0].partition('=')
            k, v = k.strip(), v.strip().strip('"').lower()
            if eq and k in VECTOR_MATH and v in ('fast', 'exact'):
                out[k] = v
    return out


def _vector_math_lines(work):
    vm = vector_math(work)
    return ['', '[vector_math]'] + ['%s = "%s"' % (k, vm[k]) for k in sorted(vm)] if vm else []


def _options_differ(work, gen):
    """The generated code was made with other register options than the game's (its _init.cpp
    records them as `.name = value,`), or another vector math choice than the game's (the last
    manifest written holds it), or a DLL module has no generated code."""
    manifests = [f for f in os.listdir(work) if f.endswith('_manifest.toml')]
    if manifests:
        text = open(os.path.join(work, manifests[0]), encoding='utf-8', errors='replace').read()
        i = text.find('\n[vector_math]\n')
        had = text[i:].strip().splitlines() if i >= 0 else []
        if had != [l for l in _vector_math_lines(work) if l]:
            return True
    # A DLL module with no generated code yet (one found by a newer rule, as the .dll modules
    # were): codegen runs again, for the manifest to list it.
    if any(not os.path.isdir(d) for d in _generated_dirs(work)[1:]):
        return True
    # Or one generated with its function table elsewhere than _module_table_bases says.
    mods = modules(work)
    for i, at in _module_table_bases(work, mods).items():
        d = os.path.join(work, *mods[i]['out'].split('/'))
        for f in glob.glob(os.path.join(d, '*_pch.h')):
            m = re.search(r'#define REX_FUNC_TABLE_BASE (0x[0-9A-Fa-f]+)',
                          open(f, encoding='utf-8', errors='replace').read())
            if m and int(m.group(1), 16) != at:
                return True
    want = options(work)
    for d in [gen] + _generated_dirs(work)[1:]:
        if not os.path.isdir(d):
            continue
        for f in os.listdir(d):
            if f.endswith('_init.cpp'):
                text = open(os.path.join(d, f), encoding='utf-8', errors='replace').read()
                for k, v in re.findall(r'\.(\w+_as_local) = (true|false)', text):
                    if k in want and want[k] != (v == 'true'):
                        return True
    return False


def slug(name):
    """'Super Meat Boy (World) (XBLA).7z' -> 'super-meat-boy'."""
    base = os.path.basename(name)
    for ext in ARCHIVES + IMAGES:
        if base.lower().endswith(ext):
            base = base[:-len(ext)]
    base = re.sub(r'\s*\([^)]*\)', '', base)
    s = re.sub(r'[^a-z0-9]+', '-', base.lower()).strip('-')
    return s or 'game'


# Where a prepare's time goes: {what: [times, seconds]} for 'codegen' (each run of the codegen
# tool), 'build' (configure + build) and 'game' (the runs that dump the image and look for missing
# functions). `prepare` says them in each game's last line (spent_text): the log has no clock.
SPENT = {}


def spent(what, seconds, times=1):
    n = SPENT.setdefault(what, [0, 0.0])
    n[0] += times
    n[1] += seconds


def spent_text():
    return ', '.join('%s %.0f s in %d' % (k, v[1], v[0]) for k, v in SPENT.items())


def _run(cmd, cwd=None):
    if os.name == 'nt':
        return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, errors='replace',
                              creationflags=BELOW_NORMAL)
    from .project import _lower_priority
    return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, errors='replace',
                          preexec_fn=_lower_priority)


def seven_zip():
    for c in SEVEN_ZIP:
        if os.path.isabs(c) and os.path.exists(c):
            return c
    return shutil.which('7z') or shutil.which('7za')


SEVEN_ZIP_PCT = re.compile(rb'^\s*(\d{1,3})%')
SEVEN_ZIP_SIZE = re.compile(rb'^(Total )?Physical Size = (\d+)')


def extract_archive(src, dst, log):
    """src (an archive) into dst with 7-Zip, its percentages as meter lines. With -bsp1 7-Zip writes
    ` 23% 900 - <file>` to stdout again and again over the same line (\\r on Windows, backspaces in
    the Linux 7-Zip). Before that, while it opens the archive, it writes ` 52% 1600 Open` (a RAR's
    headers are spread through it: Forza Motorsport 2's 5.2 GB reached 97% in 20 s on a hard disk,
    then took 5 minutes from 97% to the end). Only the percentages after the archive's properties
    (from its `--` line on) are the extraction's. The rate is of the archive read (its Physical Size,
    the volumes' total for a multi-volume one)."""
    exe = seven_zip()
    if not exe:
        raise RuntimeError('7-Zip not found (needed for %s)' % src)
    os.makedirs(dst, exist_ok=True)
    log('  7-Zip: %s' % os.path.basename(src))
    kw = {'creationflags': BELOW_NORMAL} if os.name == 'nt' else {}
    if os.name != 'nt':
        from .project import _lower_priority
        kw['preexec_fn'] = _lower_priority
    try:
        size = os.path.getsize(src)
    except OSError:
        size = 1
    p = subprocess.Popen([exe, 'x', '-y', '-bsp1', '-o' + dst, src], stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, **kw)
    m, text, rest = None, [], b''  # m from the `--` line on: the extraction
    # Windows' 7-Zip writes its messages to a pipe in the console's code page (cp850 here)
    enc = 'oem' if os.name == 'nt' else locale.getpreferredencoding(False)
    try:
        for chunk in iter(lambda: p.stdout.read1(1 << 16), b''):
            *lines, rest = re.split(rb'[\r\n\b]', rest + chunk)
            for line in lines:
                pct = SEVEN_ZIP_PCT.match(line)
                if pct:
                    if m is not None and not line.rstrip().endswith(b' Open'):
                        m.percent(int(pct.group(1)))
                    continue
                if m is None and line.strip() == b'--':
                    m = meter.Meter(size, log)
                sz = SEVEN_ZIP_SIZE.match(line)
                if sz and (sz.group(1) or size <= 1):
                    size = int(sz.group(2))
                    if m is not None:
                        m.total = size
                if line.strip():
                    text = (text + [line.decode(enc, 'replace').strip()])[-20:]
    except BaseException:
        p.kill()
        raise
    finally:
        p.stdout.close()
    if rest.strip():
        text.append(rest.decode(enc, 'replace').strip())
    if p.wait() != 0:
        raise RuntimeError('7-Zip failed on %s: %s' % (src, '\n'.join(text)[-1000:]))
    _writable(dst)


def _writable(root):
    """Clears the read-only bit a RAR can carry on its files (Windows' attribute): later steps
    replace, patch and remove them, which Windows refuses on a read-only file."""
    if os.name != 'nt':
        return
    for d, _, files in os.walk(root):
        for f in files:
            p = os.path.join(d, f)
            try:
                if not os.stat(p).st_mode & 0o200:
                    os.chmod(p, 0o666)
            except OSError:
                pass


def rmtree(path):
    """shutil.rmtree that also removes read-only files (Windows refuses those): an extraction
    stopped half way leaves them before _writable runs."""
    def retry(func, p, _):
        os.chmod(p, 0o777)
        func(p)
    if sys.version_info >= (3, 12):
        shutil.rmtree(path, onexc=retry)
    else:
        shutil.rmtree(path, onerror=retry)


def entry_xex(data):
    """game-data's default.xex as it is named there, or None: a package's can be Default.xex and a
    disc's DEFAULT.XEX, and Linux (the Steam Deck) tells names apart where Windows does not."""
    try:
        for f in sorted(os.listdir(data)):
            if f.lower() == 'default.xex' and os.path.isfile(os.path.join(data, f)):
                return os.path.join(data, f)
    except OSError:
        pass
    return None


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


def rel_or_abs(path, start):
    """path relative to start, or absolute when it cannot be (on Windows, another drive or a
    network share: relpath raises ValueError). The user's own game files may be on another
    drive than the work folder."""
    try:
        return os.path.relpath(path, start)
    except ValueError:
        return os.path.abspath(path)


def _fresh(path):
    """An empty folder at path (what an interrupted run left there goes)."""
    if os.path.isdir(path):
        rmtree(path)
    elif os.path.exists(path):
        os.remove(path)
    os.makedirs(path)
    return path


def _fresh_part(base, log):
    """An empty base.part/ to stage into. When an earlier run that is still alive (orphaned by an
    app restart, hung) holds a file open in it, Windows can't remove it (WinError 145/5/32); then
    a base.part-<pid>/ of this run's own is used, and part folders no process holds any more are
    cleared on the way."""
    for old in glob.glob(glob.escape(base) + '.part-*'):
        shutil.rmtree(old, ignore_errors=True)
    try:
        return _fresh(base + '.part')
    except OSError as e:
        own = '%s.part-%d' % (base, os.getpid())
        log('  %s.part is in use (%s): staging in %s' % (os.path.basename(base), e,
                                                         os.path.basename(own)))
        return _fresh(own)


def replace_dir(part, final, tries=30):
    """os.replace for a folder just written: Windows refuses to rename one while another process
    holds a file in it (WinError 5/32), and Defender or the search indexer scanning what an
    extraction just wrote does that for a while (Forza Motorsport 4: 11 GB staged in 18 minutes,
    then "Acesso negado" renaming game-data.part; the same rename worked when tried again later). Tries
    again for about three minutes before giving up."""
    for i in range(tries):
        try:
            os.replace(part, final)
            return
        except PermissionError:
            if i == tries - 1:
                raise
            time.sleep(min(1 + i, 8))


def stage_game(source, work, log):
    """<work>/game-data with default.xex; returns facts about where it came from.

    The archive is extracted into archive.part/ and the game into game-data.part/, each renamed only
    when complete: a run stopped half way (the Deck's battery, a cancel) never leaves a half-extracted
    folder that the next run would take as done."""
    final = os.path.join(work, 'game-data')
    if entry_xex(final):
        return {'reused': True}
    extracted = os.path.join(work, 'archive')
    if not os.path.exists(source) and not (source.lower().endswith(ARCHIVES) and
                                           os.path.isdir(extracted) and os.listdir(extracted)):
        # Only extraction reads the source (or archive/, the archive extracted): a game whose
        # game-data is complete returns above, so both can go once it is prepared, as the
        # Manager does.
        raise FileNotFoundError('the source %s is not there any more and game-data is not complete: '
                                'download or extract it again' % source)
    part = _fresh_part(final, log)
    facts = _stage(source, work, part, log)
    if os.path.exists(final):
        rmtree(final)  # an older run's, without default.xex
    replace_dir(part, final)
    return facts


def _stage(source, work, data, log):
    facts = {}
    root = source
    # An archive is extracted into archive/, which a later run reads without it (stage_game).
    if source.lower().endswith(ARCHIVES) and (os.path.isfile(source) or
                                              os.path.isdir(os.path.join(work, 'archive'))):
        root = os.path.join(work, 'archive')
        if not os.path.isdir(root) or not os.listdir(root):
            part = _fresh_part(root, log)
            extract_archive(source, part, log)
            if os.path.exists(root):
                rmtree(root)
            replace_dir(part, root)
    # A disc game: its image, given or in the archive (xiso.py).
    image = root if os.path.isfile(root) and root.lower().endswith(IMAGES) else None
    if image is None and os.path.isdir(root) and not find_xex(root):
        image = next((os.path.join(d, f) for d, _, fs in os.walk(root) for f in sorted(fs)
                      if f.lower().endswith(IMAGES)), None)
    if image and xiso.is_image(image):
        log('  disc image %s' % os.path.basename(image))
        with xiso.Image(image) as im:
            im.extract(data, log=log)
        facts['from'] = 'disc image'
        if not entry_xex(data):
            raise RuntimeError('no default.xex in the disc image %s' % image)
        return facts
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
            titles.append({'package': rel_or_abs(p, work), 'title_id': '%08X' % pkg.title_id,
                           'type': kind, 'name': pkg.display_name})
            if pkg.content_type != 0x000D0000:
                continue  # title updates and DLC are not the game's executable
            log('  STFS %s: %s (%s)' % (os.path.basename(p), pkg.display_name, kind))
            n = pkg.extract(data, progress=log)
            log('  %d files' % n)
        finally:
            pkg.close()
    facts['packages'] = titles
    if not entry_xex(data):
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


def not_starts(work):
    """Addresses found not to be function starts (not-starts.txt): hints a codegen run showed
    to split a function (_false_starts), which find_functions must not add again."""
    p = os.path.join(work, 'not-starts.txt')
    if not os.path.exists(p):
        return set()
    return {int(line.split('#')[0].split()[0], 16) for line in open(p, encoding='utf-8')
            if line.split('#')[0].split()} - tail_starts(work)


def tail_starts(work):
    """Epilogues kept as functions of their own (tail-starts.txt, written by _tail_epilogues):
    no hint check removes them, and not_starts leaves them out."""
    p = os.path.join(work, 'tail-starts.txt')
    if not os.path.exists(p):
        return set()
    return {int(line.split('#')[0].split()[0], 16) for line in open(p, encoding='utf-8')
            if line.split('#')[0].split()}


def _tail_epilogues(pairs, starts, known_false, kept):
    """Targets found not to be function starts that a `b` still reaches over other functions:
    the epilogue of a function the compiler placed after code of its own that .pdata gives
    functions of their own (Forza Motorsport 2's C++ catch blocks: sub_8276BF28 ends at 8276C2C8
    with `b 0x8276C388`, its epilogue past the catch blocks 8276C2CC and 8276C328). Codegen
    cannot grow the function over them, so the epilogue goes back as a function: its share of the
    caller's registers (r31, the frame pointer it restores the stack from) comes from
    live_in_nonvolatiles, as any function's that reads registers its caller set."""
    starts = sorted(starts)
    out = set()
    for t, src in pairs:
        if t not in known_false or t in kept or t <= src:
            continue
        i = bisect.bisect_right(starts, src)
        if i < len(starts) and starts[i] < t:
            out.add(t)
    return out


def _false_starts(text, hints):
    """Hints that split a function: codegen reports a conditional branch from one function to
    an address in no function (and writes a REX_FATAL there) when a hint between the two made
    the code after it a function of its own. Brothers - A Tale of Two Sons: hint 82F62ED4 (one
    find_functions took for a start) cut 82F62E68's loop, whose `bne` back to 82F62EA4 ended
    the game at its first frames. Returns those hints: between the branch's target and it."""
    bad = set()
    for x, y in re.findall(r'Unresolved conditional branch to (0x[0-9A-Fa-f]+) from '
                           r'(0x[0-9A-Fa-f]+)', text):
        lo, hi = sorted((int(x, 16), int(y, 16)))
        bad |= {h for h in hints if lo < h <= hi}
    return bad


def _function_table_base(work):
    """Where the recompiled code's function table goes when a title update makes the image
    smaller: codegen puts it right after the patched image, but the runtime loads the base XEX
    first, at its own size, and that range must be free (Minecraft's TU 80: image 0x2720000, base
    0x2B50000, "XEX image range 82000000-84B50000 is in use"). After the larger of the two then;
    None when the update does not shrink the image (codegen's own choice stands)."""
    data = os.path.join(work, 'game-data')
    names = {n.lower(): n for n in os.listdir(data)} if os.path.isdir(data) else {}
    if 'default.xex' not in names or 'default.xexp' not in names:
        return None
    try:
        base, base_alloc = xex.image_span(os.path.join(data, names['default.xex']))
        _, patched = xex.image_span(os.path.join(data, names['default.xexp']))
    except (OSError, ValueError, struct.error):
        return None
    if base_alloc <= patched:
        return None
    return base + ((base_alloc + 0xFFFF) & ~0xFFFF)


# The runtime allocates every binary's function table in the 0x80000000 heap (64 KiB pages):
# (code size + a 64 KiB thunk reserve) * 2 bytes, right after the image unless the manifest says
# where (function_table_base).
TABLE_HEAP = (0x80000000, 0x90000000)


def _module_table_bases(work, mods):
    """{module index: function table base} for the DLL modules loaded outside the 0x80000000 heap:
    their table cannot follow their image there (Renegade Ops' FragmentBundle_F.dll at 0x98000000,
    4 KiB pages: "AllocFixed invalid base alignment: base=98019000", its functions never
    registered). Each goes after the images and tables of the game's binaries in that heap, a
    table counted at its largest (image size + reserve) * 2. Modules in that heap keep codegen's
    own choice."""
    lo, hi = TABLE_HEAP
    away = [i for i, m in enumerate(mods) if not lo <= m['span'][0] < hi]
    if not away:
        return {}
    spans = []
    path = entry_xex(os.path.join(work, 'game-data'))
    if path:
        try:
            base, size = xex.image_span(path)
            table = _function_table_base(work) or base + size
            spans.append((base, size, table))
        except (OSError, ValueError, struct.error):
            pass
    spans += [(m['span'][0], m['span'][1], m['span'][0] + m['span'][1])
              for m in mods if lo <= m['span'][0] < hi]
    end = max([lo] + [max(b + s, t + (s + 0x10000) * 2) for b, s, t in spans])
    out = {}
    for i in away:
        end = (end + 0xFFFF) & ~0xFFFF
        out[i] = end
        end += (mods[i]['span'][1] + 0x10000) * 2
    return out


def _shares(work):
    """Functions codegen must not localize the non-volatile registers of (shares.txt, written by
    _add_shares): their entries get `share_registers = true`."""
    return set(_share_regs(work))


def _share_regs(work):
    """shares.txt as {address: the registers the function takes from its caller}, from each
    line's comment (an older line cut its list at eight with "...": those eight)."""
    p = os.path.join(work, 'shares.txt')
    if not os.path.exists(p):
        return {}
    out = {}
    for line in open(p, encoding='utf-8'):
        code, _, comment = line.partition('#')
        if code.split():
            out[int(code.split()[0], 16)] = {r for r in comment.split() if r != '...'}
    return out


_LOCAL_DECL = re.compile(r'^\s*PPC(?:V|FP)?Register\s+([rfv]\d+)\{\};')
# Instructions whose result does not depend on the register they name as both sources: x - x,
# x ^ x and the like. `subfe r29,r19,r19` is ~r19 + r19 + CA, the carry alone (Minecraft's
# compressed block storage, sub_82563428), and reads nothing from a caller.
_SELF_IDIOM = re.compile(r'^(subf|subfc|subfe|xor|andc|eqv|orc|vxor|vxor128|vandc|vandc128|'
                         r'vsubu[bhw]m)\.?$')


def _idioms_only(asm, reg):
    """Whether the PPC instructions in `asm` that name `reg` all name it as both sources of a
    _SELF_IDIOM (True only when there is at least one)."""
    seen = False
    for ins in asm:
        parts = ins.split(None, 1)
        if len(parts) < 2:
            continue
        ops = [o.strip() for o in parts[1].split(',')]
        if not any(o == reg or o.endswith('(%s)' % reg) for o in ops):
            continue
        if not (_SELF_IDIOM.match(parts[0]) and len(ops) == 3 and ops[1] == ops[2] == reg):
            return False
        seen = True
    return seen


def live_in_nonvolatiles(gen, shared=None):
    """Generated functions that read a non-volatile register they never write, {address: names}.
    ReXGlue (non_volatile_as_local) keeps r14-r31, f14-f31, v14-v31 and v64-v127 as locals of each
    function, zero at its start, on the ABI's word that a function does not take them from its
    caller; code that does (an internal helper the 360 compiler gave register arguments, a
    function split from the one it belongs to) reads zero. Fable II's animation decoder
    (sub_822340C8) reads the constants its caller sub_822E16A8 keeps in v77-v94: every rotation
    key decoded as zero, the camera following the hero turned NaN and the world drew black. The
    XDK's video decoder (XMV: WMV3, VC-1) passes v64, v65, v69 and v72 to its helpers the same
    way: Jet Set Radio's, Syndicate's and Dark Souls II's videos decoded as green and magenta
    bands. A function given `share_registers` keeps them in ctx, and its callers copy their
    locals there around each call.

    `shared` ({address: registers}, shares.txt): the functions that have it already. A register a
    shared function takes also has to reach it through the functions between it and the one that
    sets it: one with locals that names no such register copies nothing of it into ctx for the
    call (Halo: Combat Evolved Anniversary's sub_833D0F68 sets v92 and calls sub_833D08B8, which
    names no v92 and calls the shared sub_833D7BB8, which reads it). Those functions take the
    register from their caller too: worked out here to the end, over the direct calls (an
    indirect call gives no callee). Returns {address: registers} for the functions to share and
    for the shared ones whose list grew."""
    shared = shared or {}
    locs, own, calls = {}, {}, {}
    # A copy back from ctx after a call into a shared function (`r28 = ctx.r28;`) is no write of
    # the function's own: counted as one, it hid a register the function reads before it sets it.
    copy_back = re.compile(r'^\s*([rfv]\d+) = ctx\.\1;\s*$')
    call = re.compile(r'(?<![\w])sub_([0-9A-Fa-f]{8})\(ctx, base\)')
    for path in glob.glob(os.path.join(gen, '*_recomp.*.cpp')):
        name, body = None, []

        def check():
            if not name:
                return
            a = int(name, 16)
            names = [m.group(1) for line in body for m in [_LOCAL_DECL.match(line)] if m]
            locs[a] = set(names)
            lines = [line for line in body
                     if not _LOCAL_DECL.match(line) and not line.lstrip().startswith('//')]
            text = '\n'.join(lines)
            calls[a] = {int(c, 16) for c in call.findall(text)}
            if not names:
                return
            writes = '\n'.join(line for line in lines if not copy_back.match(line))
            asm = [line.strip()[2:].strip() for line in body if line.lstrip().startswith('//')]
            regs = set()
            for r in names:
                if not re.search(r'\b%s\b' % r, text):
                    continue
                written = (re.search(r'\(simde__m128i?\*\)%s\.\w+,' % r, writes) or
                           re.search(r'store_ps\(%s\.' % r, writes) or
                           re.search(r'(?<![\w.])%s(\.\w+(\[\d+\])?)?\s*=[^=]' % r, writes))
                if not written and not _idioms_only(asm, r):
                    regs.add(r)
            if regs:
                own[a] = regs

        with open(path, encoding='utf-8', errors='replace') as f:
            for line in f:
                m = re.match(r'DEFINE_REX_FUNC\(sub_([0-9A-Fa-f]{8})\)', line)
                if m:
                    check()
                    name, body = m.group(1), []
                else:
                    body.append(line.rstrip('\n'))
        check()
    # What each function takes from its caller: its own, and what the shared functions it calls
    # (or will) take and it has no local for.
    needs = {a: set(r) for a, r in shared.items()}
    for a, r in own.items():
        needs.setdefault(a, set()).update(r)
    callers = {}
    for a, cs in calls.items():
        for c in cs:
            callers.setdefault(c, set()).add(a)
    todo = [a for a in needs if needs[a]]
    while todo:
        c = todo.pop()
        for a in callers.get(c, ()):
            more = {r for r in needs[c] if r not in locs.get(a, ())} - needs.get(a, set())
            if more:
                needs.setdefault(a, set()).update(more)
                todo.append(a)
    return {a: sorted(r, key=_reg_order) for a, r in needs.items()
            if r and (a not in shared or r - shared[a])}


def _reg_order(reg):
    return ('rfv'.index(reg[0]), int(reg[1:]))


def _add_shares(work, found, log, rnd):
    """Adds the functions live_in_nonvolatiles found to shares.txt, and the registers it found
    for functions there already (each line's whole list); True when a function was new (codegen
    has to run again)."""
    have = _share_regs(work)
    new = {a: r for a, r in found.items() if a not in have}
    grown = {a: r for a, r in found.items() if a in have and set(r) - have[a]}
    if not new and not grown:
        return False
    merged = dict(have)
    for a, r in list(new.items()) + list(grown.items()):
        merged[a] = set(merged.get(a, ())) | set(r)
    with open(os.path.join(work, 'shares.txt'), 'w', encoding='utf-8', newline='\n') as f:
        for a in sorted(merged):
            f.write('0x%08X  # %s\n' % (a, ' '.join(sorted(merged[a], key=_reg_order))))
    if grown:
        log('  codegen round %d: %d shared functions take more registers through their callees'
            % (rnd, len(grown)))
    if not new:
        return False
    names = ' '.join('%08X' % a for a in sorted(new)[:12])
    log('  codegen round %d: %d functions read non-volatile registers their caller set, given '
        'share_registers: %s%s' % (rnd, len(new), names, ' ...' if len(new) > 12 else ''))
    return True


def _shares_scanned(work, gen, mark=False):
    """Whether live_in_nonvolatiles already ran on this generated code (shares.scanned holds the
    newest generated file's mtime); with mark, records that it did."""
    files = glob.glob(os.path.join(gen, '*_recomp.*.cpp'))
    newest = '%d' % max((os.path.getmtime(f) for f in files), default=0)
    stamp = os.path.join(work, 'shares.scanned')
    if mark:
        with open(stamp, 'w', encoding='utf-8') as f:
            f.write(newest)
        return True
    return os.path.exists(stamp) and open(stamp, encoding='utf-8').read().strip() == newest


# The rules of functions.jump_tables, counted: a new rule makes every game's generated code
# looked at again (tables.scanned). 2: the li + addis tables (their index overwritten before the
# bctr), the bound checks on cr0, and the DLL modules' code with their own images. 3: a far bound
# check whose index register the table load itself overwrites. 4: a far bound check on a register
# spilled to the stack, the index reloaded from it before the bctr.
TABLE_RULES = 4


def module_image(work, module):
    """The loaded image of a DLL module (its generated folder's name), which
    --rexnative_dump_image writes beside run/image.bin."""
    return os.path.join(work, 'run', 'image.%s.bin' % module)


def table_sources(work):
    """[(binary, generated folder, loaded image)] that functions.jump_tables can read: default.xex
    with run/image.bin, each DLL module with run/image.<module>.bin, when there."""
    out = []
    image = os.path.join(work, 'run', 'image.bin')
    gen = os.path.join(work, 'generated', 'default')
    if os.path.exists(image) and os.path.isdir(gen):
        out.append(('default', gen, image))
    for m in modules(work):
        ident = m['out'].split('/')[-1]
        gen = os.path.join(work, *m['out'].split('/'))
        image = module_image(work, ident)
        if os.path.exists(image) and os.path.isdir(gen):
            out.append((ident, gen, image))
    return out


def _tables_scanned(work, gen, mark=False):
    """Whether functions.jump_tables, as its rules are now (TABLE_RULES), already looked at this
    generated code (tables.scanned holds the rules' count, the newest generated file's mtime and
    the binaries it had images of); with mark, records that it did."""
    sources = table_sources(work)
    files = [f for _, g, _ in sources for f in glob.glob(os.path.join(g, '*_recomp.*.cpp'))]
    newest = '%d %d' % (TABLE_RULES, max((os.path.getmtime(f) for f in files), default=0))
    if len(sources) > 1:
        newest += ' ' + ','.join(b for b, _, _ in sources)
    stamp = os.path.join(work, 'tables.scanned')
    if mark:
        with open(stamp, 'w', encoding='utf-8') as f:
            f.write(newest)
        return True
    return os.path.exists(stamp) and open(stamp, encoding='utf-8').read().strip() == newest


def _add_switch_tables(work, gen, log, rnd):
    """The jump tables codegen cut short in this generated code, added to switch_tables.toml
    (functions.jump_tables, project.write_switch_tables); True when there are new ones, for
    codegen to run again. Only with the loaded image (run/image.bin, which `project
    --find-functions` dumps), and once per generated tree. find_functions looks for them once,
    over the code before its own hints: a table can show as cut short only in a later tree, and
    a game prepared before a rule was learned never had it applied (Crazy Taxi, prepared before
    the no-bound-check rule: its switch of 699 streamed sound names at 82229474, bounded by its
    caller only, read as 512 entries; sound 673 trapped, ILLEGAL_INSTRUCTION, when ARCADE was
    chosen; Spelunky's 534-entry 820ED328 the same). A DLL module's code is read with its own
    image (run/image.<module>.bin), into switch_tables.<module>.toml."""
    if not table_sources(work) or _tables_scanned(work, gen):
        return False
    from . import project
    new = project.find_switch_tables(work)
    _tables_scanned(work, gen, True)
    if not new:
        return False
    log('  codegen round %d: %d jump tables codegen cut short, given whole: %s' % (
        rnd, len(new), ' '.join('%08X (%d)' % (a, n) for a, n in new[:8])))
    return True


# XEX module flags (the header's second word): a DLL module, and a patch (an .xexp's).
XEX_MODULE_DLL, XEX_MODULE_PATCH = 0x8, 0x10


def modules(work):
    """The game's XEX DLL modules, which ReXGlue recompiles as [[modules]] beside the entrypoint:
    every .xex or .dll under game-data but default.xex whose header is XEX2 and whose module flags
    say DLL (Forza Horizon's SpeechFacade_default.xex and XMediaFacade_default.xex; Renegade Ops
    names its own FragmentBundle_F.dll and three more, loaded at start-up). Each as a dict: file (relative to
    work), guest_path (relative to the game's root, which the guest loads it by), out (its
    generated folder) and span (its image's base address and size)."""
    root = os.path.join(work, 'game-data')
    out = []
    for dirpath, _, files in os.walk(root):
        for f in sorted(files):
            rel = os.path.relpath(os.path.join(dirpath, f), root).replace(os.sep, '/')
            if not f.lower().endswith(('.xex', '.dll')) or rel.lower() == 'default.xex':
                continue
            path = os.path.join(dirpath, f)
            with open(path, 'rb') as fh:
                head = fh.read(8)
            if head[:4] != b'XEX2':
                continue
            flags = struct.unpack('>I', head[4:8])[0]
            if not flags & XEX_MODULE_DLL or flags & XEX_MODULE_PATCH:
                continue
            ident = re.sub(r'[. ]', '_', os.path.splitext(f)[0].lower())
            # A title update's patch beside it (codegen applies it) can make the image larger.
            base, size = xex.image_span(path)
            if os.path.exists(path + 'p'):
                size = max(size, xex.image_span(path + 'p')[1])
            out.append({'file': 'game-data/' + rel, 'guest_path': rel,
                        'out': 'generated/' + ident, 'span': (base, size)})
    return sorted(out, key=lambda m: m['guest_path'].lower())


def _generated_dirs(work):
    """The generated code of the entrypoint and of each DLL module."""
    return [os.path.join(work, 'generated', 'default')] + [
        os.path.join(work, *m['out'].split('/')) for m in modules(work)]


def _scan_shares(work, gen):
    """live_in_nonvolatiles over the entrypoint's generated code and every module's."""
    found = {}
    regs = _share_regs(work)
    for d in [gen] + _generated_dirs(work)[1:]:
        if os.path.isdir(d):
            found.update(live_in_nonvolatiles(d, regs))
    return found


def _function_lines(entries):
    """[(address, size, shared)] as a functions table's lines."""
    lines = []
    for a, size, shared in sorted(entries):
        fields = []
        if size:
            fields.append('size = 0x%X' % size)
        if shared:
            fields.append('share_registers = true')
        lines.append('0x%08X = { %s }' % (a, ', '.join(fields)) if fields else '0x%08X = {}' % a)
    return lines


def _write_manifest(work, name):
    lines = ['# ReXGlue codegen manifest written by ReXNative (rexnative prepare): edit hints.txt,',
             '# not this file.', '', '[project]', 'name = "%s"' % name, 'sdk_version = "0.11.0"',
             'game_root = "game-data"', '', '[entrypoint]', 'file_path = "game-data/%s"' % os.path.basename(entry_xex(os.path.join(work, 'game-data')) or 'default.xex'),
             'out_directory_path = "generated/default"']
    # Jump tables codegen cut short, given whole ([[switch_tables]]), the setjmp / longjmp the
    # game calls (setjmp_address, longjmp_address) and its XAPI fiber functions ([shim]), all
    # from `rexnative project --find-functions` (functions.jump_tables, .setjmp_longjmp,
    # .xapi_fibers).
    # And a game's own code hooks ([[midasm_hook]], title_hooks.py) for its executable.
    includes = [f for f in ('switch_tables.toml', 'setjmp.toml', 'crt.toml', 'title_hooks.toml')
                if os.path.exists(os.path.join(work, f))]
    lines.append('includes = [%s]' % ', '.join('"%s"' % f for f in includes))
    opts = options(work)
    lines += ['%s = %s' % (k, str(v).lower()) for k, v in opts.items()]
    table = _function_table_base(work)
    if table:
        lines.append('function_table_base = 0x%08X' % table)
    hints = _hints(work)
    # A hint inside a sized one (a function grown over its pieces) would overlap it, which
    # codegen refuses ("Overlapping boundaries"): the piece goes.
    spans = [(a, a + s) for a, s in hints.items() if s]
    hints = {a: s for a, s in hints.items() if not any(lo < a < hi for lo, hi in spans)}
    # Not a start found not to be one (not-starts.txt): the function it was split from gets the
    # code back, and with it the registers. Nor a piece a sized hint grew over: its entry would
    # overlap that function as a hint's would (Forza Horizon: 8276161C, shared while it was a
    # function, then inside 827611D8's 0x720 bytes).
    shares = {a for a in _shares(work) - not_starts(work)
              if not any(lo < a < hi for lo, hi in spans)}
    # The XEX DLL modules (ReXGlue's [[modules]]): each with the entrypoint's register options,
    # and the hints and shared functions inside its image (codegen names an address, not its
    # binary).
    mods = modules(work)
    entries = {i: [] for i in range(len(mods) + 1)}
    for a in sorted(set(hints) | shares):
        owner = next((i + 1 for i, m in enumerate(mods)
                      if m['span'][0] <= a < m['span'][0] + m['span'][1]), 0)
        entries[owner].append((a, hints.get(a, 0), a in shares))
    if entries[0]:
        lines.append('[entrypoint.functions]')
        lines += _function_lines(entries[0])
    tables_at = _module_table_bases(work, mods)
    for i, m in enumerate(mods):
        lines += ['', '[[modules]]', 'file_path = "%s"' % m['file'],
                  'guest_path = "%s"' % m['guest_path'],
                  'out_directory_path = "%s"' % m['out']]
        # Its jump tables (project.write_switch_tables): an include of the entrypoint's would
        # hold addresses outside the module's image.
        tables = 'switch_tables.%s.toml' % m['out'].split('/')[-1]
        lines.append('includes = [%s]' % ('"%s"' % tables if os.path.exists(
            os.path.join(work, tables)) else ''))
        lines += ['%s = %s' % (k, str(v).lower()) for k, v in opts.items()]
        if i in tables_at:
            lines.append('function_table_base = 0x%08X' % tables_at[i])
        if entries[i + 1]:
            lines.append('[modules.functions]')
            lines += _function_lines(entries[i + 1])
    # The game's exact vector math (vector-math.txt), for the executable and its modules: a
    # top-level table, which codegen shares with them. It stays last (_options_differ reads it).
    lines += _vector_math_lines(work)
    path = os.path.join(work, '%s_manifest.toml' % name)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    return path


# A function's first instruction that only an epilogue has: the stack pointer back from the frame
# pointer or the stack, the return address reloaded, r31 restored.
EPILOGUE_START = re.compile(r'^\t// (addi r1,r31,\d+|addi r1,r1,\d+|lwz r1,0\(r1\)|'
                            r'lwz r12,-8\(r1\)|ld r31,-16\(r1\))\s*$')


def _epilogue_starts(work, hints):
    """Hints whose generated function begins with an epilogue: the tail of the function before
    it, which branches there, split off. Called as a function of its own, it restores the stack
    from a frame pointer that is the caller's local (ReXGlue v0.11 keeps non-volatile registers
    as locals): Bionic Commando Rearmed's 821D36E0, the epilogue of 821D36A0, crashed it at
    start-up."""
    gen = os.path.join(work, 'generated', 'default')
    found = set()
    if not hints or not os.path.isdir(gen):
        return found
    hints = set(hints) - tail_starts(work)
    for name in os.listdir(gen):
        if not name.endswith('.cpp'):
            continue
        cur = None
        first = []

        def judge():
            # Up to 8 instructions, or to the first branch, or to the end of a shorter function:
            # Midnight Club's 821322B0 is two loads from r27 and r16 that fall through into
            # 821322B8 (no branch, so it was never judged, and its build failed on sub_821322B8).
            if cur is not None and first and _reads_saved_register(first):
                found.add(cur)

        with open(os.path.join(gen, name), encoding='utf-8', errors='replace') as f:
            for line in f:
                if line.startswith('DEFINE_REX_FUNC(sub_'):
                    judge()
                    cur = int(line[20:28], 16)
                    first = []
                    if cur not in hints:
                        cur = None
                elif cur is not None and line.startswith('	// '):
                    first.append(line[4:].strip())
                    if len(first) == 1 and EPILOGUE_START.match(line):
                        found.add(cur)
                        cur = None
                    elif len(first) == 8 or first[-1].split()[0].startswith('b'):
                        judge()
                        cur = None
                elif cur is not None and line.startswith('}'):
                    judge()
                    cur = None
            judge()
    return found


NON_VOLATILE = {'r%d' % i for i in range(14, 32)}
_GPR = re.compile(r'r\d+')


def _reads_saved_register(instructions):
    """Whether code reads a non-volatile register (r14-r31) before it writes it: the caller's value,
    which no function start uses, so the code is the rest of the function before it (Bionic
    Commando Rearmed's 821DBF10, `lwz r11,0(r29)`, and 822BA4C8, `addi r3,r31,128`). Storing one,
    as a prologue does, is not a use."""
    written = set()
    for text in instructions:
        parts = text.replace(',', ' ').replace('(', ' ').replace(')', ' ').split()
        if not parts:
            continue
        op, operands = parts[0], parts[1:]
        if op.startswith('b'):
            return False
        if op.startswith('st'):
            # The register stored is saved, not used (a prologue, or a context save such as
            # SwitchToFiber's: Double Dragon Neon's 826250F0); only the address registers count
            # (`stfs f0,12(r31)` reads r31).
            sources, dest = [p for p in operands[1:] if _GPR.fullmatch(p)], None
        elif op.startswith(('cmp', 'tw', 'td', 'mt', 'dcb', 'icb')):
            sources, dest = [p for p in operands if _GPR.fullmatch(p)], None
        else:
            # The first operand is written when it is a GPR (`lfs f1,0(r31)` writes f1 and reads
            # r31); rlwimi and rldimi also read it.
            dest = operands[0] if operands and _GPR.fullmatch(operands[0]) else None
            sources = [p for p in operands[1:] if _GPR.fullmatch(p)]
            if dest and op.startswith(('rlwimi', 'rldimi')):
                sources.append(dest)
        if any(r in NON_VOLATILE and r not in written for r in sources):
            return True
        if dest:
            written.add(dest)
    return False


def _generated_starts(gen):
    """The function starts of the generated code in `gen` (its DEFINE_REX_FUNC lines), or none."""
    starts = set()
    for path in glob.glob(os.path.join(gen, '*_recomp.*.cpp')):
        with open(path, encoding='utf-8', errors='replace') as f:
            for line in f:
                if line.startswith('DEFINE_REX_FUNC(sub_'):
                    m = re.match(r'DEFINE_REX_FUNC\(sub_([0-9A-Fa-f]{8})\)', line)
                    if m:
                        starts.add(int(m.group(1), 16))
    return starts


def drop_outside_functions(work, text, log=print):
    """Functions codegen says are in no code region (Deus Ex: Human Revolution's sub_82C63678, a
    data word read as `b 0x8434A9EC`, past the image's end): codegen writes a stub for each and
    lists it in the function table, and the runtime then refuses the whole table ("func_mappings
    entries were rejected": the game does not start). Their entries go from the table; the stubs
    stay for the calls that name them (data, never run). Only the addresses the runtime refuses,
    those outside the code range (REX_CODE_BASE to + REX_CODE_SIZE + REX_THUNK_RESERVE_SIZE, as
    GuestAddressSpace::SetFunction checks): Fable II has 24 hinted functions codegen also calls
    "in no code region" (found running the game, inside that range), which the runtime takes and
    its indirect calls need. Returns the addresses dropped."""
    outside = {int(a, 16) for a in re.findall(
        r'function (0x[0-9A-Fa-f]+) not in any code region', text)}
    gen = os.path.join(work, 'generated', 'default')
    defs = {}
    for path in glob.glob(os.path.join(gen, '*_pch.h')):
        for m in re.finditer(r'#define (REX_CODE_BASE|REX_CODE_SIZE|REX_THUNK_RESERVE_SIZE) '
                             r'(0x[0-9A-Fa-f]+)', open(path, encoding='utf-8', errors='replace').read()):
            defs[m.group(1)] = int(m.group(2), 16)
    if 'REX_CODE_BASE' in defs and 'REX_CODE_SIZE' in defs:
        lo = defs['REX_CODE_BASE']
        hi = lo + defs['REX_CODE_SIZE'] + defs.get('REX_THUNK_RESERVE_SIZE', 0)
        outside = {a for a in outside if not lo <= a < hi}
    if not outside:
        return set()
    pats = [re.compile(r'^\s*\{\s*0x([0-9A-Fa-f]{8}),\s*sub_[0-9A-Fa-f]{8}\s*\},\s*$'),
            re.compile(r'^\s*registrar->SetFunction\(0x([0-9A-Fa-f]{8}),')]
    for path in glob.glob(os.path.join(gen, '*_init.cpp')) + glob.glob(os.path.join(gen, '*_register.cpp')):
        with open(path, encoding='utf-8', errors='replace') as f:
            lines = f.read().split('\n')
        kept = [l for l in lines
                if not any((m := pt.match(l)) and int(m.group(1), 16) in outside for pt in pats)]
        if len(kept) != len(lines):
            with open(path, 'w', encoding='utf-8', newline='\n') as f:
                f.write('\n'.join(kept))
    log('  codegen: %d functions in no code region left out of the function table: %s' % (
        len(outside), ' '.join('%08X' % a for a in sorted(outside))))
    return outside


_TOOL_IDS = {}


def _tool_id(rexglue):
    """The codegen tool's identity for codegen.ok: a hash of its executable. Its --version names
    the SDK's last tagged commit, the same for two builds of a patched SDK (patch 0034 changed
    what codegen writes, not its version)."""
    if rexglue not in _TOOL_IDS:
        import hashlib
        try:
            with open(rexglue, 'rb') as f:
                _TOOL_IDS[rexglue] = hashlib.sha1(f.read()).hexdigest()[:16]
        except OSError:
            _TOOL_IDS[rexglue] = ''
    return _TOOL_IDS[rexglue]


def _made_by(done, rexglue):
    """Whether codegen.ok says the code was generated by this codegen tool. Code from another one
    is generated again here, whole: left to the build, the SDK's codegen step would write it again
    on its own (its fingerprint holds the SDK's version), without drop_outside_functions."""
    try:
        text = open(done, encoding='utf-8').read()
    except OSError:
        return False
    tool = _tool_id(rexglue)
    return bool(tool) and ('rexglue %s' % tool) in text


def codegen(work, name, rexglue, max_rounds, log):
    from . import fps_boost
    gen = os.path.join(work, 'generated', 'default')
    done = os.path.join(work, 'codegen.ok')
    # FPS Boost (fps_boost.py): the patch file beside the executable as the game's choice says,
    # before codegen loads the executable with it.
    try:
        patches = fps_boost.sync(work, log)
    except Exception as e:  # never a reason for a build to fail
        log('  FPS Boost: %s' % e)
        patches = fps_boost.fingerprint(work)
    # The game's own code hooks (title_hooks.py), before codegen too: new or changed ones are
    # in the code it writes, so it runs again.
    from . import title_hooks
    try:
        if title_hooks.apply(work, log) and os.path.exists(done):
            os.remove(done)
    except Exception as e:  # never a reason for a build to fail
        log('  title hooks: %s' % e)
    # Reused only when made under this name (prepare's older runs used names with '_'), by this
    # codegen tool, over the same patch file.
    if (os.path.exists(done) and os.path.isdir(gen) and _made_by(done, rexglue) and
            os.path.exists(os.path.join(gen, '%s_init.h' % name)) and
            not _options_differ(work, gen) and fps_boost.built_with(work) == patches):
        again = (options(work)['non_volatile_as_local'] and not _shares_scanned(work, gen) and
                 _add_shares(work, _scan_shares(work, gen), log, 0))
        if not again:
            _shares_scanned(work, gen, True)
            # Generated code from before a rule of functions.jump_tables gets its tables too.
            again = _add_switch_tables(work, gen, log, 0)
        if not again:
            return {'reused': True, 'rounds': 0}
        os.remove(done)
    for old in os.listdir(work):
        if old.endswith('_manifest.toml') and old != '%s_manifest.toml' % name:
            os.remove(os.path.join(work, old))
    if os.path.exists(done):
        # Made under another name or by another codegen tool: codegen writes it again (the hints
        # stay); a generated/rexglue.cmake of the old SDK goes (v0.11 writes
        # generated/sources.cmake).
        os.remove(done)
        stale = os.path.join(work, 'generated', 'rexglue.cmake')
        if os.path.exists(stale):
            os.remove(stale)
    # Codegen skips an unchanged module, and its warnings with it (unresolved tail calls and
    # branches, which the rounds below act on): it runs whole.
    stamp = os.path.join(gen, 'codegen.stamp')
    if os.path.exists(stamp):
        os.remove(stamp)
    for rnd in range(max_rounds):
        manifest = _write_manifest(work, name)
        t0 = time.time()
        r = _run([rexglue, 'codegen', os.path.basename(manifest)], cwd=work)
        spent('codegen', time.time() - t0)
        text = r.stdout + r.stderr
        with open(os.path.join(work, 'codegen.log'), 'w', encoding='utf-8') as f:
            f.write(text)
        hints = _hints(work)
        # A tail call (b) to an address in no function: codegen 0.10 warns and still succeeds,
        # writing a call with no name that does not compile (Orcs Must Die!'s
        # `b 0x824BDB18`). Its target is a function start, as a failed run's targets are.
        tails = [int(t, 16) for t in re.findall(r'Unresolved b target (0x[0-9A-Fa-f]+) from', text)]
        tails = [t for t in tails if t not in hints]
        false = _false_starts(text, hints)
        # A hint at a millicode helper (__savegprlr_N and the rest): ReXGlue v0.11 finds every
        # copy of the helpers (a game can link them twice, as Jet Set Radio does) and refuses a
        # config function there; a hint from before is dropped.
        # Codegen stops at the first one: the hints around it go too (a helper run is 18 to 64
        # entries of 4 or 8 bytes); a real start among them is a target codegen reports again.
        for a in re.findall(
                r'function (0x[0-9A-Fa-f]+) is millicode, which no config entry may replace', text):
            false |= {h for h in hints if abs(h - int(a, 16)) <= 0x200}
        if false:
            for h in false:
                del hints[h]
            with open(os.path.join(work, 'not-starts.txt'), 'a', encoding='utf-8',
                      newline='\n') as f:
                f.write(''.join('0x%08X\n' % h for h in sorted(false)))
            with open(os.path.join(work, 'hints.txt'), 'w', encoding='utf-8', newline='\n') as f:
                f.write(''.join('0x%08X 0x%X\n' % (a, z) for a, z in sorted(hints.items())))
            log('  codegen round %d: %d hints that split a function removed (%.0f s): %s' % (
                rnd, len(false), time.time() - t0, ' '.join('%08X' % h for h in sorted(false))))
            if not tails:
                continue
        if r.returncode == 0 and not tails:
            epilogues = _epilogue_starts(work, hints)
            if epilogues:
                for h in epilogues:
                    del hints[h]
                with open(os.path.join(work, 'not-starts.txt'), 'a', encoding='utf-8',
                          newline='\n') as f:
                    f.write(''.join('0x%08X\n' % h for h in sorted(epilogues)))
                with open(os.path.join(work, 'hints.txt'), 'w', encoding='utf-8',
                          newline='\n') as f:
                    f.write(''.join('0x%08X 0x%X\n' % (a, z) for a, z in sorted(hints.items())))
                log('  codegen round %d: %d hints at an epilogue or inside a function removed '
                    '(%.0f s): %s' % (rnd, len(epilogues), time.time() - t0,
                                      ' '.join('%08X' % h for h in sorted(epilogues))))
                continue
            if options(work)['non_volatile_as_local'] and _add_shares(
                    work, _scan_shares(work, gen), log, rnd):
                continue
            if _add_switch_tables(work, gen, log, rnd):
                continue
            drop_outside_functions(work, text, log)
            _shares_scanned(work, gen, True)
            open(done, 'w').write('round %d\nrexglue %s\npatches %s\n' % (
                rnd, _tool_id(rexglue), patches))
            log('  codegen: done (round %d, %.0f s)' % (rnd, time.time() - t0))
            return {'rounds': rnd + 1, 'hints': len(hints)}
        pairs = [(int(t, 16), int(s, 16)) for t, s in re.findall(
            r'^\s+(0x[0-9A-Fa-f]+) from (0x[0-9A-Fa-f]+):', text, re.M)]
        pairs += [(int(t, 16), int(s, 16)) for t, s in re.findall(
            r'Unresolved b target (0x[0-9A-Fa-f]+) from (0x[0-9A-Fa-f]+)', text)]
        # A target found before not to be a function start (not-starts.txt: its hint split a
        # function), asked for again: the hint whose function branches there is code of that
        # function placed out of line, after its return, and not a function of its own. Bionic
        # Commando Rearmed: hint 821DB4F8 (r31 used unset, then `b 0x821DB404`) is
        # sub_821DB258's; the hint codegen asked for at 821DB404 split sub_821DB258's loop, was
        # removed, and was asked for again, round after round. That source hint goes instead.
        # The source's own function is the start nearest below it among every start codegen
        # knows, .pdata's too (from the last generated code): only a hint is blamed, not the hint
        # before a .pdata function, which would go round after round.
        known_false = not_starts(work)
        fragments = set()
        if any(t in known_false for t, _ in pairs):
            starts = sorted(set(hints) | _generated_starts(gen))
            for t, src in pairs:
                if t not in known_false:
                    continue
                i = bisect.bisect_right(starts, src) - 1
                if (i >= 0 and starts[i] in hints and src - starts[i] < 0x200 and
                        starts[i] != t):
                    fragments.add(starts[i])
        targets = [t for t, _ in pairs] + tails
        new = sorted({t for t in targets if t not in hints and t not in known_false})
        if not new and not fragments:
            kept = _tail_epilogues([p for p in pairs if p[0] in tails],
                                   set(hints) | _generated_starts(gen), known_false,
                                   tail_starts(work))
            if kept:
                with open(os.path.join(work, 'tail-starts.txt'), 'a', encoding='utf-8',
                          newline='\n') as f:
                    f.write(''.join('0x%08X\n' % t for t in sorted(kept)))
                log('  codegen round %d: %d epilogues a branch reaches over other functions '
                    'kept as functions: %s' % (rnd, len(kept),
                                               ' '.join('%08X' % t for t in sorted(kept))))
                new = sorted(kept)
            else:
                tail = text.strip().splitlines()[-12:]
                raise RuntimeError('codegen failed with nothing to hint:\n' + '\n'.join(tail))
        for h in fragments:
            del hints[h]
        if fragments:
            with open(os.path.join(work, 'not-starts.txt'), 'a', encoding='utf-8',
                      newline='\n') as f:
                f.write(''.join('0x%08X\n' % h for h in sorted(fragments)))
            log('  codegen round %d: %d hints that branch into another function removed: %s' % (
                rnd, len(fragments), ' '.join('%08X' % h for h in sorted(fragments))))
        for t in new:
            hints[t] = 0
        with open(os.path.join(work, 'hints.txt'), 'w', encoding='utf-8', newline='\n') as f:
            f.write(''.join('0x%08X 0x%X\n' % (a, s) for a, s in sorted(hints.items())))
        if new:
            log('  codegen round %d: +%d hints (%.0f s)' % (rnd, len(new), time.time() - t0))
    raise RuntimeError('codegen still failing after %d rounds' % max_rounds)


def _processes():
    """{pid: (parent pid, command line)} of this machine's processes; {} when they can't be
    listed."""
    out = {}
    try:
        if os.name == 'nt':
            r = subprocess.run(['powershell', '-NoProfile', '-NonInteractive', '-Command',
                                "Get-CimInstance Win32_Process | ForEach-Object { '{0} {1} {2}' "
                                "-f $_.ProcessId, $_.ParentProcessId, $_.CommandLine }"],
                               capture_output=True, text=True, errors='replace', timeout=60)
            for line in r.stdout.splitlines():
                parts = line.split(' ', 2)
                if len(parts) == 3 and parts[0].isdigit() and parts[1].isdigit():
                    out[int(parts[0])] = (int(parts[1]), parts[2])
        elif os.path.isdir('/proc'):
            for d in os.listdir('/proc'):
                if not d.isdigit():
                    continue
                try:
                    cmd = open('/proc/%s/cmdline' % d, 'rb').read().replace(b'\0', b' ').decode(
                        'utf-8', 'replace')
                    ppid = int(open('/proc/%s/stat' % d).read().rsplit(')', 1)[1].split()[1])
                except (OSError, ValueError, IndexError):
                    continue
                out[int(d)] = (ppid, cmd)
    except (OSError, subprocess.SubprocessError):
        return {}
    return out


def _alive(pid):
    if os.name == 'nt':
        return pid in _processes()
    try:
        os.kill(pid, 0)
        return True
    except PermissionError:
        return True
    except OSError:
        return False


def other_prepares(source, name):
    """PIDs of other `rexnative prepare` runs of this game (not this one nor its parents): one an
    app restart orphaned keeps extracting and building in the same work folder, and a second run
    beside it removes the files it writes (WinError 145) and races its codegen and build."""
    procs = _processes()
    mine, pid = set(), os.getpid()
    while pid and pid not in mine:
        mine.add(pid)
        pid = procs.get(pid, (0, ''))[0]
    keys = {os.path.abspath(source).lower()}
    if os.path.isfile(source):
        keys.add(os.path.basename(source).lower())  # the same archive given by another path
    return sorted(p for p, (_, cmd) in procs.items()
                  if p not in mine and 'rexnative' in cmd and ' prepare ' in cmd
                  and any(k in cmd.lower() for k in keys))


def wait_other_prepares(source, name, log, limit=2 * 3600):
    """Waits (up to limit seconds) for other runs preparing this game to end."""
    others = other_prepares(source, name)
    if not others:
        return
    log('  another prepare of %s is running (pid %s): waiting for it to end'
        % (name, ' '.join(map(str, others))))
    t0 = time.time()
    while others and time.time() - t0 < limit:
        time.sleep(30)
        others = [p for p in others if _alive(p)]
    log('  %s (%.0f s)' % ('it ended' if not others else 'still running; going on',
                          time.time() - t0))


def prepare(source, work_root, rexglue, refs, run_adapt, log=print, max_rounds=40,
            title_update=False, dlc=False):
    name = slug(source)
    work = os.path.join(work_root, name)
    os.makedirs(work, exist_ok=True)
    wait_other_prepares(source, name, log)
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
        # The newest title update, before codegen (title_update.py): codegen recompiles the
        # patched image.
        # XboxUnity out of reach (after its retries): recorded, and the game is prepared from
        # the image it has; a later --title-update run installs it and starts codegen again.
        if title_update:
            from . import title_update as tu
            try:
                status['steps']['title_update'] = tu.update(work, log)
            except tu.Unreachable as e:
                status['steps']['title_update'] = {'error': str(e)}
                log('  title update: %s; going on without it' % e)
            save()
        # Its downloadable content from the DLC sources (dlc.py; none set: skipped). Content,
        # not code: a failure (no archive.org login) is recorded and the game goes on.
        if dlc:
            from . import dlc as dlcmod
            try:
                got = dlcmod.fetch(work, log)
                status['steps']['dlc'] = {k: v['installed'] for k, v in got.items()}
            except (SystemExit, Exception) as e:  # content, not code: prepare goes on
                status['steps']['dlc'] = {'error': str(e)}
                log('  DLC: %s' % e)
            save()
        info = xex.read(entry_xex(os.path.join(work, 'game-data')) or os.path.join(work, 'game-data', 'default.xex'))
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
        # The title (the settings menu's): the archive's name, as the Deck's shortcut has it.
        from .deck import title_of
        target = {'name': name, 'title': title_of(work) or name,
                  'xex': entry_xex(os.path.join(work, 'game-data')) or os.path.join(work, 'game-data', 'default.xex'),
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
        status.pop('error', None)  # an earlier run's failure, from the prepare.json it read
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
