"""FPS Boost: a game's frame rate raised by Xenia canary's community patches.

Xenia canary keeps patches for many games (github.com/xenia-canary/game-patches:
patches/<TITLE ID> - <name>.patch.toml), among them "60 FPS" and "Unlock FPS" ones that change how
the game paces itself. Each file is for one executable, named by Xenia canary's module hash
(XXH3-64 over the code pages, its import thunks rewritten; UserModule::CalculateHash). ReXGlue
applies such a file beside the module (game-data/default.xex.patch.toml; SDK patch 0047) once the
module is loaded, for codegen as for the game: a code patch is in the recompiled code, a data
patch in the game's memory.

FPS Boost is the game's choice (fps-boost.json), applied by its next build: sync(), before codegen,
writes the file with the frame rate patch alone (or removes it), and codegen runs again when the
file changed (codegen.ok records it), so a game never runs patched data over code recompiled
without the patch, or the reverse. Nothing is fetched unless a game asks: the repository's file
list is kept a day, each file by its blob hash.

    python -m rexnative fps-boost <work> [on|off] [--refresh]
"""
import hashlib
import json
import os
import re
import struct
import time
import urllib.request

TREE_URL = 'https://api.github.com/repos/xenia-canary/game-patches/git/trees/main?recursive=1'
RAW_URL = 'https://raw.githubusercontent.com/xenia-canary/game-patches/main/%s'
REPO_URL = 'https://github.com/xenia-canary/game-patches/blob/main/%s'
USER_AGENT = 'rexnative/1.0 (+https://github.com/gtguerreiro-max/ReXNative)'
TREE_DAYS = 1
CHOICE = 'fps-boost.json'

# A patch that raises the frame rate: "60 FPS", "60 FPS - Game Speed Fix", "Unlock FPS",
# "Uncapped Framerate"; not one that shows it ("Show FPS"), lowers or locks it.
_FPS = re.compile(r'(\d+)\s*fps|fps\s*(unlock|uncap)|unlock(ed)?\s*(the\s*)?(fps|frame\s*rate)|'
                  r'uncap(ped)?\s*(fps|frame\s*rate)|frame\s*rate\s*(unlock|uncap)', re.I)
_NOT_FPS = re.compile(r'\bshow|counter|display|overlay|\block(ed)?\s+to\b|cutscene', re.I)


# ---------------------------------------------------------------- XXH3-64 (inputs over 240 bytes)

_K = bytes.fromhex(
    'b8fe6c3923a44bbe7c01812cf721ad1cded46de9839097db7240a4a4b7b3671fcb79e64eccc0e578825ad07dcc'
    'ff7221b8084674f743248ee03590e6813a264c3c2852bb91c300cb88d0658b1b532ea371644897a20df94e3819'
    'ef46a9deacd8a8fa763fe39c343ff9dcbbc7c70b4f1d8a51e04bcdb45931c89f7ec9d9787364eac5ac8334d3eb'
    'c3c581a0fffa1363eb170ddd51b7f0da49d316552629d4689e2b16be587d47a1fc8ff8b8d17ad031ce45cb3a8f'
    '95160428afd7fbcabb4b407e')
_M64 = (1 << 64) - 1
_P32_1, _P32_2, _P32_3 = 0x9E3779B1, 0x85EBCA77, 0xC2B2AE3D
_P64_1, _P64_2, _P64_3 = 0x9E3779B185EBCA87, 0xC2B2AE3D27D4EB4F, 0x165667B19E3779F9
_P64_4, _P64_5 = 0x85EBCA77C2B2AE63, 0x27D4EB2F165667C5


