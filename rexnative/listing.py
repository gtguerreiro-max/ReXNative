"""Instruction listings and call graph of a ReXGlue generated/default directory.

ReXGlue writes every guest instruction as a comment above its C++ (`\t// lwz r11,12704(r31)`),
one `DEFINE_REX_FUNC(sub_XXXXXXXX)` per function and a `loc_XXXXXXXX:` label at each branch
target, so the listing (address, instruction) of every function can be rebuilt from the
generated code alone, without the executable. The parser is PGR4Native's
scripts/analysis/tu_address_map.py `listings()`, extended with the call graph.

Parsed trees are cached (pickle): one file per generated tree (t-<hash of its path>.pkl), holding
the key of the files it parsed (their names, sizes and times) and parsed again when they change, in
%TEMP%/rexnative-cache on Windows and ~/.cache/rexnative-cache elsewhere. Before, every build of a
game added a file and none went: 8.8 GB in 161 files on the PC, and on the Steam Deck 669 MB in
/tmp, which is in memory there with a quota per user that leftovers filled, after which flatpak
could start no app (the ReXNative Manager's launcher among them; 2026-11-25). Files of that older
scheme, and caches not used for CACHE_DAYS days, are removed when a cache is written.
"""
import glob
import hashlib
import os
import pickle
import re
import sys
import tempfile

FUNC = re.compile(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)', re.M)
INSN = re.compile(r'^\t// ([a-z][a-z0-9.+\-]*(?: [^\n]*)?)$')
LABEL = re.compile(r'^loc_([0-9A-F]{8}):')
TARGET = re.compile(r'0x(8[0-9a-f]{7})$')

CACHE_VERSION = 2
CACHE_DAYS = 30


class Tree:
    """The parsed functions of one generated tree.

    funcs: {start address: [(address, instruction text)]}
    calls: {start address: [(call site address, callee, kind)]}, kind 'bl' or 'tail' (a `b` to
           another function's start), in listing order
    callers: {callee: set(callers)}
    """

    def __init__(self, path, funcs):
        self.path = path
        self.funcs = funcs
        self.starts = sorted(funcs)
        self.calls = {}
        self.callers = {}
        starts = set(funcs)
        for f, insns in funcs.items():
            out = []
            for addr, text in insns:
                mn, _, ops = text.partition(' ')
                if mn not in ('bl', 'b'):
                    continue
                m = TARGET.search(ops)
                if not m:
                    continue
                t = int(m.group(1), 16)
                if mn == 'bl':
                    out.append((addr, t, 'bl'))
                elif t in starts and t != f:
                    out.append((addr, t, 'tail'))
            self.calls[f] = out
            for _, t, _ in out:
                self.callers.setdefault(t, set()).add(f)

    def __contains__(self, f):
        return f in self.funcs

    def __getitem__(self, f):
        return self.funcs[f]

    def containing(self, addr):
        """The function whose listing holds `addr`, or None."""
        import bisect
        i = bisect.bisect_right(self.starts, addr) - 1
        if i < 0:
            return None
        f = self.starts[i]
        insns = self.funcs[f]
        if insns and insns[0][0] <= addr <= insns[-1][0]:
            return f
        return None

    def size(self, f):
        return len(self.funcs.get(f, ()))


def _parse(files):
    funcs = {}
    for path in files:
        text = open(path, encoding='utf-8', errors='replace').read()
        starts = [(m.start(), int(m.group(1), 16)) for m in FUNC.finditer(text)]
        for i, (pos, addr) in enumerate(starts):
            end = starts[i + 1][0] if i + 1 < len(starts) else len(text)
            insns, cur = [], addr
            for line in text[pos:end].split('\n'):
                m = INSN.match(line)
                if m:
                    insns.append((cur, m.group(1)))
                    cur += 4
                    continue
                m = LABEL.match(line)
                if m:
                    cur = int(m.group(1), 16)  # jump tables leave gaps in the listing
            funcs[addr] = insns
    return funcs


PACK = 'listing.json.xz'  # a reference pack's listings (save_pack): JSON, never code to run


def save_pack(tree, folder):
    """Writes the tree's listings to <folder>/listing.json.xz: a reference for adapt on a machine
    that does not have the reference game's generated code (the Steam Deck). The listings are the
    game's own instructions: such a pack stays with whoever owns the game."""
    import json
    import lzma
    os.makedirs(folder, exist_ok=True)
    data = {'version': CACHE_VERSION,
            'funcs': {'%08X' % f: [[a, t] for a, t in insns] for f, insns in tree.funcs.items()}}
    with lzma.open(os.path.join(folder, PACK), 'wt', encoding='utf-8', preset=6) as f:
        json.dump(data, f, separators=(',', ':'))


def _load_pack(path):
    import json
    import lzma
    with lzma.open(path, 'rt', encoding='utf-8') as f:
        data = json.load(f)
    return {int(k, 16): [(a, t) for a, t in v] for k, v in data['funcs'].items()}


def load(tree):
    """Tree of a generated/default directory (cached), or of a reference pack (save_pack)."""
    pack = os.path.join(tree, PACK)
    if os.path.exists(pack):
        return Tree(tree, _load_pack(pack))
    files = sorted(glob.glob(os.path.join(tree, '*_recomp.*.cpp')))
    if not files:
        sys.exit('rexnative: no ReXGlue generated code (*_recomp.*.cpp) in %s' % tree)
    key = hashlib.sha1(repr((CACHE_VERSION, [(os.path.basename(f), os.path.getmtime(f),
                                              os.path.getsize(f)) for f in files])).encode()).hexdigest()
    cache_dir = _cache_dir()
    cache = os.path.join(cache_dir, 't-%s.pkl' % hashlib.sha1(
        os.path.realpath(tree).encode('utf-8', 'surrogateescape')).hexdigest()[:20])
    try:
        with open(cache, 'rb') as f:
            cached_key, funcs = pickle.load(f)
        if cached_key == key:
            os.utime(cache)  # in use: kept by the pruning below
            return Tree(tree, funcs)
    except (OSError, EOFError, ValueError, TypeError, pickle.UnpicklingError):
        pass
    funcs = _parse(files)
    try:
        os.makedirs(cache_dir, exist_ok=True)
        with open(cache + '.new', 'wb') as f:
            pickle.dump((key, funcs), f)
        os.replace(cache + '.new', cache)
        _prune_cache(cache_dir, keep=cache)
    except OSError:
        pass  # a cache that cannot be written (a full disk) only costs the parse next time
    return Tree(tree, funcs)


def _cache_dir():
    if os.name == 'nt':
        return os.path.join(tempfile.gettempdir(), 'rexnative-cache')
    base = os.environ.get('XDG_CACHE_HOME') or os.path.join(os.path.expanduser('~'), '.cache')
    old = os.path.join(tempfile.gettempdir(), 'rexnative-cache')
    if os.path.isdir(old):  # the older place, in /tmp
        import shutil
        shutil.rmtree(old, ignore_errors=True)
    return os.path.join(base, 'rexnative-cache')


def _prune_cache(cache_dir, keep):
    """The older scheme's files (any .pkl not t-*), and caches not used for CACHE_DAYS days."""
    import time
    old = time.time() - CACHE_DAYS * 86400
    for name in os.listdir(cache_dir):
        path = os.path.join(cache_dir, name)
        if path == keep or not name.endswith(('.pkl', '.pkl.new')):
            continue
        try:
            if not name.startswith('t-') or os.path.getmtime(path) < old:
                os.remove(path)
        except OSError:
            pass


def mnemonics(insns):
    return [text.split(' ', 1)[0] for _, text in insns]
