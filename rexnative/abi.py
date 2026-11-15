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
    identity = regs == list(range(3, 11)) and stack_ok
    return {
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