def xxh3_64(data):
    """XXH3-64 with the default secret and seed 0, for inputs longer than 240 bytes (a module's
    code pages): the stripes loop of xxhash's scalar path."""
    n = len(data)
    if n <= 240:
        raise ValueError('xxh3_64 here takes more than 240 bytes')
    secret = struct.unpack('<24Q', _K)
    acc = [_P32_3, _P64_1, _P64_2, _P64_3, _P64_4, _P32_2, _P64_5, _P32_1]
    per_block = (len(_K) - 64) // 8            # 16 stripes of 64 bytes a block
    block = 64 * per_block

    def stripes(off, count):
        words = struct.unpack_from('<%dQ' % (8 * count), data, off)
        a0, a1, a2, a3, a4, a5, a6, a7 = acc
        for s in range(count):
            w = words[8 * s:8 * s + 8]
            k = secret[s:s + 8]                 # the secret moves 8 bytes a stripe
            d = w[0] ^ k[0]; a1 += w[0]; a0 += (d & 0xFFFFFFFF) * (d >> 32)
            d = w[1] ^ k[1]; a0 += w[1]; a1 += (d & 0xFFFFFFFF) * (d >> 32)
            d = w[2] ^ k[2]; a3 += w[2]; a2 += (d & 0xFFFFFFFF) * (d >> 32)
            d = w[3] ^ k[3]; a2 += w[3]; a3 += (d & 0xFFFFFFFF) * (d >> 32)
            d = w[4] ^ k[4]; a5 += w[4]; a4 += (d & 0xFFFFFFFF) * (d >> 32)
            d = w[5] ^ k[5]; a4 += w[5]; a5 += (d & 0xFFFFFFFF) * (d >> 32)
            d = w[6] ^ k[6]; a7 += w[6]; a6 += (d & 0xFFFFFFFF) * (d >> 32)
            d = w[7] ^ k[7]; a6 += w[7]; a7 += (d & 0xFFFFFFFF) * (d >> 32)
        acc[:] = [a & _M64 for a in (a0, a1, a2, a3, a4, a5, a6, a7)]

    def stripe_at(off, key_off):
        w = struct.unpack_from('<8Q', data, off)
        k = struct.unpack_from('<8Q', _K, key_off)
        for i in range(8):
            d = w[i] ^ k[i]
            acc[i ^ 1] = (acc[i ^ 1] + w[i]) & _M64
            acc[i] = (acc[i] + (d & 0xFFFFFFFF) * (d >> 32)) & _M64

    scramble = struct.unpack_from('<8Q', _K, len(_K) - 64)
    blocks = (n - 1) // block
    for b in range(blocks):
        stripes(b * block, per_block)
        for i in range(8):
            a = acc[i]
            a ^= a >> 47
            a ^= scramble[i]
            acc[i] = (a * _P32_1) & _M64
    stripes(blocks * block, ((n - 1) - block * blocks) // 64)
    stripe_at(n - 64, len(_K) - 64 - 7)
    h = (n * _P64_1) & _M64
    for i in range(4):
        k0, k1 = struct.unpack_from('<2Q', _K, 11 + 16 * i)
        p = (acc[2 * i] ^ k0) * (acc[2 * i + 1] ^ k1)
        h = (h + ((p & _M64) ^ (p >> 64))) & _M64
    h ^= h >> 37
    h = (h * 0x165667919E3779F9) & _M64
    return h ^ (h >> 32)


# ------------------------------------------------------------------------------- the module hash

def _image_range(work):
    """(image base, code start, code end) from the generated precompiled header: the code pages
    Xenia canary hashes are the 64 KB pages of the code (REX_CODE_BASE, REX_CODE_SIZE)."""
    gen = os.path.join(work, 'generated', 'default')
    if not os.path.isdir(gen):
        return None
    for f in os.listdir(gen):
        if f.endswith('_pch.h'):
            t = open(os.path.join(gen, f), encoding='utf-8', errors='replace').read()
            v = {k: int(x, 16) for k, x in re.findall(
                r'#define (REX_IMAGE_BASE|REX_CODE_BASE|REX_CODE_SIZE) (0x[0-9A-Fa-f]+)', t)}
            if len(v) == 3:
                page = 0x10000
                s = (v['REX_CODE_BASE'] - v['REX_IMAGE_BASE']) // page * page
                e = -(-(v['REX_CODE_BASE'] + v['REX_CODE_SIZE'] - v['REX_IMAGE_BASE']) // page) * page
                return v['REX_IMAGE_BASE'], s, e
    return None


def image_hash(work):
    """Xenia canary's module hash from run/image.bin (the loaded image as ReXGlue left it, its
    import thunks as loaded): the code pages, each thunk (an import record of type 1: `mtctr r11;
    bctr` after its two record words) rewritten `sc 2; blr; nop; nop` as Xenia canary does."""
    rng = _image_range(work)
    image = os.path.join(work, 'run', 'image.bin')
    if not rng or not os.path.exists(image):
        return None
    _, s, e = rng
    with open(image, 'rb') as f:
        f.seek(s)
        code = bytearray(f.read(e - s))
    if len(code) != e - s:
        return None
    thunk = struct.pack('>4I', 0x44000042, 0x4E800020, 0x60000000, 0x60000000)
    tail = b'\x7d\x69\x03\xa6\x4e\x80\x04\x20'
    i = code.find(tail)
    while i >= 0:
        o = i - 8
        if o >= 0 and o % 4 == 0 and code[o] == 1:
            code[o:o + 16] = thunk
        i = code.find(tail, i + 4)
    return '%016X' % xxh3_64(bytes(code))


def _logged_hash(work, xex_name):
    """The module hash ReXGlue logged for the game's executable (codegen.log, the newest run
    logs): `Module default.xex: title 4D5307F1, module hash EE56F849188A6A20`."""
    pat = re.compile(r'Module (\S+): title [0-9A-F]{8}, module hash ([0-9A-F]{16})')
    files = [os.path.join(work, 'codegen.log')]
    logs = os.path.join(work, 'run', 'logs')
    if os.path.isdir(logs):
        files += sorted((os.path.join(logs, f) for f in os.listdir(logs) if f.endswith('.log')),
                        key=os.path.getmtime, reverse=True)[:5]
    files.sort(key=lambda p: os.path.getmtime(p) if os.path.exists(p) else 0, reverse=True)
    for p in files:
        try:
            text = open(p, encoding='utf-8', errors='replace').read()
        except OSError:
            continue
        # ReXGlue names the module by its file name without the extension ('default').
        names = {xex_name.lower(), os.path.splitext(xex_name)[0].lower()}
        found = [h for m, h in pat.findall(text) if m.lower() in names]
        if found:
            return found[-1], os.path.getmtime(p)
    return None, 0


def _xex(work):
    from . import title_update
    return title_update.game_xex(work)


def _version_key(work):
    """What the module hash depends on: the executable and its title update patch."""
    x = _xex(work)
    parts = []
    for p in (x, x + 'p'):
        if os.path.exists(p):
            st = os.stat(p)
            parts.append('%d:%d' % (st.st_size, int(st.st_mtime)))
    return '|'.join(parts)


def patch_path(work):
    """The patch file ReXGlue applies to the game's executable: <xex>.patch.toml beside it."""
    return _xex(work) + '.patch.toml'


def module_hash(work):
    """Xenia canary's module hash of the game's executable, or None while it is not known: from
    ReXGlue's log (after a build with SDK patch 0047), else from run/image.bin when that image was
    dumped with no patch file applied; kept in fps-boost.json for this executable and update."""
    choice = _load(work)
    key = _version_key(work)
    if choice.get('module_hash') and choice.get('version') == key:
        return choice['module_hash']
    x = _xex(work)
    if not os.path.exists(x):
        return None
    # Older than the executable or its update: another version's.
    newest = max(os.path.getmtime(p) for p in (x, x + 'p') if os.path.exists(p))
    h, at = _logged_hash(work, os.path.basename(x))
    if h and at < newest:
        h = None
    pf = patch_path(work)
    image = os.path.join(work, 'run', 'image.bin')
    # The image as dumped is patched when a patch file was there: its hash would be another.
    if not h and os.path.exists(image) and os.path.getmtime(image) >= newest and (
            not os.path.exists(pf) or os.path.getmtime(pf) > os.path.getmtime(image)):
        h = image_hash(work)
    if h:
        choice.update(module_hash=h, version=key)
        _save(work, choice)
    return h


# ----------------------------------------------------------------- Xenia canary's game-patches

def cache_dir():
    base = os.environ.get('XDG_CACHE_HOME') or (
        os.environ.get('LOCALAPPDATA') if os.name == 'nt' else None) or os.path.join(
        os.path.expanduser('~'), '.cache')
    d = os.path.join(base, 'rexnative', 'game-patches')
    os.makedirs(d, exist_ok=True)
    return d


def _get(url, timeout=60):
    req = urllib.request.Request(url, headers={'User-Agent': USER_AGENT})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def catalog(refresh=False):
    """{TITLE ID: [(path, blob sha)]} of the repository's patch files, fetched again after a day
    (the last list is used when GitHub cannot be reached)."""
    p = os.path.join(cache_dir(), 'tree.json')
    have = None
    if os.path.exists(p):
        try:
            have = json.load(open(p, encoding='utf-8'))
        except ValueError:
            have = None
    if refresh or not have or time.time() - have.get('at', 0) > TREE_DAYS * 86400:
        try:
            tree = json.loads(_get(TREE_URL))
            have = {'at': time.time(), 'files': [
                [t['path'], t['sha']] for t in tree.get('tree', [])
                if t.get('type') == 'blob' and t['path'].startswith('patches/') and
                t['path'].endswith('.patch.toml')]}
            with open(p + '.tmp', 'w', encoding='utf-8') as f:
                json.dump(have, f)
            os.replace(p + '.tmp', p)
        except (OSError, ValueError):
            if not have:
                raise
    out = {}
    for path, sha in have['files']:
        tid = os.path.basename(path)[:8].upper()
        out.setdefault(tid, []).append((path, sha))
    return out


def fetch(path, sha):
    """A patch file's text, kept by its blob hash."""
    p = os.path.join(cache_dir(), '%s.toml' % sha)
    if not os.path.exists(p):
        data = _get(RAW_URL % urllib.request.quote(path))
        with open(p + '.tmp', 'wb') as f:
            f.write(data)
        os.replace(p + '.tmp', p)
    return open(p, encoding='utf-8').read()


def _toml(text):
    try:
        import tomllib
    except ImportError:          # Python before 3.11
        import tomli as tomllib  # noqa: F401
    return tomllib.loads(text)


def _hashes(doc):
    h = doc.get('hash')
    hs = h if isinstance(h, list) else [h]
    return [x.upper().replace('0X', '') for x in hs if isinstance(x, str)]


def fps_patch(doc):
    """The patch of a file that raises the frame rate, the best of them: 60 FPS first (the Deck's
    screen, and ReXNative's frame limit), then an unlocked rate, then another rate above 30."""
    best = None
    for p in doc.get('patch') or []:
        name = p.get('name') or ''
        m = _FPS.search(name)
        if not m or _NOT_FPS.search(name):
            continue
        fps = int(m.group(1)) if m.group(1) else 0
        if fps and fps <= 30:
            continue
        rank = (0 if fps == 60 else 1 if not fps else 2 if fps > 60 else 3,
                0 if re.search(r'speed|delta|fix', name, re.I) else 1)
        if best is None or rank < best[0]:
            best = (rank, p)
    return best[1] if best else None


# --------------------------------------------------------------------------------- the choice

def _load(work):
    p = os.path.join(work, CHOICE)
    try:
        return json.load(open(p, encoding='utf-8'))
    except (OSError, ValueError):
        return {}


def _save(work, choice):
    p = os.path.join(work, CHOICE)
    with open(p + '.tmp', 'w', encoding='utf-8') as f:
        json.dump(choice, f, indent=1)
    os.replace(p + '.tmp', p)


def wanted(work):
    return bool(_load(work).get('on'))


def set_wanted(work, on):
    choice = _load(work)
    choice['on'] = bool(on)
    choice['at'] = time.strftime('%Y-%m-%d %H:%M:%S')
    _save(work, choice)
    return status(work)


def _title_id(work):
    from . import xex
    return (xex.read(_xex(work)).get('title_id') or '').upper()


def find(work, refresh=False):
    """(file path, blob sha, parsed file, the frame rate patch, reason): the game's patch file for
    its executable's module hash with a frame rate patch, or the reason there is none."""
    tid = _title_id(work)
    files = catalog(refresh).get(tid, [])
    if not files:
        return None, None, None, None, 'Xenia canary has no patches for this game (%s)' % tid
    h = module_hash(work)
    if not h:
        return None, None, None, None, ("the game's version is known after its next build "
                                         '(Xenia canary has patches for %s)' % tid)
    others = []
    for path, sha in files:
        try:
            doc = _toml(fetch(path, sha))
        except (OSError, ValueError) as e:
            others.append('%s (unreadable: %s)' % (os.path.basename(path), e))
            continue
        if h in _hashes(doc):
            p = fps_patch(doc)
            if not p:
                return path, sha, doc, None, ('the patches for this version (%s) have none that '
                                              'raises the frame rate' % os.path.basename(path))
            return path, sha, doc, p, None
        if fps_patch(doc):
            others.append(os.path.basename(path)[11:-len('.patch.toml')])
    if others:
        return None, None, None, None, ('the frame rate patches are for other versions of the '
                                        'game (%s); this one is %s' % ('; '.join(others), h))
    return None, None, None, None, 'Xenia canary has no frame rate patch for this game'


def built_with(work):
    """The patch file fingerprint codegen.ok records (none when it records nothing)."""
    try:
        text = open(os.path.join(work, 'codegen.ok'), encoding='utf-8').read()
    except OSError:
        return None
    m = re.search(r'^patches (\S+)$', text, re.M)
    return m.group(1) if m else 'none'


def _sdk_applied(work):
    """Whether the last codegen run applied a patch (ReXGlue logs `Game patch '60 FPS' applied`)."""
    try:
        text = open(os.path.join(work, 'codegen.log'), encoding='utf-8', errors='replace').read()
    except OSError:
        return False
    return "Game patch '" in text and ' applied to ' in text


def fingerprint(work):
    """sha1 (16 digits) of the patch file beside the executable, 'none' without one."""
    p = patch_path(work)
    if not os.path.exists(p):
        return 'none'
    return hashlib.sha1(open(p, 'rb').read()).hexdigest()[:16]


def status(work, refresh=False):
    """What the game page shows: whether FPS Boost exists for this version, is chosen, and is in
    the built game."""
    out = {'on': wanted(work), 'available': False, 'patch': None, 'desc': None, 'author': None,
           'file': None, 'url': None, 'reason': None, 'module_hash': None}
    try:
        path, sha, doc, p, reason = find(work, refresh)
    except (OSError, ValueError) as e:
        path, p, reason = None, None, 'the patch list could not be read (%s)' % e
    out['module_hash'] = _load(work).get('module_hash')
    if p:
        out.update(available=True, patch=p.get('name'), desc=p.get('desc'),
                   author=p.get('author'), file=os.path.basename(path),
                   url=REPO_URL % urllib.request.quote(path))
    else:
        out['reason'] = reason
    built = built_with(work)
    cur = fingerprint(work)
    applied = built is not None and built == cur and cur != 'none'
    if applied and not _sdk_applied(work):
        # Codegen ran over the file, but its SDK does not read patch files (before SDK patch
        # 0047): the game is built without the patch.
        applied = False
        out['reason'] = "the SDK in use does not apply patch files (it needs ReXGlue's patch 0047)"
    out['built'] = applied
    want_file = out['on'] and out['available']
    out['needs_build'] = (built is not None) and (want_file != applied or
                                                  (want_file and _render_if(work) != _current(work)))
    return out


def _current(work):
    p = patch_path(work)
    return open(p, encoding='utf-8').read() if os.path.exists(p) else None


def _value(kind, v):
    if kind.startswith('be'):
        return '0x%x' % (v & ((1 << 64) - 1)) if isinstance(v, int) else json.dumps(v)
    if kind in ('f32', 'f64'):
        return repr(float(v))
    return json.dumps(v, ensure_ascii=False)


def render(doc, patch, path, sha, h):
    """The patch file beside the executable: the file's title and this module's hash, with the
    frame rate patch alone, enabled."""
    lines = ['# FPS Boost (ReXNative): Xenia canary\'s "%s" patch, from' % patch.get('name'),
             '# %s (blob %s).' % (REPO_URL % path, sha),
             'title_name = %s' % json.dumps(doc.get('title_name') or '', ensure_ascii=False),
             'title_id = %s' % json.dumps(doc.get('title_id') or ''),
             'hash = "%s"' % h, '', '[[patch]]']
    for k in ('name', 'desc', 'author'):
        if patch.get(k):
            lines.append('    %s = %s' % (k, json.dumps(patch[k], ensure_ascii=False)))
    lines.append('    is_enabled = true')
    for kind in ('be8', 'be16', 'be32', 'be64', 'f32', 'f64', 'string', 'u16string', 'array'):
        for w in patch.get(kind) or []:
            lines += ['', '    [[patch.%s]]' % kind, '        address = 0x%08x' % w['address'],
                      '        value = %s' % _value(kind, w['value'])]
    return '\n'.join(lines) + '\n'


def _render_if(work):
    """The patch file the choice asks for, or None (no file)."""
    if not wanted(work):
        return None
    path, sha, doc, p, _ = find(work)
    if not p:
        return None
    return render(doc, p, path, sha, module_hash(work))


def sync(work, log=print):
    """Before codegen: the patch file beside the executable as the choice says (written, replaced
    or removed). Returns the fingerprint codegen.ok records. When the patches cannot be read (no
    network and nothing kept), the file stays as it is."""
    p = patch_path(work)
    try:
        want = _render_if(work)
    except (OSError, ValueError) as e:
        log('  FPS Boost: the patch list could not be read (%s): the patch file stays as it is' % e)
        return fingerprint(work)
    cur = _current(work)
    if want != cur:
        if want is None:
            os.remove(p)
            log('  FPS Boost: off (the patch file goes; the code is generated again)')
        else:
            with open(p, 'w', encoding='utf-8', newline='\n') as f:
                f.write(want)
            log('  FPS Boost: %s (the code is generated again with the patch)' % (
                re.search(r'^\s+name = (".*")$', want, re.M).group(1)))
    elif wanted(work) and want is None:
        st = status(work)
        log('  FPS Boost: not for this game: %s' % st['reason'])
    return fingerprint(work)


def main(argv=None):
    import argparse
    ap = argparse.ArgumentParser(prog='rexnative fps-boost', description=__doc__.split('\n')[0])
    ap.add_argument('work')
    ap.add_argument('state', nargs='?', choices=('on', 'off'))
    ap.add_argument('--refresh', action='store_true', help='fetch the patch list again')
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args(argv)
    work = os.path.abspath(a.work)
    st = set_wanted(work, a.state == 'on') if a.state else status(work, a.refresh)
    if a.json:
        print(json.dumps(st))
        return
    if st['available']:
        print('FPS Boost: %s, "%s" by %s (%s)' % ('on' if st['on'] else 'off', st['patch'],
                                                st['author'], st['file']))
        if st['desc']:
            print('  ' + st['desc'])
    else:
        print('FPS Boost: not available: %s' % st['reason'])
    if st['needs_build']:
        print('  the next build applies the choice')
