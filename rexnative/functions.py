"""Function starts ReXGlue codegen did not discover: functions the game reaches only through a
pointer (callbacks, tables of functions outside vtables, thread entries). At run time each one
stops the game ("No function registered at X", "unregistered function at guest address"), one
at a time; here they are found at once, from the loaded image and the generated code.

A candidate is an address in the code section that no recompiled function covers (from a
function's start to its last instruction), that follows code which cannot fall through into it
(blr, bctr, an unconditional b, or zero padding), and that something points to (a big-endian
word in the image equal to it, or an address built with lis + addi / ori within 32
instructions) or that begins with a prologue (mflr r12, stwu r1). It is Iron Brigade's
tools/find_functions.py (FromDustDeck's gen_manifest.py --seed-old test), made generic: the
image's and code's bounds come from the generated <name>_pch.h, the import thunks (pointed to,
but not functions) from <name>_init.cpp's import table.

The image is the loaded default.xex (decrypted, decompressed), which the project's app writes
with --rexnative_dump_image=<file> (`rexnative project --find-functions` does it).
"""
import glob
import os
import re

from . import listing


def bounds(generated):
    """(image base, image size, code base, code size) from the generated <name>_pch.h."""
    for path in glob.glob(os.path.join(generated, '*_pch.h')):
        with open(path, encoding='utf-8', errors='replace') as f:
            text = f.read()
        vals = {}
        for k in ('IMAGE_BASE', 'IMAGE_SIZE', 'CODE_BASE', 'CODE_SIZE'):
            m = re.search(r'#define REX_%s (0x[0-9A-Fa-f]+)' % k, text)
            if m:
                vals[k] = int(m.group(1), 16)
        if len(vals) == 4:
            return vals['IMAGE_BASE'], vals['IMAGE_SIZE'], vals['CODE_BASE'], vals['CODE_SIZE']
    raise SystemExit('no REX_IMAGE_BASE / REX_CODE_BASE in %s/*_pch.h' % generated)


def import_thunks(generated):
    """The import thunks' addresses (16 bytes each), from the generated import table."""
    out = set()
    for path in glob.glob(os.path.join(generated, '*_init.cpp')):
        with open(path, encoding='utf-8', errors='replace') as f:
            out.update(int(a, 16) for a in re.findall(r'\{\s*0x([0-9A-Fa-f]{8}),\s*__imp__', f.read()))
    return out


def save_restore_helpers(word, code0, code1, primary=True):
    """The XDK's register save / restore helpers (__savegprlr_14, __restgprlr_14, __savefpr_14,
    __restfpr_14, __savevmx_14/64, __restvmx_14/64), each from its first instruction to the blr
    that ends it, found by the patterns codegen uses (phase_register.cpp,
    detectSaveRestoreHelpers). Codegen turns a call into one (at any of its entries) into saves
    and restores of its own; made a function by a hint (.pdata points at them), the call stays a
    call, and with non_volatile_as_local the caller's f14-f31 / v14-v127 are not kept (Jet Set
    Radio's garbage function pointers). Never candidates.

    Codegen knows only the first of each kind (by address); a game with a second copy (Super
    Meat Boy's __restgprlr at 82238600, reached by `b` at each of its entries) gets those as
    unresolved calls. With primary=False the copies are returned instead: each of their entries
    becomes a function (as locals, the caller's non-volatile registers need no restore)."""
    starts = []
    for a in range(code0, code1 - 4, 4):
        w = word(a)
        if w in (0xE9C1FF68, 0xF9C1FF68, 0xC9CCFF70, 0xD9CCFF70):  # ld/std r14 / lfd/stfd f14
            starts.append((a, w))
        elif w in (0x3960FEE0, 0x3960FC00) and word(a + 4) in (
                0x7DCB60CE, 0x7DCB61CE, 0x100B60CB, 0x100B61CB):  # li r11 + lvx/stvx(128)
            starts.append((a, (w, word(a + 4))))
    seen = set()
    out = []
    for s, kind in starts:
        e = s
        while e < code1 and word(e) != 0x4E800020:  # to the blr
            e += 4
        first = kind not in seen
        seen.add(kind)
        if first == primary:
            out.append((s, e + 4))
    return out


