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
import bisect
import glob
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time

from . import profile as prof, stfs, xex, xiso

BELOW_NORMAL = 0x00004000  # Windows process priority class
SEVEN_ZIP = [r'C:\Program Files\7-Zip\7z.exe', r'C:\Program Files (x86)\7-Zip\7z.exe', '7z']
ARCHIVES = ('.7z', '.zip', '.rar')
IMAGES = ('.iso',)  # disc images (xiso.py)
OPTIONS = {  # register-as-local codegen, as in PGR4Native, From Dust and Iron Brigade
    'cr_as_local': True, 'ctr_as_local': True, 'xer_as_local': True,
    'non_volatile_as_local': True, 'reserved_as_local': True,
}


def slug(name):
    """'Super Meat Boy (World) (XBLA).7z' -> 'super-meat-boy'."""
    base = os.path.basename(name)
    for ext in ARCHIVES + IMAGES:
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
        if not os.path.exists(os.path.join(data, 'default.xex')):
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


def not_starts(work):
    """Addresses found not to be function starts (not-starts.txt): hints a codegen run showed
    to split a function (_false_starts), which find_functions must not add again."""
    p = os.path.join(work, 'not-starts.txt')
    if not os.path.exists(p):
        return set()
    return {int(line.split('#')[0].split()[0], 16) for line in open(p, encoding='utf-8')
            if line.split('#')[0].split()}


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


def _shares(work):
    """Functions codegen must not localize the non-volatile registers of (shares.txt, written by
    _add_shares): their entries get `share_registers = true`."""
    p = os.path.join(work, 'shares.txt')
    if not os.path.exists(p):
        return set()
    return {int(line.split('#')[0].split()[0], 16) for line in open(p, encoding='utf-8')
            if line.split('#')[0].split()}


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


