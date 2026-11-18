"""Where a hooked function's arguments are, from the listings alone.

The renderer decodes Resolve with the public signature (hook_args.h):
    Resolve(r3 device, r4 flags, r5 src_rect, r6 dest_texture, r7 dest_point, r8 level,
            r9 slice, r10 clear_color, f1 clear_z, [sp+92] clear_stencil, [sp+100] params)
XDK 2.0.20353 made the public function a thin wrapper of an inner one with two more arguments
(From Dust's 820EE9D0 -> 820ED880), and its LTCG build (Iron Brigade) inlined the wrapper and
dropped arguments of the inner one (82649280). This module works out, for the function the
target hooks, which entry register (or stack slot) holds each public argument:

  * a wrapper's moves give its callee's arguments in terms of its own (straight-line code);
  * two matched functions' arguments correspond where the two listings, aligned by mnemonic,
    use the registers the arguments were copied into (`mr r14,r6`, `lwz r17,476(r1)`, ...)
    at the same operand of the same instruction. Each such use is a vote.

An argument is ('r', n), ('f', n), ('s', caller stack offset) or ('0',) for a constant 0.
On the Xbox 360 the n-th parameter has a doubleword slot at caller sp + 20 + 8 * n, so register
spills of r3..r10 land at +20..+83 and the 9th parameter onwards (floats included) at +84...
"""
import collections
import difflib
import re

from .listing import mnemonics

REG = re.compile(r'^([rf])(\d+)$')
DFORM = re.compile(r'^(-?\d+)\((r\d+)\)$')
NO_DEST = ('st', 'cmp', 'b', 'tw', 'dcb', 'mt', 'sync', 'eieio', 'isync', 'nop', 'lvx', 'stvx')


def _ops(text):
    mn, _, ops = text.partition(' ')
    return mn, [o.strip() for o in ops.split(',')] if ops else []


def frame_size(insns):
    for _, text in insns[:12]:
        mn, ops = _ops(text)
        if mn == 'stwu' and len(ops) == 2 and ops[0] == 'r1':
            m = DFORM.match(ops[1])
            if m and m.group(2) == 'r1':
                return -int(m.group(1))
    return 0


