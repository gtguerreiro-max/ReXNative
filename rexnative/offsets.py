"""Guest D3D device field offsets in the target, from the matched functions (Iron Brigade's
tools/device_offsets.py, generalised).

For every matched function pair the two listings are aligned by mnemonic and each aligned
load/store with a displacement (`lwz r11,12688(r31)`) or `addi` of a large constant votes for
"reference offset -> target offset". The stack (r1) and small displacements (<1024, object
fields everywhere) are left out. Each named offset of the reference profile then takes the
target offset with the most votes; offsets nobody votes for are kept (and flagged).
"""
import collections
import difflib
import re

from .listing import mnemonics

DFORM = re.compile(r'^(l[bhwd]z?u?|lha|lwa|st[bhwd]u?|lfs|lfd|stfs|stfd)\s+([^,]+),(-?\d+)\((r\d+)\)$')
ADDI = re.compile(r'^addi\s+(r\d+),(r\d+),(-?\d+)$')


def _site(text):
    m = DFORM.match(text)
    if m:
        return m.group(1), int(m.group(3)), m.group(4)
    m = ADDI.match(text)
    if m:
        return 'addi', int(m.group(3)), m.group(2)
    return None


def votes(R, T, pairs, minimum=1024):
    """{reference offset: Counter(target offset)} over matched function pairs."""
    out = collections.defaultdict(collections.Counter)
    for a, b in pairs:
        if a not in R or b not in T:
            continue
        la, lb = R[a], T[b]
        sm = difflib.SequenceMatcher(None, mnemonics(la), mnemonics(lb), autojunk=False)
        for tag, i1, i2, j1, j2 in sm.get_opcodes():
            if tag != 'equal':
                continue
            for k in range(i2 - i1):
                xa, xb = _site(la[i1 + k][1]), _site(lb[j1 + k][1])
                if not xa or not xb or xa[0] != xb[0] or 'r1' in (xa[2], xb[2]):
                    continue
                if xa[1] < minimum or xb[1] < minimum:
                    continue  # small displacements: fields of every kind of object
                if xa[0] == 'addi' and (xa[1] > 0x7FFF or xa[1] < 0):
                    continue
                out[xa[1]][xb[1]] += 1
    return out


def used_in(tree, funcs, value):
    """How many instructions of `funcs` use `value` as a displacement or addi constant."""
    n = 0
    for f in funcs:
        for _, text in tree.funcs.get(f, ()):
            s = _site(text)
            if s and s[1] == value and s[2] != 'r1':
                n += 1
    return n


def map_offsets(ref_offsets, v, R=None, ref_funcs=()):
    """{name: {'ref', 'target', 'votes', 'total', 'how'}} for the reference profile's offsets."""
    out = {}
    for name, ref in ref_offsets.items():
        c = v.get(ref)
        row = {'ref': ref, 'target': ref, 'votes': 0, 'total': 0}
        if c:
            (tgt, n), = c.most_common(1)
            row.update(target=tgt, votes=n, total=sum(c.values()))
            row['how'] = 'same' if tgt == ref else 'moved'
            if n * 3 < row['total'] * 2:
                row['how'] += ' (split vote)'
        elif ref < 1024:
            row['how'] = 'small: kept'
        else:
            used = used_in(R, ref_funcs, ref) if R else 0
            row['how'] = ('not voted: kept (%d uses in the reference library)' % used
                          if used else 'unused in the reference library: kept')
        out[name] = row
    # Fields of one block move together: predicated tiling's count, rectangles and origins
    # (render_targets.h). A rectangle list read through a register (no use to vote on) moves
    # with the count and the origins when both moved by the same amount (Overlord II: +16 from
    # PGR4's XDK; kept, the second tile was read at the first's place and never drawn).
    block = ('rt.kDevTileCount', 'rt.kDevTileRects', 'rt.kDevTileOrigins')
    if all(k in out for k in block):
        count, rects, origins = (out[k] for k in block)
        moved = count['target'] - count['ref']
        if (moved and origins['target'] - origins['ref'] == moved and not rects['votes'] and
                rects['target'] == rects['ref']):
            rects['target'] = rects['ref'] + moved
            rects['how'] = 'moved with the tile count and origins (%+d)' % moved
    return out