def live_in_nonvolatiles(gen):
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
    locals there around each call."""
    found = {}
    for path in glob.glob(os.path.join(gen, '*_recomp.*.cpp')):
        name, body = None, []

        def check():
            if not name:
                return
            locs = [m.group(1) for line in body for m in [_LOCAL_DECL.match(line)] if m]
            if not locs:
                return
            text = '\n'.join(line for line in body
                             if not _LOCAL_DECL.match(line) and not line.lstrip().startswith('//'))
            asm = [line.strip()[2:].strip() for line in body if line.lstrip().startswith('//')]
            regs = []
            for r in locs:
                if not re.search(r'\b%s\b' % r, text):
                    continue
                written = (re.search(r'\(simde__m128i?\*\)%s\.\w+,' % r, text) or
                           re.search(r'store_ps\(%s\.' % r, text) or
                           re.search(r'(?<![\w.])%s(\.\w+(\[\d+\])?)?\s*=[^=]' % r, text))
                if not written and not _idioms_only(asm, r):
                    regs.append(r)
            if regs:
                found[int(name, 16)] = regs

        with open(path, encoding='utf-8', errors='replace') as f:
            for line in f:
                m = re.match(r'DEFINE_REX_FUNC\(sub_([0-9A-Fa-f]{8})\)', line)
                if m:
                    check()
                    name, body = m.group(1), []
                else:
                    body.append(line.rstrip('\n'))
        check()
    return found


def _add_shares(work, found, log, rnd):
    """Adds the functions live_in_nonvolatiles found to shares.txt; True when any was new."""
    have = _shares(work)
    new = {a: r for a, r in found.items() if a not in have}
    if not new:
        return False
    with open(os.path.join(work, 'shares.txt'), 'a', encoding='utf-8', newline='\n') as f:
        for a in sorted(new):
            regs = new[a]
            f.write('0x%08X  # %s%s\n' % (a, ' '.join(regs[:8]), ' ...' if len(regs) > 8 else ''))
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


def _write_manifest(work, name):
    lines = ['# ReXGlue codegen manifest written by ReXNative (rexnative prepare): edit hints.txt,',
             '# not this file.', '', '[project]', 'name = "%s"' % name, 'sdk_version = "0.11.0"',
             'game_root = "game-data"', '', '[entrypoint]', 'file_path = "game-data/default.xex"',
             'out_directory_path = "generated/default"']
    # Jump tables codegen cut short, given whole ([[switch_tables]]), the setjmp / longjmp the
    # game calls (setjmp_address, longjmp_address) and its XAPI fiber functions ([shim]), all
    # from `rexnative project --find-functions` (functions.jump_tables, .setjmp_longjmp,
    # .xapi_fibers).
    includes = [f for f in ('switch_tables.toml', 'setjmp.toml', 'crt.toml')
                if os.path.exists(os.path.join(work, f))]
    lines.append('includes = [%s]' % ', '.join('"%s"' % f for f in includes))
    lines += ['%s = %s' % (k, str(v).lower()) for k, v in OPTIONS.items()]
    table = _function_table_base(work)
    if table:
        lines.append('function_table_base = 0x%08X' % table)
    hints = _hints(work)
    # A hint inside a sized one (a function grown over its pieces) would overlap it, which
    # codegen refuses ("Overlapping boundaries"): the piece goes.
    spans = [(a, a + s) for a, s in hints.items() if s]
    hints = {a: s for a, s in hints.items() if not any(lo < a < hi for lo, hi in spans)}
    # Not a start found not to be one (not-starts.txt): the function it was split from gets the
    # code back, and with it the registers.
    shares = _shares(work) - not_starts(work)
    if hints or shares:
        lines.append('[entrypoint.functions]')
        for a in sorted(set(hints) | shares):
            fields = []
            if hints.get(a):
                fields.append('size = 0x%X' % hints[a])
            if a in shares:
                fields.append('share_registers = true')
            lines.append('0x%08X = { %s }' % (a, ', '.join(fields)) if fields else '0x%08X = {}' % a)
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


def codegen(work, name, rexglue, max_rounds, log):
    gen = os.path.join(work, 'generated', 'default')
    done = os.path.join(work, 'codegen.ok')
    # Reused only when made under this name (prepare's older runs used names with '_').
    if (os.path.exists(done) and os.path.isdir(gen) and
            os.path.exists(os.path.join(gen, '%s_init.h' % name))):
        if not (OPTIONS['non_volatile_as_local'] and not _shares_scanned(work, gen) and
                _add_shares(work, live_in_nonvolatiles(gen), log, 0)):
            _shares_scanned(work, gen, True)
            return {'reused': True, 'rounds': 0}
        os.remove(done)
    for old in os.listdir(work):
        if old.endswith('_manifest.toml') and old != '%s_manifest.toml' % name:
            os.remove(os.path.join(work, old))
    if os.path.exists(done):
        # Made under another name: codegen writes it again (the hints stay); a
        # generated/rexglue.cmake of the old SDK goes (v0.11 writes generated/sources.cmake).
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
            log('  codegen round %d: %d hints that split a function removed: %s' % (
                rnd, len(false), ' '.join('%08X' % h for h in sorted(false))))
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
                log('  codegen round %d: %d hints at an epilogue or inside a function removed: %s' % (
                    rnd, len(epilogues), ' '.join('%08X' % h for h in sorted(epilogues))))
                continue
            if OPTIONS['non_volatile_as_local'] and _add_shares(
                    work, live_in_nonvolatiles(gen), log, rnd):
                continue
            drop_outside_functions(work, text, log)
            _shares_scanned(work, gen, True)
            open(done, 'w').write('round %d\n' % rnd)
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


def prepare(source, work_root, rexglue, refs, run_adapt, log=print, max_rounds=40,
            title_update=False, dlc=False):
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
        # The newest title update, before codegen (title_update.py): codegen recompiles the
        # patched image.
        if title_update:
            from . import title_update as tu
            status['steps']['title_update'] = tu.update(work, log)
            save()
        # Its downloadable content from archive.org's collection (dlc.py). Content, not code:
        # a failure (no archive.org login) is recorded and the game goes on.
        if dlc:
            from . import dlc as dlcmod
            try:
                got = dlcmod.fetch(work, log)
                status['steps']['dlc'] = {k: v['installed'] for k, v in got.items()}
            except (SystemExit, OSError, RuntimeError) as e:
                status['steps']['dlc'] = {'error': str(e)}
                log('  DLC: %s' % e)
            save()
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
        # The title (the settings menu's): the archive's name, as the Deck's shortcut has it.
        from .deck import title_of
        target = {'name': name, 'title': title_of(work) or name,
                  'xex': os.path.join(work, 'game-data', 'default.xex'),
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