def caller_slot(off):
    """The argument a caller stack offset holds: a register spill slot or a stack argument."""
    if 20 <= off < 84:
        return ('r', 3 + (off - 20) // 8)
    if off >= 84:
        return ('s', off)
    return None


def _writes(mn, ops):
    if not ops or mn.startswith(NO_DEST) or mn in ('lvx128', 'stvx128'):
        return None
    return ops[0] if REG.match(ops[0]) else None


def homes(insns):
    """[(index, register, argument, end index)]: the argument a register holds from index up
    to (not including) end, from copies of argument registers and loads of caller slots."""
    frame = frame_size(insns)
    out = []
    live = {}  # register -> [start, arg]
    first_call = next((i for i, (_, t) in enumerate(insns) if t.startswith('bl ') and i > 2),
                      len(insns))
    for i, (_, text) in enumerate(insns):
        mn, ops = _ops(text)
        arg = None
        if mn == 'mr' and len(ops) == 2:
            m = REG.match(ops[1])
            if m and m.group(1) == 'r' and 3 <= int(m.group(2)) <= 10 and i < first_call:
                arg = ('r', int(m.group(2)))
            elif ops[1] in live:
                arg = live[ops[1]][1]
        elif mn == 'fmr' and len(ops) == 2:
            m = REG.match(ops[1])
            if m and 1 <= int(m.group(2)) <= 13 and i < first_call:
                arg = ('f', int(m.group(2)))
        elif mn in ('lwz', 'ld', 'lfs', 'lfd') and len(ops) == 2 and frame:
            m = DFORM.match(ops[1])
            if m and m.group(2) == 'r1' and int(m.group(1)) >= frame:
                arg = caller_slot(int(m.group(1)) - frame)
        dest = _writes(mn, ops)
        if dest and dest in live:
            s, a = live.pop(dest)
            out.append((s, dest, a, i))
        if dest and arg:
            live[dest] = [i, arg]
    for dest, (s, a) in live.items():
        out.append((s, dest, a, len(insns)))
    # The argument registers themselves, until the first call or their first overwrite.
    for n in range(3, 11):
        reg = 'r%d' % n
        end = first_call
        for i, (_, text) in enumerate(insns[:first_call]):
            mn, ops = _ops(text)
            if _writes(mn, ops) == reg:
                end = i
                break
        out.append((0, reg, ('r', n), end))
    out.append((0, 'f1', ('f', 1), first_call))
    return out


def _home_at(hs, reg, i):
    for s, r, a, e in hs:
        if r == reg and s <= i < e:
            return a
    return None


SHAPE_REG = re.compile(r'[rfv]\d+')
SHAPE_ADDR = re.compile(r'0x8[0-9a-f]{7}')


def shapes(insns):
    """Instructions with registers and code addresses masked: `cmplwi cr6,R,0`, `lwz R,4(R)`.
    Aligning on these (not on mnemonics alone) pairs the same use of the same field."""
    return [SHAPE_ADDR.sub('@', SHAPE_REG.sub('R', t)) for _, t in insns]


def correspond(ref_insns, tgt_insns):
    """{target argument: Counter(reference argument)} from aligned uses."""
    hr, ht = homes(ref_insns), homes(tgt_insns)
    votes = collections.defaultdict(collections.Counter)
    seen = set()
    # Two alignments: by mnemonic (robust to changed offsets and constants) and by shape (exact
    # about which use pairs with which); a pair of instructions votes once.
    for key in (mnemonics, shapes):
        _vote(ref_insns, tgt_insns, key, hr, ht, votes, seen)
    return votes


def _vote(ref_insns, tgt_insns, key, hr, ht, votes, seen):
    sm = difflib.SequenceMatcher(None, key(ref_insns), key(tgt_insns), autojunk=False)
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag != 'equal':
            continue
        for k in range(i2 - i1):
            i, j = i1 + k, j1 + k
            if (i, j) in seen:
                continue
            seen.add((i, j))
            mr, opr = _ops(ref_insns[i][1])
            mt, opt = _ops(tgt_insns[j][1])
            if len(opr) != len(opt):
                continue
            # Only operands read here: skip the destination of a write.
            skip = 0 if _writes(mr, opr) else -1
            for p, (a, b) in enumerate(zip(opr, opt)):
                if p == skip:
                    continue
                da, db = DFORM.match(a), DFORM.match(b)
                if da and db:
                    a, b = da.group(2), db.group(2)
                if not (REG.match(a) and REG.match(b)) or a == 'r1':
                    continue
                ha, hb = _home_at(hr, a, i), _home_at(ht, b, j)
                if ha and hb:
                    votes[hb][ha] += 1


def wrapper_moves(insns):
    """{callee argument: caller argument} of a straight-line wrapper's call."""
    frame = 0
    content = {'r%d' % n: ('r', n) for n in range(3, 11)}
    content['f1'] = ('f', 1)
    out_stack = {}
    for _, text in insns:
        mn, ops = _ops(text)
        if mn == 'stwu' and ops and ops[0] == 'r1':
            frame = -int(DFORM.match(ops[1]).group(1))
            continue
        if mn == 'bl' and content.get('_seen_save'):
            break
        if mn == 'bl':
            content['_seen_save'] = True  # the first bl is __savegprlr
            continue
        if mn == 'mr' and len(ops) == 2:
            content[ops[0]] = content.get(ops[1])
        elif mn == 'li' and len(ops) == 2:
            content[ops[0]] = ('0',) if ops[1] == '0' else ('const', int(ops[1], 0))
        elif mn in ('lwz', 'ld') and len(ops) == 2:
            m = DFORM.match(ops[1])
            if m and m.group(2) == 'r1' and frame and int(m.group(1)) >= frame:
                content[ops[0]] = caller_slot(int(m.group(1)) - frame)
            else:
                content[ops[0]] = None
        elif mn in ('stw', 'std') and len(ops) == 2:
            m = DFORM.match(ops[1])
            if m and m.group(2) == 'r1' and int(m.group(1)) < frame:
                out_stack[('s', int(m.group(1)))] = content.get(ops[0])
        else:
            d = _writes(mn, ops)
            if d:
                content[d] = None
    args = {('r', n): content.get('r%d' % n) for n in range(3, 11)}
    args[('f', 1)] = content.get('f1')
    args.update(out_stack)
    return args


def trampoline(tree, f):
    """The address a `lis rX,hi; addi rX,rX,lo; mtctr rX; bctr` function jumps to, or None
    (Jet Set Radio reaches its inner Resolve through one)."""
    insns = tree.funcs.get(f, ())
    if len(insns) != 4:
        return None
    t = [text for _, text in insns]
    m0 = re.match(r'^lis (r\d+),(-?\d+)$', t[0])
    m1 = re.match(r'^addi (r\d+),(r\d+),(-?\d+)$', t[1])
    if not (m0 and m1 and m1.group(2) == m0.group(1) and t[2] == 'mtctr %s' % m1.group(1)
            and t[3].startswith('bctr')):
        return None
    return ((int(m0.group(2)) << 16) + int(m1.group(3))) & 0xFFFFFFFF


def thin_wrapper(tree, f, max_insns=48):
    """The function `f` wraps (its only real call, through a trampoline if there is one), or
    None."""
    insns = tree.funcs.get(f, ())
    if not insns or len(insns) > max_insns:
        return None
    # The register save right after `mflr r12` and the restore the function ends with are
    # __savegprlr / __restgprlr, which some games' trees list as functions (Super Meat Boy).
    save = insns[1][0] if len(insns) > 1 and insns[0][1] == 'mflr r12' else None
    last = insns[-1][0]
    calls = [c for c in tree.calls.get(f, ()) if c[1] in tree.funcs and c[0] != save and
             not (c[2] == 'tail' and c[0] == last)]
    if len(calls) != 1:
        return None
    callee = calls[0][1]
    for _ in range(3):
        jump = trampoline(tree, callee)
        if jump is None or jump not in tree.funcs:
            break
        callee = jump
    return callee


def best(votes):
    """{target argument: (reference argument, votes, total)} keeping each reference argument
    for the target argument with the most votes for it."""
    pairs = []
    for b, c in votes.items():
        for a, n in c.items():
            pairs.append((n, b, a))
    pairs.sort(reverse=True)
    out, used = {}, set()
    for n, b, a in pairs:
        if b in out or a in used:
            continue
        out[b] = (a, n, sum(votes[b].values()))
        used.add(a)
    return out


PUBLIC = [('r', n) for n in range(3, 11)] + [('f', 1), ('s', 92), ('s', 100)]
PUBLIC_NAMES = ['device', 'flags', 'src_rect', 'dest_texture', 'dest_point', 'level', 'slice',
                'clear_color', 'clear_z', 'clear_stencil', 'params']


def fmt(a):
    if a is None:
        return '?'
    if a[0] == '0':
        return '0'
    if a[0] == 's':
        return 'sp+%d' % a[1]
    if a[0] == 'const':
        return '#%d' % a[1]
    return '%s%d' % a


# The hooked functions besides Resolve whose arguments the renderer reads in the public order
# (hook_args.h, d3d_state.cpp ReadDrawState / AfterCall). LTCG builds drop and move them: Iron
# Brigade's DrawIndexedVertices has no BaseVertexIndex (constant 0) and takes start, count in
# r5, r6; its DrawVertices takes only the count, in r5.
CALL_ARG_FUNCTIONS = ['DrawIndexedVertices', 'DrawVertices', 'DrawVerticesUP', 'BeginVertices']


def call_args(R, T, ref_fn, tgt_fn, ref_args=None):
    """[entry register for public r3..r10, 0 = constant 0] of the target's `tgt_fn`, from the
    aligned uses of its arguments against the reference's `ref_fn` (whose own map is ref_args,
    identity when None). A public argument nothing corresponds to is where the public signature
    has it if that register is not taken by another argument, else a constant 0."""
    ref_args = ref_args or list(range(3, 11))
    ref_entry_of_public = {('r', 3 + i): (('r', s) if s else ('0',)) for i, s in enumerate(ref_args)}
    to_ref = best(correspond(R[ref_fn], T[tgt_fn]))  # target arg -> (ref arg, n, total)
    # A register that moved needs two aligned uses: one can be a coincidence.
    to_ref = {b: v for b, v in to_ref.items() if v[0] == b or v[1] >= 2}
    ref_to_target = {a: b for b, (a, n, tot) in to_ref.items()}
    claimed = set(to_ref)
    # Argument registers the target copies before using (`mr r17,r5`): real arguments.
    saved = {a for s, reg, a, e in homes(T[tgt_fn]) if a[0] == 'r' and reg != 'r%d' % a[1]}
    out, evidence = [], []
    for i in range(8):
        p = ('r', 3 + i)
        ref_entry = ref_entry_of_public[p]
        if ref_entry[0] == '0':
            # A constant in the reference (Iron Brigade's BaseVertexIndex); this game may still
            # pass it, where the public signature has it (From Dust).
            out.append(3 + i if p in saved and p not in claimed else 0)
            continue
        hit = ref_to_target.get(ref_entry)
        if hit and hit[0] == 'r':
            out.append(hit[1])
            evidence.append('%s<-%s' % (fmt(p), fmt(hit)))
        elif p not in claimed:
            out.append(3 + i)
        else:
            out.append(0)
            evidence.append('%s<-0' % fmt(p))
    return out, evidence


def call_site_writes(tree, fn, back=24):
    """For each call of `fn`: {register: the last instruction writing it in the `back`
    instructions before the call}."""
    sites = []
    for caller in tree.callers.get(fn, ()):
        insns = tree.funcs.get(caller, ())
        index = {a: i for i, (a, _) in enumerate(insns)}
        for site, callee, _ in tree.calls.get(caller, ()):
            if callee != fn or site not in index:
                continue
            last = {}
            for _, text in reversed(insns[max(0, index[site] - back):index[site]]):
                mn, ops = _ops(text)
                dst = _writes(mn, ops)
                if dst and dst not in last:
                    last[dst] = (mn, ops)
            sites.append(last)
    return sites


def reads_at_entry(insns, reg):
    """Whether the function reads `reg` before writing it, in its code from the entry up to its
    first call (the register-save helper in the prologue aside): True, False, or None when the
    code reaches a call (or its end) without touching it."""
    for i, (_, text) in enumerate(insns):
        mn, ops = _ops(text)
        if mn == 'bl' and i > 2:
            return None
        dest = _writes(mn, ops)
        srcs = ops[1:] if dest else ops
        if dest == reg and mn.startswith(('rlwimi', 'rldimi')):
            return True  # an insert reads its destination
        for o in srcs:
            m = DFORM.match(o)
            if o == reg or (m and m.group(2) == reg):
                return True
        if dest == reg:
            return False
    return None


def clear_color_arg(insns, window=80):
    """The argument register the function tests against 0 and, when it is 0, replaces with the
    address of a vector it builds in its own frame (`addi rX,r1,N` and an lvx/stvx pair right
    after): the D3D Resolve's pClearColor, defaulted when the caller passes none. None if the
    first `window` instructions show no such test."""
    hs = homes(insns)
    for i, (_, text) in enumerate(insns[:window]):
        mn, ops = _ops(text)
        if mn not in ('cmplwi', 'cmpwi') or len(ops) != 3 or ops[2] != '0':
            continue
        arg = _home_at(hs, ops[1], i)
        if not arg or arg[0] != 'r' or not 7 <= arg[1] <= 10:
            continue
        after = [_ops(t) for _, t in insns[i + 1:i + 10]]
        if (any(m.startswith(('lvx', 'stvx')) for m, _ in after) and
                any(m == 'addi' and len(o) == 3 and o[1] == 'r1' for m, o in after)):
            return arg[1]
    return None


def stack_address_args(sites):
    """The registers some call sets to a stack address (`addi rN,r1,X`, a pointer into the
    caller's frame) as their last write before it (call_site_writes)."""
    return {reg for last in sites for reg, (mn, ops) in last.items()
            if mn == 'addi' and len(ops) == 3 and ops[1] == 'r1'}


def resolve_abi(R, T, ref_profile, matches, log=print):
    """The target's Resolve hook and the source of each public argument.

    matches: {ref address: Match}. Returns a dict (see emit / the report) or None."""
    ref_hooked = ref_profile['functions'].get('Resolve')
    ref_inner = ref_profile['functions'].get('ResolveInner')
    if not ref_hooked:
        return None
    # Reference: public argument -> the reference hooked function's entry argument.
    ref_args = ref_profile['resolve_abi']['args']  # public slot i (r3+i) <- entry register / 0
    ref_entry_of_public = {}
    for i, src in enumerate(ref_args):
        ref_entry_of_public[('r', 3 + i)] = ('r', src) if src else ('0',)
    for extra in (('f', 1), ('s', 92), ('s', 100)):
        ref_entry_of_public[extra] = extra

    steps = []
    m = matches.get(ref_hooked)
    target_inner = None
    ref_is_wrapper = thin_wrapper(R, ref_hooked) is not None
    if m is not None and m.how != 'wrapper' and ref_is_wrapper:
        # The reference hooks the XDK's public wrapper (From Dust) and the target has it too:
        # hook it, and its callee is the inner function. Never climb to a wrapper of it: that
        # one is the game's own (Double Dragon Neon wraps the D3D's wrapper).
        target_fn = m.target
        target_inner = thin_wrapper(T, target_fn)
        ref_moves, tgt_moves = wrapper_moves(R[ref_hooked]), wrapper_moves(T[target_fn])
        if target_inner and ref_moves == tgt_moves:
            to_ref = {a: (a, 1, 1) for a in PUBLIC}
            steps.append('target %08X is the same public wrapper as the reference %08X (same '
                         'argument moves), inner %08X' % (target_fn, ref_hooked, target_inner))
        else:
            to_ref = best(correspond(R[ref_hooked], T[target_fn]))
            steps.append('target %08X matched to the reference wrapper %08X (%s), its moves '
                         'differ: arguments by aligned uses' % (target_fn, ref_hooked, m.how))
    elif m is not None and m.how != 'wrapper':
        target_fn = m.target
        # The target may still have a public wrapper over the matched function (the reference
        # predates the XDK's wrapper: PGR4 -> From Dust). Hook the wrapper: the game calls it.
        wrappers = [c for c in T.callers.get(target_fn, ()) if thin_wrapper(T, c) == target_fn]
        if len(wrappers) == 1 and len(T.callers.get(wrappers[0], ())) >= 1:
            target_inner, target_fn = target_fn, wrappers[0]
            moves = wrapper_moves(T[target_fn])  # inner arg -> wrapper (hooked) arg
            v = best(correspond(R[ref_hooked], T[target_inner]))  # inner arg -> ref hooked arg
            to_ref = {}
            for inner_arg, (ref_arg, n, tot) in v.items():
                src = moves.get(inner_arg)
                if src and src[0] != '0':
                    to_ref[src] = (ref_arg, n, tot)
            steps.append('target %08X is a thin wrapper of %08X (matched to the reference '
                         '%08X): hooked, arguments through its moves' % (target_fn, target_inner,
                                                                         ref_hooked))
        else:
            to_ref = best(correspond(R[ref_hooked], T[target_fn]))
            steps.append('target %08X matched to the reference hooked %08X (%s)'
                         % (target_fn, ref_hooked, m.how))
    elif ref_inner and matches.get(ref_inner) is not None:
        # The reference hooks a wrapper (From Dust's public Resolve), the target has only the
        # inner function (Iron Brigade's LTCG D3D): hook it and map through the wrapper.
        target_fn = matches[ref_inner].target
        target_inner = target_fn
        moves = wrapper_moves(R[ref_hooked])  # ref inner arg -> ref hooked arg
        v = best(correspond(R[ref_inner], T[target_fn]))  # target arg -> ref inner arg
        to_ref = {}
        for b, (a, n, tot) in v.items():
            src = moves.get(a)
            if src and src[0] not in ('0', 'const'):
                to_ref[b] = (src, n, tot)
        steps.append('reference hooks the wrapper %08X of %08X; the target has only %08X: '
                     'hooked, arguments through the reference wrapper\'s moves'
                     % (ref_hooked, ref_inner, target_fn))
    else:
        return None

    # Public argument -> target entry argument.
    ref_to_target = {}
    for b, (a, n, tot) in to_ref.items():
        ref_to_target[a] = (b, n, tot)
    public = []
    claimed = {hit[0] for hit in ref_to_target.values()}
    assumed = []
    overruled = []
    for p, name in zip(PUBLIC, PUBLIC_NAMES):
        ref_entry = ref_entry_of_public[p]
        if ref_entry[0] == '0':
            # The reference layer passes a constant here. If the reference function still
            # uses its entry register at this position like the target uses one of its own,
            # the argument is there after all (Iron Brigade's layer zeroes slice, but its
            # inner Resolve reads r9 as From Dust's reads slice).
            hit = ref_to_target.get(p)
            if hit and p[0] == 'r':
                public.append((name, p, hit[0], hit[1], hit[2]))
                overruled.append(name)
            else:
                public.append((name, p, ('0',), 0, 0))
            continue
        hit = ref_to_target.get(ref_entry)
        if hit:
            public.append((name, p, hit[0], hit[1], hit[2]))
        elif p not in claimed:
            # No use of it aligned: where the public signature has it, if nothing else is
            # there (flagged in the report).
            public.append((name, p, p, 0, 0))
            assumed.append(name)
        else:
            public.append((name, p, None, 0, 0))
    regs = []
    for name, p, src, n, tot in public[:8]:
        if src is None or src[0] == '0':
            regs.append(0)
        elif src[0] == 'r':
            regs.append(src[1])
        else:
            regs.append(None)  # a register argument now on the stack: not supported
    stack_ok = all(src == p for name, p, src, n, tot in public[8:] if src is not None)
    # The reference's own mapping, copied where no use aligned (PGR4 hooks an inner Resolve that
    # takes no source rectangle and the level in r5), against the target's calls: one that passes
    # a stack address in r5 passes the source rectangle there, as the public signature has it
    # (Overlord's EndTiling calls its public Resolve so; with PGR4's mapping each tile's resolve
    # read the rectangle's address as the level and was dropped: a white scene).
    sites = call_site_writes(T, target_fn)

    def stack_sites(reg):
        # Calls whose last write to `reg` is a stack address (addi reg,r1,X): a pointer.
        return sum(1 for last in sites
                   if last.get('r%d' % reg, ('', ()))[0] == 'addi' and len(last['r%d' % reg][1]) == 3
                   and last['r%d' % reg][1][1] == 'r1')

    # A level or a slice is a small number, never a pointer into the caller's frame: a mapping
    # that takes one from a register the calls set to stack addresses more often than they set
    # the public one is the reference's guess, not this game's signature. Fable II's EndTiling
    # passes each tile's rectangle in r5 and destination point in r7 as the public call does;
    # mapped as point r5, level r7 (votes 2/3 and 1/2), every resolve read the point's address as
    # the level and was dropped: the scene never reached its texture and the world drew black
    # (Deus Ex: Human Revolution's the same). Overlord's PGR4-shaped mapping (no source rectangle,
    # the level in r5) is the same case: its tiles' resolves passed their rectangles in r5.
    misplaced = [i for i in (5, 6) if regs[i] and regs[i] != 3 + i and
                 stack_sites(regs[i]) > stack_sites(3 + i)]
    if misplaced or (regs[2] == 0 and regs[5] == 5 and 'r5' in stack_address_args(sites)):
        steps.append('the calls of %08X pass stack addresses where the mapping reads %s: the public '
                     'signature, not the reference\'s mapping' % (
                         target_fn, ', '.join('%s from r%d' % (PUBLIC_NAMES[i], regs[i])
                                              for i in misplaced) or 'the level from r5'))
        regs = list(range(3, 11))
        public = [(name, p, p, n, tot) if i < 8 else (name, p, src, n, tot)
                  for i, (name, p, src, n, tot) in enumerate(public)]
    # The level and the slice, 0 unless the caller says otherwise: a register most calls leave as
    # it was is no argument (Overlord II's LTCG Resolve takes neither; the mapping's r9, unset by
    # EndTiling, moved each tile's destination by a whole surface per leftover unit).
    for i in (5, 6):
        reg = 'r%d' % regs[i] if regs[i] else None
        if reg and sites and 2 * sum(reg in last for last in sites) < len(sites):
            steps.append('%s: %s is set before only %d of the %d calls of %08X: a constant 0' % (
                PUBLIC_NAMES[i], reg, sum(reg in last for last in sites), len(sites), target_fn))
            regs[i] = 0
            name, p, src, n, tot = public[i]
            public[i] = (name, p, ('0',), 0, 0)
    # The clear colour is the argument the function tests against 0 and then replaces with a
    # default vector (clear_color_arg); the level and the slice can only be in the registers
    # between the destination point and it. And a register the function never reads at its
    # entry is no argument at all: a constant 0. Deus Ex: Human Revolution's LTCG Resolve takes
    # the colour in r8 (the level and the slice dropped, ClearZ in r9's slot, the stencil in r10):
    # read with the public signature, each slice came from r9's leftover and moved the
    # destination by billions of bytes (no resolve reached the texture its D3D wrote).
    # Fable II's reads no r9 either (no slice), and Overlord's neither r8 nor r9.
    insns = T[target_fn]
    color = clear_color_arg(insns)
    if color and regs[4] == 7 and regs[7] != color:
        between = list(range(8, color))
        steps.append('clear_color: r%d, which %08X tests against 0 and replaces with a default '
                     'vector; level and slice from %s' % (
                         color, target_fn, ', '.join('r%d' % r for r in between) or 'nothing'))
        for i, reg in ((5, between[0] if between else 0),
                       (6, between[1] if len(between) > 1 else 0), (7, color)):
            regs[i] = reg
            name, p, src, n, tot = public[i]
            public[i] = (name, p, ('r', reg) if reg else ('0',), n, tot)
    for i in (5, 6, 7):
        if regs[i] and reads_at_entry(insns, 'r%d' % regs[i]) is False:
            steps.append('%s: %08X writes r%d before reading it: a constant 0' % (
                PUBLIC_NAMES[i], target_fn, regs[i]))
            regs[i] = 0
            name, p, src, n, tot = public[i]
            public[i] = (name, p, ('0',), 0, 0)
    identity = regs == list(range(3, 11)) and stack_ok
    # ClearStencil follows ClearColor and ClearZ (a float, which takes the next register's slot):
    # with the colour moved to a lower register (the level and the slice dropped), the stencil is
    # in a register too, two after it (Overlord II: colour r8, Z f1 in r9's slot, stencil r10),
    # when most calls set that register. Else on the stack (sp + 92), as the public call has it.
    stencil_reg = 0
    color = regs[7]
    if color and 3 <= color <= 8:
        cand = color + 2
        if cand not in regs and sites and 2 * sum(('r%d' % cand) in last for last in sites) >= len(sites):
            stencil_reg = cand
            name, p, _, n, tot = public[9]
            public[9] = (name, p, ('r', cand), n, tot)
            steps.append('clear_stencil: r%d (two after clear_color r%d, set before %d of the %d '
                         'calls)' % (cand, color, sum(('r%d' % cand) in last for last in sites),
                                     len(sites)))
    return {
        'stencil_reg': stencil_reg,
        'hook': target_fn,
        'inner': target_inner,
        'abi': 'public' if identity else 'remapped',
        'assumed': assumed,
        'overruled': overruled,
        'args': regs,
        'public': [(name, fmt(p), fmt(src), n, tot) for name, p, src, n, tot in public],
        'stack_ok': stack_ok,
        'steps': steps,
    }