def helper_ranges(generated, image_path, primary=True):
    """save_restore_helpers for a generated tree and its image."""
    base, size, code0, code_size = bounds(generated)
    with open(image_path, 'rb') as f:
        img = f.read()
    return save_restore_helpers(
        lambda a: int.from_bytes(img[a - base:a - base + 4], 'big'), code0, code0 + code_size,
        primary)


def find(generated, image_path, known=(), gaps=True):
    """The candidates, sorted, minus `known` (the hints already given). gaps: every start of
    uncovered code after code that does not fall through, pointed to or not."""
    base, size, code0, code_size = bounds(generated)
    code1 = code0 + code_size
    with open(image_path, 'rb') as f:
        img = f.read()
    if len(img) < code1 - base:
        raise SystemExit('%s holds %d bytes; the code ends at +%X' % (image_path, len(img), code1 - base))

    def word(a):
        return int.from_bytes(img[a - base:a - base + 4], 'big')

    tree = listing.load(generated)
    covered = bytearray((code1 - code0) // 4 + 1)
    for f, insns in tree.funcs.items():
        if not insns or not code0 <= f < code1:
            continue
        end = max(a for a, _ in insns)
        for a in range(f, min(end, code1 - 4) + 4, 4):
            covered[(a - code0) // 4] = 1
    thunks = import_thunks(generated)

    words = [int.from_bytes(img[i:i + 4], 'big') for i in range(0, len(img) - 3, 4)]
    pointed = set(w for w in words if code0 <= w < code1 and not w & 3)
    built = set()
    for i, w in enumerate(words):
        if w >> 26 != 15 or (w >> 16) & 31:  # lis rD,hi (addis rD,0,hi)
            continue
        rd, hi = (w >> 21) & 31, (w & 0xFFFF) << 16
        for v in words[i + 1:i + 33]:
            op = v >> 26
            if op == 14 and (v >> 16) & 31 == rd:  # addi rX,rD,lo
                built.add((hi + (v & 0xFFFF) - ((v & 0x8000) << 1)) & 0xFFFFFFFF)
            elif op == 24 and (v >> 21) & 31 == rd:  # ori rX,rD,lo
                built.add(hi | (v & 0xFFFF))

    def ends_flow(w):  # blr, bctr, zero padding, or an unconditional b without link
        return w in (0x4E800020, 0x4E800420, 0) or (w >> 26 == 18 and not w & 1)

    helpers = save_restore_helpers(word, code0, code1)
    found = []
    for a in range(code0, code1, 4):
        if covered[(a - code0) // 4] or word(a) == 0 or not ends_flow(word(a - 4)):
            continue
        if any(t <= a < t + 16 for t in thunks) or any(h0 <= a < h1 for h0, h1 in helpers):
            continue
        w = word(a)
        # A function with an exception handler has the handler's address and its data (the
        # C++ frame handler and the FuncInfo) in the two words before it, where .pdata's
        # exception flag points codegen: data, which decodes as `lwz rN,d(rM) / lwz r16,...`,
        # and was made a function falling into the real one (Midnight Club: Los Angeles's
        # 821322B0, whose build then failed on sub_821322B8). The function is the word after.
        if w in tree.funcs and base <= word(a + 4) < base + size:
            a += 8
            if a >= code1 or covered[(a - code0) // 4]:
                continue
            w = word(a)
        prologue = w == 0x7D8802A6 or w >> 16 == 0x9421  # mflr r12 / stwu r1,-N(r1)
        # gaps: also code nothing points to (Dungeons & Dragons: Chronicles of Mystara's
        # 822BC9D0, `lis r11,...` after a bctr, reached through an address it computes); the
        # runs codegen left between its functions are small leaf functions (a lone blr, a
        # getter), and one compiled for nothing costs nothing.
        if gaps or a in pointed or a in built or prologue:
            found.append(a)
    # Every entry of a second copy of a helper (save_restore_helpers, primary=False).
    for h0, h1 in save_restore_helpers(word, code0, code1, primary=False):
        found += [a for a in range(h0, h1 - 4, 4)]
    # Code codegen folded into a function across other functions: a `b` tail call it followed
    # (Double Dragon Neon's 823F5EE8, addi r3,r3,-4; b 823F5F00, took in 823F5F00, a getter
    # also reached through a vtable, past 823F5EF0 and 823F5EF8). The first instruction after a
    # gap holding another function's start is a function of its own, when it was reached by a
    # `b` straight to it, nothing falls into it, and something points to it (a cut inside a real
    # function would lose its non-volatile registers, locals here).
    import bisect
    starts = sorted(f for f in tree.funcs if code0 <= f < code1)
    for f, insns in tree.funcs.items():
        addrs = sorted(a for a, _ in insns)
        for prev, cur in zip(addrs, addrs[1:]):
            if (cur - prev <= 4 or cur in tree.funcs or cur not in pointed or
                    not ends_flow(word(cur - 4))):
                continue
            w = word(prev)
            if w >> 26 != 18 or w & 3 or (prev + (w & 0x3FFFFFC) - ((w & 0x2000000) << 1)) != cur:
                continue
            i = bisect.bisect_right(starts, prev)
            if i < len(starts) and starts[i] < cur and not any(h0 <= cur < h1 for h0, h1 in helpers):
                found.append(cur)
    # Functions the SDK replaces ([rexcrt], crt.toml) have no generated code: their insides are
    # not gaps.
    crt = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(generated))), 'crt.toml')
    if os.path.exists(crt):
        with open(crt, encoding='utf-8') as fh:
            replaced = [int(a, 16) for a in re.findall(r'= 0x([0-9A-Fa-f]{8})', fh.read())]
        spans = []
        for a in replaced:
            i = bisect.bisect_right(starts, a)
            spans.append((a, starts[i] if i < len(starts) else a + 4))
        found = [a for a in found if not any(s0 <= a < s1 for s0, s1 in spans)]
    known = set(known)
    return sorted(set(a for a in found if a not in known))


# The XDK compiler's jump table: a bound check, the table's address in two halves, the index times
# four, a load, a jump. Its operands, in the instructions' listing form.
# The listing leaves cr0 out (`cmplwi r3,5`, `bgt 0x8a601c04`): a missing field is cr0.
_CMPLWI = re.compile(r'^cmplwi (?:(cr\d),)?(r\d+),(\d+)$')
_BGT = re.compile(r'^bgt (?:(cr\d),)?0x[0-9a-f]+$')
_LIS = re.compile(r'^lis (r\d+),(-?\d+)$')
_ADDI = re.compile(r'^addi (r\d+),(r\d+),(-?\d+)$')
_LI = re.compile(r'^li (r\d+),(-?\d+)$')
_ADDIS = re.compile(r'^addis (r\d+),(r\d+),(-?\d+)$')
_SCALE = re.compile(r'^rlwinm (r\d+),(r\d+),2,0,29$')
_LWZX = re.compile(r'^lwzx (r\d+),(r\d+),(r\d+)$')
_MTCTR = re.compile(r'^mtctr (r\d+)$')
_MR = re.compile(r'^mr (r\d+),(r\d+)$')


def _far_bound(insns, i, index, back, far=200):
    """The bound of the bctr at insns[i] whose compare the compiler moved far ahead of it (Burnout
    Crash's crash.dll: `cmplwi cr2,r27,4` 94 instructions before its `bgt cr2`, stores between):
    from the last `bgt crN` before the bctr back to the nearest instruction that sets crN, which
    must be `cmplwi crN,<index>,N`, with nothing between it and the bctr writing the index (an
    instruction other than a store, compare or branch whose first operand it is). None when not
    so."""
    bgt = next((k for k in range(i - 1, max(-1, i - back), -1) if _BGT.match(insns[k][1])), None)
    if bgt is None:
        return None
    cr = _BGT.match(insns[bgt][1]).group(1) or 'cr0'
    for k in range(bgt - 1, max(-1, bgt - far), -1):
        t = insns[k][1]
        mn, _, ops = t.partition(' ')
        m = _CMPLWI.match(t)
        if m and (m.group(1) or 'cr0') == cr:
            if m.group(2) != index:
                return None
            for kk in range(k + 1, i):
                mn2, _, ops2 = insns[kk][1].partition(' ')
                # The table load itself may write the index register (`lwzx r3,r3,r5; mtctr
                # r3`: crash.dll's 8A360254, a 5-way switch 27 instructions after its
                # `cmplwi r3,4`): the index is used up there.
                if mn2 == 'lwzx' and kk == i - 2:
                    continue
                if (not mn2.startswith(('st', 'cmp', 'b', 'mtctr')) and
                        ops2.split(',')[0] == index and not
                        (mn2 == 'rlwinm' and _SCALE.match(insns[kk][1]))):
                    return None
            return int(m.group(3))
        # Anything else that sets crN: a compare, a record form on cr0, a CR operation.
        if (mn.startswith('cmp') and (ops.split(',')[0] if ops.startswith('cr') else 'cr0') == cr
                or mn.endswith('.') and cr == 'cr0' or mn.startswith(('cr', 'mtcr', 'mcrf'))):
            return None
    return None


def jump_tables(generated, image_path, back=24, all_tables=False):
    """The jump tables codegen cut short: [(bctr address, register, [targets], by_target)]. A table
    is read from the image with as many entries as its bound check allows (cmplwi rA,N; bgt:
    N + 1); it is cut short when one of its targets is not among the instructions codegen
    emitted for the function (Double Dragon Neon's 826B8920: a bound of 143 on r11 and the index
    in its copy r10, so codegen read 16 entries, emitted no code for the other cases, and traps
    on them). ReXGlue takes those from [[switch_tables]] (address: the bctr; register; labels).
    by_target: the index is overwritten before the bctr (the li + addis form), and the register is
    the one the target was loaded into."""
    base, size, code0, code_size = bounds(generated)
    with open(image_path, 'rb') as f:
        img = f.read()

    def word(a):
        return int.from_bytes(img[a - base:a - base + 4], 'big')

    tree = listing.load(generated)
    out = []
    for f, insns in tree.funcs.items():
        emitted = {a for a, _ in insns}
        for i, (addr, text) in enumerate(insns):
            if text.strip() != 'bctr' or i < 5:
                continue
            m = _MTCTR.match(insns[i - 1][1])
            if not m:
                continue
            target_reg = m.group(1)
            lwzx = idx_reg = table = None
            lo = hi = None
            bound = None
            # The table's address: `lis rT,hi` + `addi rT,rT,lo` in the load's first operand
            # (the index in the second), or `li rT,lo` + `addis rT,rT,hi` in its second, the
            # index in the first (Burnout Crash's crash.dll: li r27,11084; rlwinm r3,r3,2,0,29;
            # addis r27,r27,-30208; lwzx r3,r3,r27). The index is then overwritten before the
            # bctr, which codegen cannot switch on (its switch reads the index register at the
            # bctr): it took the bctr for a tail call and emitted none of the cases.
            form = None
            scales = {}  # the scaled index register: the register it was scaled from

            def scale_src():
                return scales.get(lwzx[0] if form == 'li' else lwzx[1])

            for j in range(i - 2, max(-1, i - back), -1):
                t = insns[j][1]
                if lwzx is None:
                    m = _LWZX.match(t)
                    if m and m.group(1) == target_reg:
                        lwzx = (m.group(2), m.group(3))
                    continue
                m = _ADDI.match(t)
                if (m and lo is None and form != 'li' and m.group(1) == lwzx[0] and
                        m.group(2) == lwzx[0]):
                    lo, form = int(m.group(3)), 'lis'
                    continue
                m = _LIS.match(t)
                if m and hi is None and form != 'li' and m.group(1) == lwzx[0]:
                    hi, form = int(m.group(2)), 'lis'
                    continue
                m = _ADDIS.match(t)
                if (m and hi is None and form is None and m.group(1) == lwzx[1] and
                        m.group(2) == lwzx[1] and lwzx[0] != lwzx[1]):
                    hi, form = int(m.group(3)), 'li'
                    continue
                m = _LI.match(t)
                if m and lo is None and form == 'li' and m.group(1) == lwzx[1]:
                    lo = int(m.group(2))
                    continue
                m = _SCALE.match(t)
                if m and m.group(1) in lwzx and m.group(1) not in scales:
                    scales[m.group(1)] = m.group(2)
                    continue
                m = _CMPLWI.match(t)
                if m and scale_src() and j + 1 < len(insns) and bound is None:
                    # the bound check on the index or the register it was copied from
                    regs = {scale_src()}
                    for k in range(max(0, i - back), i):  # copies either way, before or after
                        mm = _MR.match(insns[k][1])
                        if mm and mm.group(1) in regs:
                            regs.add(mm.group(2))
                        elif mm and mm.group(2) in regs:
                            regs.add(mm.group(1))
                    cr = m.group(1) or 'cr0'
                    if m.group(2) in regs and any(
                            _BGT.match(insns[k][1]) and
                            (_BGT.match(insns[k][1]).group(1) or 'cr0') == cr
                            for k in range(j + 1, i)):
                        bound = int(m.group(3))
                        break
            if None in (lo, hi) or scale_src() is None:
                continue
            by_target = form == 'li'
            if by_target and bound is None:
                bound = _far_bound(insns, i, scale_src(), back)
            table = ((hi << 16) + lo) & 0xFFFFFFFF
            if bound is None:
                # No bound check (the index from a mask: Bink's 8362CDC0 in The Bureau, r19 from
                # clrlwi 29 or 7 + clrlwi 30, so 0..10; codegen read 8): a table right after the
                # bctr ends where the first of its cases begins.
                if table != addr + 4 or by_target:
                    continue
                first = None
                k = 0
                while k < 1024 and (first is None or table + 4 * k < first):
                    t = word(table + 4 * k)
                    if not addr < t < code0 + code_size:
                        break
                    first = t if first is None else min(first, t)
                    k += 1
                if not k or table + 4 * k != first:
                    continue
                bound = k - 1
            if not base <= table < base + size - 4 * (bound + 1):
                continue
            targets = [word(table + 4 * k) for k in range(bound + 1)]
            if not all(code0 <= t < code0 + code_size for t in targets):
                continue
            if not all_tables and all(t in emitted for t in targets):
                continue
            # A table whose index is overwritten: the switch is on the loaded target, which a
            # hook at the bctr turns back into its index (project.write_switch_tables).
            out.append((addr, int((target_reg if by_target else scale_src())[1:]), targets,
                        by_target))
    return out


def setjmp_longjmp(generated, image_path):
    """(setjmps, longjmps, notes): every address the game calls the XDK CRT's setjmp and longjmp
    at, sorted (empty when not found). ReXGlue's codegen turns a `bl` to setjmp_address /
    longjmp_address into a host setjmp / longjmp (ppc_setjmp in the generated <name>_pch.h, a
    jmp_buf per guest jmp_buf); without them a longjmp is recompiled as the guest's register
    restore and a return, which goes back into the function that called longjmp with the stack
    pointer of the one that called setjmp (Super Meat Boy's libjpeg: error_exit's longjmp, then
    first_marker reading cinfo->src from the wrong frame, a null). All nine games tried have both.

    setjmp stores f14-f31 at 0, 8, 16, ... of its jmp_buf (stfd f14,0(r3); stfd f15,8(r3); ...)
    and longjmp loads them back from there (lfd f14,0(rX); ...); each is the function codegen
    made around that run. The addresses called are that function and the thunks to it (lis r11;
    addi r11,r11; mtctr r11; bctr, the incremental linker's: Super Meat Boy calls setjmp at
    826882B0 through 82117310 and 82702624), those some `bl` leads to; codegen takes them as a
    list (sdk/patches/rexnative/0003). notes: what was found, for the log."""
    base, size, code0, code_size = bounds(generated)
    code1 = code0 + code_size
    with open(image_path, 'rb') as f:
        img = f.read()

    def word(a):
        return int.from_bytes(img[a - base:a - base + 4], 'big')

    def runs(op):  # stfd (54) / lfd (50) f14..f17 at 0..24 of one register
        out = []
        for a in range(code0, code1 - 16, 4):
            w = word(a)
            if w >> 21 != (op << 5 | 14) or w & 0xFFFF:
                continue
            ra = (w >> 16) & 31
            if all(word(a + 4 * k) == (op << 26 | (14 + k) << 21 | ra << 16 | 8 * k)
                   for k in (1, 2, 3)):
                out.append(a)
        return out

    tree = listing.load(generated)
    spans = sorted((f, max(a for a, _ in insns)) for f, insns in tree.funcs.items() if insns)
    starts = [f for f, _ in spans]

    def owner(a):
        import bisect
        i = bisect.bisect_right(starts, a) - 1
        return spans[i][0] if i >= 0 and spans[i][0] <= a <= spans[i][1] else None

    calls = {}
    thunks = {}
    for a in range(code0, code1 - 12, 4):
        w = word(a)
        if w >> 26 == 18 and w & 3 == 1:  # bl
            t = (a + ((w & 0x3FFFFFC) - ((w & 0x2000000) << 1))) & 0xFFFFFFFF
            calls[t] = calls.get(t, 0) + 1
        elif w >> 16 == 0x3D60 and word(a + 4) >> 16 == 0x396B and word(a + 8) == 0x7D6903A6 \
                and word(a + 12) == 0x4E800420:  # lis r11; addi r11,r11; mtctr r11; bctr
            lo = word(a + 4) & 0xFFFF
            t = ((w & 0xFFFF) << 16) + lo - ((lo & 0x8000) << 1) & 0xFFFFFFFF
            thunks.setdefault(t, []).append(a)

    notes = []

    def called(op, what):
        out = []
        for f in sorted({owner(a) for a in runs(op)} - {None}):
            for c in [f] + thunks.get(f, []):
                n = calls.get(c, 0)
                notes.append('%s %08X%s: %d calls' % (what, c, '' if c == f else ' (thunk to %08X)' % f, n))
                if n:
                    out.append(c)
        return sorted(out)

    return called(54, 'setjmp'), called(50, 'longjmp'), notes


_LIS_ANY = re.compile(r'^lis (r\d+),(-?\d+)$')
_ADDI_ANY = re.compile(r'^addi (r\d+),(r\d+),(-?\d+)$')


def xapi_fibers(generated):
    """The game's XAPI fiber functions, {name: address} for ReXGlue's [rexcrt] table (the
    SDK's host-fiber versions, kernel/crt/threading.cpp, replace them), or {} unless all five
    are found: the host SwitchToFiber only knows fibers the host CreateFiber /
    ConvertThreadToFiber made. Recompiled as code, SwitchToFiber saves the registers into the
    current fiber and loads the next one's stack pointer and return address, which a
    recompiled function cannot follow (Double Dragon Neon's Play wrote into another fiber's
    stack). By shape, all reading the current thread (lwz rT,256(r13)) and its fiber (+356):
    ConvertThreadToFiber sets ERROR_ALREADY_FIBER (li r3,1280), ConvertFiberToThread
    ERROR_ALREADY_THREAD (li r3,1281), DeleteFiber compares the fiber with its argument (cmplw
    ...,r3); SwitchToFiber's body starts lwz r4,256(r13); lwz r5,356(r4); mfcr; mflr (its
    public entry may be a lone `b` to it); CreateFiber builds the address of the new fiber's
    start-up code, the instructions right before that body."""
    tree = listing.load(generated)
    funcs = {f: [(a, t.strip()) for a, t in insns] for f, insns in tree.funcs.items() if insns}
    body = None
    for f, insns in funcs.items():
        for i in range(len(insns) - 3):
            if (re.match(r'lwz (r\d+),256\(r13\)$', insns[i][1]) and
                    re.match(r'lwz r\d+,356\(%s\)$' % insns[i][1].split()[1].split(',')[0],
                             insns[i + 1][1]) and
                    insns[i + 2][1].startswith('mfcr') and insns[i + 3][1].startswith('mflr')):
                body = insns[i][0]
                break
        if body:
            break
    if not body:
        return {}
    out = {}
    for f, insns in funcs.items():
        texts = [t for _, t in insns]
        thread = {m.group(1) for m in (re.match(r'lwz (r\d+),256\(r13\)$', t) for t in texts) if m}
        fiber = any(re.match(r'(lwz|stw) r\d+,356\((%s)\)$' % '|'.join(thread), t)
                    for t in texts) if thread else False
        if len(texts) == 1 and texts[0] == 'b 0x%08x' % body:
            out['SwitchToFiber'] = f
        if fiber and 'li r3,1280' in texts:
            out['ConvertThreadToFiber'] = f
        elif fiber and 'li r3,1281' in texts:
            out['ConvertFiberToThread'] = f
        elif fiber and any(re.match(r'cmplw cr\d,r\d+,r3$', t) for t in texts) and len(texts) < 64:
            out['DeleteFiber'] = f
        # CreateFiber: lis rX,hi ... addi rY,rX,lo with the address just before the body.
        his = {}
        for t in texts:
            m = _LIS_ANY.match(t)
            if m:
                his[m.group(1)] = int(m.group(2))
                continue
            m = _ADDI_ANY.match(t)
            if m and m.group(2) in his:
                a = ((his[m.group(2)] << 16) + int(m.group(3))) & 0xFFFFFFFF
                if body - 0x40 <= a < body and f != body:
                    out['CreateFiber'] = f
    out.setdefault('SwitchToFiber', body)
    names = ('ConvertThreadToFiber', 'ConvertFiberToThread', 'CreateFiber', 'DeleteFiber',
             'SwitchToFiber')
    return {n: out[n] for n in names} if all(n in out for n in names) else {}
