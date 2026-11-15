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


def save_restore_helpers(word, code0, code1):
    """The XDK's register save / restore helpers (__savegprlr_14, __restgprlr_14, __savefpr_14,
    __restfpr_14, __savevmx_14/64, __restvmx_14/64), each from its first instruction to the blr
    that ends it, found by the patterns codegen uses (phase_register.cpp,
    detectSaveRestoreHelpers). Codegen turns a call into one (at any of its entries) into saves
    and restores of its own; made a function by a hint (.pdata points at them), the call stays a
    call, and with non_volatile_as_local the caller's f14-f31 / v14-v127 are not kept (Jet Set
    Radio's garbage function pointers). Never candidates."""
    starts = []
    for a in range(code0, code1 - 4, 4):
        w = word(a)
        if w in (0xE9C1FF68, 0xF9C1FF68, 0xC9CCFF70, 0xD9CCFF70):  # ld/std r14 / lfd/stfd f14
            starts.append(a)
        elif w in (0x3960FEE0, 0x3960FC00) and word(a + 4) in (
                0x7DCB60CE, 0x7DCB61CE, 0x100B60CB, 0x100B61CB):  # li r11 + lvx/stvx(128)
            starts.append(a)
    out = []
    for s in starts:
        e = s
        while e < code1 and word(e) != 0x4E800020:  # to the blr
            e += 4
        out.append((s, e + 4))
    return out


def helper_ranges(generated, image_path):
    """save_restore_helpers for a generated tree and its image."""
    base, size, code0, code_size = bounds(generated)
    with open(image_path, 'rb') as f:
        img = f.read()
    return save_restore_helpers(
        lambda a: int.from_bytes(img[a - base:a - base + 4], 'big'), code0, code0 + code_size)


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
        prologue = w == 0x7D8802A6 or w >> 16 == 0x9421  # mflr r12 / stwu r1,-N(r1)
        # gaps: also code nothing points to (Dungeons & Dragons: Chronicles of Mystara's
        # 822BC9D0, `lis r11,...` after a bctr, reached through an address it computes); the
        # runs codegen left between its functions are small leaf functions (a lone blr, a
        # getter), and one compiled for nothing costs nothing.
        if gaps or a in pointed or a in built or prologue:
            found.append(a)
    known = set(known)
    return [a for a in found if a not in known]


# The XDK compiler's jump table: a bound check, the table's address in two halves, the index times
# four, a load, a jump. Its operands, in the instructions' listing form.
_CMPLWI = re.compile(r'^cmplwi (cr\d),(r\d+),(\d+)$')
_BGT = re.compile(r'^bgt (cr\d),0x[0-9a-f]+$')
_LIS = re.compile(r'^lis (r\d+),(-?\d+)$')
_ADDI = re.compile(r'^addi (r\d+),(r\d+),(-?\d+)$')
_SCALE = re.compile(r'^rlwinm (r\d+),(r\d+),2,0,29$')
_LWZX = re.compile(r'^lwzx (r\d+),(r\d+),(r\d+)$')
_MTCTR = re.compile(r'^mtctr (r\d+)$')
_MR = re.compile(r'^mr (r\d+),(r\d+)$')


def jump_tables(generated, image_path, back=24):
    """The jump tables codegen cut short: [(bctr address, index register, [targets])]. A table
    is read from the image with as many entries as its bound check allows (cmplwi rA,N; bgt:
    N + 1); it is cut short when one of its targets is not among the instructions codegen
    emitted for the function (Double Dragon Neon's 826B8920: a bound of 143 on r11 and the index
    in its copy r10, so codegen read 16 entries, emitted no code for the other cases, and traps
    on them). ReXGlue takes those from [[switch_tables]] (address: the bctr; register; labels)."""
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
            lwzx = idx_reg = table = scale_src = None
            lo = hi = None
            bound = None
            for j in range(i - 2, max(-1, i - back), -1):
                t = insns[j][1]
                if lwzx is None:
                    m = _LWZX.match(t)
                    if m and m.group(1) == target_reg:
                        lwzx = (m.group(2), m.group(3))  # (table register, scaled index)
                    continue
                m = _ADDI.match(t)
                if m and lo is None and m.group(1) == lwzx[0] and m.group(2) == lwzx[0]:
                    lo = int(m.group(3))
                    continue
                m = _LIS.match(t)
                if m and hi is None and m.group(1) == lwzx[0]:
                    hi = int(m.group(2))
                    continue
                m = _SCALE.match(t)
                if m and scale_src is None and m.group(1) == lwzx[1]:
                    scale_src = m.group(2)
                    continue
                m = _CMPLWI.match(t)
                if m and scale_src and j + 1 < len(insns) and bound is None:
                    # the bound check on the index or the register it was copied from
                    regs = {scale_src}
                    for k in range(max(0, i - back), i):  # copies either way, before or after
                        mm = _MR.match(insns[k][1])
                        if mm and mm.group(1) in regs:
                            regs.add(mm.group(2))
                        elif mm and mm.group(2) in regs:
                            regs.add(mm.group(1))
                    if m.group(2) in regs and any(_BGT.match(insns[k][1]) and
                                                  _BGT.match(insns[k][1]).group(1) == m.group(1)
                                                  for k in range(j + 1, i)):
                        bound = int(m.group(3))
                        break
            if None in (lo, hi, bound) or scale_src is None:
                continue
            table = ((hi << 16) + lo) & 0xFFFFFFFF
            if not base <= table < base + size - 4 * (bound + 1):
                continue
            targets = [word(table + 4 * k) for k in range(bound + 1)]
            if not all(code0 <= t < code0 + code_size for t in targets):
                continue
            if all(t in emitted for t in targets):
                continue
            out.append((addr, int(scale_src[1:]), targets))
    return out
