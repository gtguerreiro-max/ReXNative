"""Finds a reference port's guest D3D functions in another game.

Both games link a version of the XDK D3D library, so most of its functions have the same
instruction listing at another address. Methods, in order of confidence:

  exact        the listing is the same (in-image branch targets relative, `lis` immediates
               masked) and unique in the target
  masked       the same with every number masked, unique in the target
  region       exact/masked with several candidates, only one inside the target's D3D library
  callgraph    a matched function calls it at the aligned call site (the two listings aligned
               by mnemonic); every vote agrees
  neighbour    the function next to a matched one (the library keeps its object files' order)
               is next to the counterpart too, with a similar listing (tiny functions included)
  callees      the target function in the library that calls the most of its matched callees
               (the callee-set fingerprint), with a similar listing
  fuzzy        the most similar mnemonic sequence in the library, clearly ahead of the next
  wrapper      not found, but it is a thin wrapper of a matched function: hook the inner one
               (LTCG builds inline the public wrappers, Iron Brigade's Resolve)

PGR4Native's scripts/analysis/tu_address_map.py and Iron Brigade's tools/match_d3d_fd.py did
the first two and the last-but-one; the call graph steps follow the XREF idea of Cxbx-Reloaded's
XbSymbolDatabase (a symbol found through the functions that call it).
"""
import collections
import difflib
import re

from .listing import mnemonics

NUM = re.compile(r'-?(?:0x[0-9a-fA-F]+|\d+)')
ADDR = re.compile(r'0x8[0-9a-f]{7}')
IMAGE = (0x82000000, 0x8A800000)

RANK = {'exact': 0, 'masked': 1, 'region': 2, 'callgraph': 3, 'neighbour': 4, 'callees': 5,
        'fuzzy': 6, 'wrapper': 7}


def keys(insns):
    """(exact, masked) keys of a listing."""
    exact, masked = [], []
    for addr, insn in insns:
        def rel(m):
            v = int(m.group(0), 16)
            return '@%+d' % (v - addr) if IMAGE[0] <= v < IMAGE[1] else m.group(0)
        e = ADDR.sub(rel, insn)
        if insn.startswith('lis '):
            e = NUM.sub('N', e)
        exact.append(e)
        masked.append(NUM.sub('N', insn))
    return '\n'.join(exact), '\n'.join(masked)


def ratio(a, b):
    """Mnemonic sequence similarity of two listings."""
    sm = difflib.SequenceMatcher(None, mnemonics(a), mnemonics(b), autojunk=False)
    return sm.ratio()


def clusters(addrs, gap=0x20000, pad=0x4000):
    """[(lo, hi)] around groups of addresses no more than `gap` apart."""
    out = []
    for a in sorted(addrs):
        if out and a - out[-1][1] <= gap:
            out[-1][1] = a
        else:
            out.append([a, a])
    return [(lo - pad, hi + pad) for lo, hi in out]


def in_ranges(a, ranges):
    return any(lo <= a < hi for lo, hi in ranges)


class Match:
    __slots__ = ('ref', 'target', 'how', 'score', 'detail', 'margin')

    def __init__(self, ref, target, how, score=1.0, detail='', margin=None):
        self.ref, self.target, self.how, self.score, self.detail = ref, target, how, score, detail
        self.margin = margin  # score - the next candidate's, where there was a choice

    def confidence(self):
        m = 1.0 if self.margin is None else self.margin
        if self.how in ('exact', 'masked', 'region'):
            return 'high'
        if self.how == 'callgraph':
            return 'high' if self.score >= 0.6 else 'medium'
        if self.how == 'neighbour':
            return 'high' if self.score >= 0.9 else 'medium'
        if self.how == 'callees':
            return 'high' if self.score >= 0.9 and m >= 0.2 else 'medium'
        if self.how == 'fuzzy':
            if self.score >= 0.95 and m >= 0.1:
                return 'high'
            if self.score >= 0.85 or (self.score >= 0.7 and m >= 0.25):
                return 'medium'
            return 'low'
        return 'low'


