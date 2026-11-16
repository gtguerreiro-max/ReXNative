"""Guest data constants the renderer reads: the default clear colour of Resolve.

When Resolve's pClearColor is NULL the XDK D3D clears with a constant from the executable's
data: PGR4's (XDK 6132) loads one float (`lis; lfs f31,-30196(r11)` = 0x82258A0C) and stores
it four times; XDK 20353's inner Resolve copies a 16-byte vector (`lis; addi; lvx128`): From
Dust 0x82007F50, Iron Brigade 0x8202EED0. Found here by that shape: the first `lfs` from a
constant address, or constant address whose `lvx`/`lvx128` follows, after a null check of a
pointer, in the first quarter of the function.
"""
import re

from .abi import _ops, _writes

LIS = re.compile(r'^lis (r\d+),(-?\d+)$')
DFORM = re.compile(r'^(-?\d+)\((r\d+)\)$')


def data_refs(insns):
    """[(index, address, kind)] of constant data addresses (lis + displacement / addi)."""
    out = []
    hi = {}
    for i, (_, text) in enumerate(insns):
        m = LIS.match(text)
        if m:
            hi[m.group(1)] = (int(m.group(2)) << 16) & 0xFFFFFFFF
            continue
        mn, ops = _ops(text)
        if mn == 'addi' and len(ops) == 3 and ops[1] in hi:
            addr = (hi[ops[1]] + int(ops[2])) & 0xFFFFFFFF
            kind = 'address'
            for _, nxt in insns[i + 1:i + 6]:
                nm, nops = _ops(nxt)
                if nm.startswith('lvx') and ops[0] in nops:
                    kind = 'vector'
                    break
            out.append((i, addr, kind))
        elif len(ops) == 2 and DFORM.match(ops[1]) and DFORM.match(ops[1]).group(2) in hi:
            d = DFORM.match(ops[1])
            addr = (hi[d.group(2)] + int(d.group(1))) & 0xFFFFFFFF
            out.append((i, addr, 'float' if mn == 'lfs' else mn))
        dest = _writes(mn, ops)
        if dest in hi and not (mn == 'addi' and dest == ops[1] and False):
            hi.pop(dest, None)
    return out


def default_clear_color(insns):
    """{'address', 'vector', 'index'} or None."""
    limit = max(64, len(insns) // 4)
    for i, addr, kind in data_refs(insns[:limit]):
        if not 0x82000000 <= addr < 0x8A000000:
            continue
        if kind == 'float':
            # Splatted into four consecutive stack words (stfs f,192/196/200/204(r1)).
            stores = [t for _, t in insns[i + 1:i + 10] if t.startswith('stfs ')]
            if len(stores) >= 4:
                return {'address': addr, 'vector': False, 'index': i}
        elif kind == 'vector':
            return {'address': addr, 'vector': True, 'index': i}
    return None


LWZ = re.compile(r'^lwz (r\d+),(-?\d+)\((r\d+)\)$')
ORI_BIN_MASK_LO = re.compile(r'^ori (r\d+),\1,24576$')  # with lis rN,-16384: 0xC0006000


def bin_mask_offset(T):
    """The D3D device's bin mask offset: where the segment start reads the two words it writes
    after SET_BIN_MASK_LO / _HI (0xC0006000, 0xC0006100), `lwz rN,off(rDevice)` then
    `lwz rM,off+4(rDevice)` (Iron Brigade's and From Dust's 2.0.20353: 12956; PGR4's 6132:
    12700; Midnight Club: Los Angeles's 6995: 12708). A replayed command buffer starts from that
    mask (cmd_replay.cpp): read at the wrong offset, Midnight Club's predicated draws were all
    skipped. The most common offset found, or None."""
    from collections import Counter
    found = Counter()
    for insns in T.funcs.values():
        for i, (_, text) in enumerate(insns):
            if not ORI_BIN_MASK_LO.match(text):
                continue
            for j in range(i + 1, min(i + 9, len(insns))):
                m = LWZ.match(insns[j][1])
                if not m or not 8000 <= int(m.group(2)) <= 20000:
                    continue
                off, dev = int(m.group(2)), m.group(3)
                if any((LWZ.match(t) and LWZ.match(t).group(2) == str(off + 4) and
                        LWZ.match(t).group(3) == dev) for _, t in insns[j + 1:j + 8]):
                    found[off] += 1
                break
    return found.most_common(1)[0][0] if found else None