class Matcher:
    def __init__(self, ref_tree, target_tree, named, log=None):
        """named: {ref address: name} of the functions wanted (hooks and d3d_surface.inc)."""
        self.R, self.T = ref_tree, target_tree
        self.named = {a: n for a, n in named.items() if a in ref_tree}
        self.missing_in_ref = sorted(a for a in named if a not in ref_tree)
        self.log = log or (lambda *a: None)
        self.map = {}  # ref -> Match
        self.taken = collections.defaultdict(set)  # target -> refs
        self.ambiguous = {}  # ref -> [target candidates]
        self.ref_ranges = clusters(self.named)
        # Context: every reference function in the library's address ranges (more anchors for
        # the call graph steps).
        self.scope = [f for f in ref_tree.starts if in_ranges(f, self.ref_ranges)]
        self.target_ranges = []
        self.candidates = {}  # ref -> [(score, target, why)] not accepted
        self.callgraph_min = 0.5

    # -- bookkeeping ------------------------------------------------------------------------
    def _set(self, r, t, how, score=1.0, detail='', margin=None):
        old = self.map.get(r)
        if old:
            self.taken[old.target].discard(r)
        self.map[r] = Match(r, t, how, score, detail, margin)
        self.taken[t].add(r)

    # -- steps --------------------------------------------------------------------------------
    def by_listing(self):
        t_exact, t_masked = collections.defaultdict(list), collections.defaultdict(list)
        for f, insns in self.T.funcs.items():
            if not insns:
                continue
            e, m = keys(insns)
            t_exact[e].append(f)
            t_masked[m].append(f)
        for r in self.scope:
            insns = self.R[r]
            if len(insns) < 2:
                continue
            e, m = keys(insns)
            if len(t_exact.get(e, ())) == 1:
                self._set(r, t_exact[e][0], 'exact')
            elif len(t_masked.get(m, ())) == 1 and len(insns) >= 4:
                self._set(r, t_masked[m][0], 'masked')
            else:
                cands = t_exact.get(e) or t_masked.get(m)
                if cands and len(cands) <= 64:
                    self.ambiguous[r] = list(cands)

    def library_ranges(self):
        """The target's D3D library: around the confident matches of sizeable functions, plus
        a little around every confident match of a wanted function (an engine may link a few
        D3D objects far from the rest: From Dust's draw functions at 8250D000)."""
        anchors = [m.target for r, m in self.map.items()
                   if m.confidence() == 'high' and len(self.R[r]) >= 12]
        if not anchors:
            anchors = [m.target for m in self.map.values()]
        # Keep the dense part: clusters holding at least 3 anchors (stray matches of common
        # helpers elsewhere in the game would widen the region).
        groups = collections.Counter()
        cl = clusters(anchors, gap=0x20000, pad=0x8000)
        for a in anchors:
            for i, (lo, hi) in enumerate(cl):
                if lo <= a < hi:
                    groups[i] += 1
        ranges = [cl[i] for i in range(len(cl)) if groups[i] >= 3] or cl
        for r, m in self.map.items():
            if r in self.named and m.confidence() == 'high' and not in_ranges(m.target, ranges):
                ranges.append((m.target - 0x2000, m.target + 0x2000))
        self.target_ranges = sorted(ranges)
        return self.target_ranges

    def by_region(self):
        for r, cands in list(self.ambiguous.items()):
            if r in self.map:
                continue
            inside = [c for c in cands if in_ranges(c, self.target_ranges) and not self.taken[c]]
            if len(inside) == 1:
                self._set(r, inside[0], 'region', detail='%d candidates, 1 in the library'
                          % len(cands))

    def _aligned_calls(self, r, t):
        """[(ref callee, target callee)] at aligned call sites of two listings."""
        a, b = self.R[r], self.T[t]
        ca = {addr: callee for addr, callee, _ in self.R.calls.get(r, ())}
        cb = {addr: callee for addr, callee, _ in self.T.calls.get(t, ())}
        if not ca or not cb:
            return []
        sm = difflib.SequenceMatcher(None, mnemonics(a), mnemonics(b), autojunk=False)
        out = []
        for tag, i1, i2, j1, j2 in sm.get_opcodes():
            if tag == 'equal' or (tag == 'replace' and i2 - i1 == j2 - j1):
                for k in range(i2 - i1):
                    x, y = ca.get(a[i1 + k][0]), cb.get(b[j1 + k][0])
                    if x is not None and y is not None:
                        out.append((x, y))
        return out

    def by_callgraph(self, rounds=6):
        done = set()
        for _ in range(rounds):
            votes = collections.defaultdict(collections.Counter)
            for r, m in list(self.map.items()):
                if (r, m.target) in done or m.target not in self.T:
                    continue
                done.add((r, m.target))
                for x, y in self._aligned_calls(r, m.target):
                    votes[x][y] += 1
            added = 0
            for x, c in votes.items():
                if x in self.map or x not in self.R or not in_ranges(x, self.ref_ranges):
                    continue
                (y, n), = c.most_common(1)
                if n != sum(c.values()) or y not in self.T:
                    continue  # disagreeing votes
                if self.taken[y]:
                    continue
                score = ratio(self.R[x], self.T[y])
                if score < self.callgraph_min:
                    # An aligned call to a function that changed too much: most likely the
                    # reference function was inlined into another one. Kept as a candidate.
                    self.candidates.setdefault(x, []).append(
                        (score, y, 'called at the aligned site, listing %.2f' % score))
                    continue
                self._set(x, y, 'callgraph', score, '%d call site(s), listing %.2f' % (n, score))
                added += 1
            if not added:
                break

    def by_neighbour(self, wanted, accept=0.6, sure=0.9):
        """Functions next to a matched one, whose counterparts are next to each other too."""
        import bisect
        added = 1
        while added:
            added = 0
            for r in wanted:
                if r in self.map:
                    continue
                i = bisect.bisect_left(self.R.starts, r)
                for step in (-1, 1):
                    j = i + step
                    if not 0 <= j < len(self.R.starts):
                        continue
                    nb = self.R.starts[j]
                    m = self.map.get(nb)
                    if not m or m.confidence() != 'high' or m.target not in self.T:
                        continue
                    k = bisect.bisect_left(self.T.starts, m.target) - step
                    if not 0 <= k < len(self.T.starts):
                        continue
                    cand = self.T.starts[k]
                    if self.taken[cand]:
                        continue
                    same = keys(self.R[r])[1] == keys(self.T[cand])[1]
                    score = 1.0 if same else ratio(self.R[r], self.T[cand])
                    if accept <= score < sure:
                        # Not the same listing: only if nothing else in the library is
                        # clearly more alike (the order may differ: Iron Brigade's draws).
                        best = self.scan(r)
                        if best and best[0][1] != cand and best[0][0] > score + 0.05:
                            self.candidates.setdefault(r, []).append(
                                (score, cand, 'neighbour of %08X, but %08X is %.2f alike'
                                 % (nb, best[0][1], best[0][0])))
                            continue
                    if score >= accept:
                        self._set(r, cand, 'neighbour', score, '%s %08X, listing %.2f' % (
                            'after' if step == -1 else 'before', nb, score))
                        added += 1
                        break

    def _callers_in_target(self, ts):
        return collections.Counter(c for t in ts for c in self.T.callers.get(t, ()))

    def by_callees(self, wanted):
        for r in wanted:
            if r in self.map:
                continue
            callees = {c for _, c, _ in self.R.calls.get(r, ()) if c in self.map}
            images = {self.map[c].target for c in callees}
            if len(images) < 2:
                continue
            cnt = self._callers_in_target(images)
            scored = []
            for cand, n in cnt.items():
                if self.taken[cand] or not in_ranges(cand, self.target_ranges) or n < 2:
                    continue
                cover = n / len(images)
                if cover < 0.5:
                    continue
                scored.append((0.5 * cover + 0.5 * ratio(self.R[r], self.T[cand]), cand, n))
            scored.sort(reverse=True)
            if scored and scored[0][0] >= 0.6 and (len(scored) == 1 or
                                                   scored[0][0] - scored[1][0] >= 0.1):
                s, cand, n = scored[0]
                nxt = scored[1][0] if len(scored) > 1 else 0.0
                self._set(r, cand, 'callees', s, '%d of %d matched callees, score %.2f (next %.2f)'
                          % (n, len(images), s, nxt), margin=s - nxt)

    def scan(self, r):
        """[(listing ratio, target function)] of the library's free functions, best first."""
        want = mnemonics(self.R[r])
        scored = []
        for f in self.T.starts:
            if not in_ranges(f, self.target_ranges) or self.taken[f]:
                continue
            other = self.T[f]
            if not (0.6 * len(want) <= len(other) <= 1.6 * len(want)):
                continue
            sm = difflib.SequenceMatcher(None, want, mnemonics(other), autojunk=False)
            if sm.real_quick_ratio() < 0.6 or sm.quick_ratio() < 0.6:
                continue
            scored.append((sm.ratio(), f))
        scored.sort(reverse=True)
        return scored

    def by_fuzzy(self, wanted, accept=0.70, margin=0.10):
        weak = {}
        for r in wanted:
            if r in self.map or not self.R[r]:
                continue
            scored = self.scan(r)
            if not scored:
                continue
            best, f = scored[0]
            nxt = scored[1][0] if len(scored) > 1 else 0.0
            if best >= accept and best - nxt >= margin:
                # Corroboration: it calls the counterparts of every matched callee.
                images = {self.map[c].target for _, c, _ in self.R.calls.get(r, ())
                          if c in self.map}
                calls = {c for _, c, _ in self.T.calls.get(f, ())}
                agree = bool(images) and images <= calls
                self._set(r, f, 'fuzzy', best, 'listing %.2f, next %.2f%s' % (
                    best, nxt, ', calls the same matched functions' if agree else ''),
                          margin=best - nxt + (0.15 if agree else 0.0))
            else:
                weak[r] = scored[:3]
        return weak

    def by_wrapper(self, wanted):
        """Thin reference wrappers of a matched function, whose target build has no wrapper."""
        out = {}
        for r in wanted:
            if r in self.map:
                continue
            # Calls into the middle of __savegprlr / __restgprlr are not function starts.
            calls = [c for c in self.R.calls.get(r, ()) if c[1] in self.R.funcs]
            if len(self.R[r]) > 48 or len(calls) != 1:
                continue
            inner = calls[0][1]
            if inner in self.map:
                m = self.map[inner]
                self._set(r, m.target, 'wrapper', m.score,
                          'wrapper of %08X; the target has only the inner function' % inner)
                out[r] = inner
        return out

    def run(self):
        wanted = sorted(self.named)
        self.by_listing()
        self.library_ranges()
        self.by_region()
        self.by_callgraph()
        self.library_ranges()
        self.by_neighbour(wanted)
        self.by_callees(wanted)
        self.by_callgraph()
        self.by_neighbour(wanted)
        self.library_ranges()
        self.weak = self.by_fuzzy(wanted)
        self.by_callgraph()
        self.by_neighbour(wanted)
        self.by_callees(wanted)
        self.wrappers = self.by_wrapper(wanted)
        return self
