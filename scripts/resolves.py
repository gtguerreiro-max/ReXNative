"""ReXNative's resolves against the stock renderer's, side by side, from a resolve dump.

    python scripts/resolves.py <dump dir> [--frame N] [--only 3,7,12] [--over 0.02]

A dump comes from a game run with --rexnative_beside_xenos=true and
--rexnative_resolve_dump=<dir>, after <dir>\\dump.request is created: each resolve of one frame
as f<frame>-r<i>-native.bin (ReXNative's destination texture, linear, in its host format) and
f<frame>-r<i>-guest.bin (guest memory at the destination after the frame, which the Xenos
plugin wrote: tiled and byte swapped, in the guest format), described in f<frame>-resolves.jsonl.
For each 32-bit and 64-bit color resolve this writes f<frame>-r<i>.png (ReXNative left, stock
right, at the destination's size) and prints how far apart they are: the first resolve that
differs is where a pass went wrong. With --over only the resolves further apart than that are
printed, then a line with how many were compared (live.ps1's `oracle`). Needs numpy and Pillow.

    python scripts/resolves.py <dump dir> --against <other dump dir> [--over 0.02]

compares ReXNative's own resolves of two dumps instead (the latest frame of each): the game with a
fault against a played frame trace of the same moment without it (live.ps1 `resolves`), resolve
by resolve in order, each pair at the same destination address. The first pair apart is the pass
that the fault is in; f<frame>-r<i>-against.png shows the two, this dump's left.

The guest side is untiled with Xenos's 2D tiling (XGAddress2DTiledOffset, as ReXGlue's
texture_address.h) and swapped by the destination endian (1 8-in-16, 2 8-in-32, 3 16-in-32).
"""
import argparse
import json
import os

import numpy as np
from PIL import Image

# Guest texture formats (xenos::TextureFormat) a resolve writes, with bytes per texel and how
# to show them: 'u8' four 8-bit unorm channels, 'u1010102' 10:10:10:2, 'u16' / 'f16' four 16-bit.
FORMATS = {6: (4, 'u8'), 7: (4, 'u1010102'), 26: (8, 'u16'), 32: (8, 'f16')}


def tiled_offset(x, y, pitch, log2_bpp):
    """Byte offset of texel (x, y) in a tiled 2D surface `pitch` texels wide (32-aligned)."""
    macro = ((y >> 5) * (pitch >> 5)) << (log2_bpp + 7)
    micro = ((y & 6) << 2) << log2_bpp
    row = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 8) << (3 + log2_bpp)) + ((y & 1) << 4)
    macro = (x >> 5) << (log2_bpp + 7)
    micro = (x & 7) << log2_bpp
    off = row + macro + ((micro & ~0xF) << 1) + (micro & 0xF)
    return (((off & ~0x1FF) << 3) + ((off & 0x1C0) << 2) + (off & 0x3F) + ((y & 16) << 7) +
            (((((y & 8) >> 2) + (x >> 3)) & 3) << 6))


def untile(data, width, height, pitch, bpp, tiled):
    log2 = {4: 2, 8: 3}[bpp]
    out = np.zeros((height, width, bpp), np.uint8)
    buf = np.frombuffer(data, np.uint8)
    ys, xs = np.mgrid[0:height, 0:width]
    if tiled:
        offs = np.vectorize(tiled_offset, otypes=[np.int64])(xs, ys, pitch, log2)
    else:
        offs = (ys * pitch + xs) * bpp
    valid = offs + bpp <= len(buf)
    for b in range(bpp):
        o = np.where(valid, offs + b, 0)
        out[..., b] = np.where(valid, buf[o], 0)
    return out


def swap(t, endian):
    if endian == 1:  # 8 in 16
        return t.reshape(t.shape[:-1] + (-1, 2))[..., ::-1].reshape(t.shape)
    if endian == 2:  # 8 in 32
        return t.reshape(t.shape[:-1] + (-1, 4))[..., ::-1].reshape(t.shape)
    if endian == 3:  # 16 in 32
        return t.reshape(t.shape[:-1] + (-1, 2, 2))[..., ::-1, :].reshape(t.shape)
    return t


def to_rgb(texels, kind):
    """Texels (h, w, bytes) in little-endian channel order -> float RGB 0..1 for display."""
    texels = np.ascontiguousarray(texels)
    if kind == 'u8':
        return texels[..., :3].astype(np.float32) / 255.0
    v = texels.view('<u4')[..., 0] if kind == 'u1010102' else None
    if kind == 'u1010102':
        return np.stack([(v >> s) & 0x3FF for s in (0, 10, 20)], -1).astype(np.float32) / 1023.0
    words = texels.view('<u2')
    if kind == 'u16':
        return words[..., :3].astype(np.float32) / 65535.0
    return np.clip(np.nan_to_num(words.view('<f2')[..., :3].astype(np.float32)), 0.0, 1.0)


def load_native(folder, prefix, m):
    """A dump's native image of resolve `m` as (h, w, bytes), or None."""
    bpp = FORMATS.get(m['dest_format'], (None,))[0]
    w, h = m['native_width'], m['native_height']
    path = os.path.join(folder, '%s-r%d-native.bin' % (prefix, m['index']))
    if not os.path.exists(path):
        return None
    data = np.fromfile(path, np.uint8)
    bpp = bpp or (data.size // (w * h) if w * h else 0)
    if not bpp or data.size < w * h * bpp:
        return None
    return data[:w * h * bpp].reshape(h, w, bpp)


def against(a, meta, prefix):
    """This dump's native resolves against another dump's, in order."""
    others = sorted(f for f in os.listdir(a.against) if f.endswith('-resolves.jsonl'))
    if not others:
        raise SystemExit('no resolve dump in ' + a.against)
    other_prefix = others[-1][:-len('-resolves.jsonl')]
    mine = [json.loads(l) for l in open(os.path.join(a.dir, meta))]
    theirs = [json.loads(l) for l in open(os.path.join(a.against, others[-1]))]
    over = 0.02 if a.over is None else a.over
    compared, apart, unmatched = 0, [], 0
    # The k-th resolve of a source into a destination against the other dump's k-th: two frames
    # of one scene need not hold the same resolves in the same order.
    def key(m):
        return (m['dest_base'], m['dest_format'], m['native_width'], m['native_height'], m['source'])

    pool = {}
    for t in theirs:
        pool.setdefault(key(t), []).append(t)
    for m in mine:
        t = pool.get(key(m), [None]).pop(0) if pool.get(key(m)) else None
        x = load_native(a.dir, prefix, m)
        y = load_native(a.against, other_prefix, t) if t else None
        if x is None or y is None or x.shape != y.shape:
            unmatched += 1
            continue
        compared += 1
        kind = FORMATS.get(m['dest_format'], (0, None))[1]
        if kind and m['source'] != 4:
            xr, yr = to_rgb(x, kind), to_rgb(y, kind)
        else:  # depth and other formats: their bytes
            xr, yr = x.astype(np.float32) / 255.0, y.astype(np.float32) / 255.0
        if kind in ('f16', 'u1010102'):
            # Linear light: as a display shows it (a dark fault is a small number there).
            xr, yr = np.sqrt(np.clip(xr, 0, 1)), np.sqrt(np.clip(yr, 0, 1))
        diff = float(np.abs(xr - yr).mean())
        if diff <= over:
            continue
        apart.append(m['index'])
        out = ''
        if xr.shape[-1] == 3:
            out = os.path.join(a.dir, '%s-r%d-against.png' % (prefix, m['index']))
            img = np.concatenate([xr, yr], 1)
            Image.fromarray((np.clip(img, 0, 1) * 255 + 0.5).astype(np.uint8), 'RGB').save(out)
        print('r%-3d %s -> %08X fmt %-2d %4dx%-4d mean |diff| %.4f  mean %.4f against %.4f  %s' % (
            m['index'], 'depth' if m['source'] == 4 else 'c%d' % m['source'], m['dest_base'],
            m['dest_format'], m['native_width'], m['native_height'], diff, xr.mean(), yr.mean(), out))
    print('%s against %s: %d resolves compared, %d further apart than %g%s%s' % (
        prefix, other_prefix, compared, len(apart), over,
        ' (first: r%d)' % apart[0] if apart else '',
        ', %d without its pair in the other' % unmatched if unmatched else ''))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir')
    ap.add_argument('--frame', type=int)
    ap.add_argument('--only', help='resolve indices, comma separated')
    ap.add_argument('--against', help="another dump's folder: its native resolves against this one's")
    ap.add_argument('--over', type=float, help='print only the resolves whose mean difference is above this')
    a = ap.parse_args()
    metas = sorted(f for f in os.listdir(a.dir) if f.endswith('-resolves.jsonl'))
    if a.frame is not None:
        metas = [f for f in metas if f.startswith('f%d-' % a.frame)]
    if not metas:
        raise SystemExit('no resolve dump in ' + a.dir)
    meta = metas[-1]
    prefix = meta[:-len('-resolves.jsonl')]
    only = {int(i) for i in a.only.split(',')} if a.only else None
    compared, apart = 0, []
    if a.against:
        return against(a, meta, prefix)
    for line in open(os.path.join(a.dir, meta)):
        m = json.loads(line)
        i = m['index']
        if (only and i not in only) or m['source'] == 4 or m['dest_format'] not in FORMATS:
            continue
        bpp, kind = FORMATS[m['dest_format']]
        stem = os.path.join(a.dir, '%s-r%d' % (prefix, i))
        w, h = m['native_width'], m['native_height']
        native = np.fromfile(stem + '-native.bin', np.uint8)
        if native.size < w * h * bpp:
            continue
        native = native[:w * h * bpp].reshape(h, w, bpp)
        guest = untile(open(stem + '-guest.bin', 'rb').read(), w, h, m['pitch_blocks'], bpp,
                       m['tiled'])
        guest = swap(guest, m['endian'])
        n_rgb, g_rgb = to_rgb(native, kind), to_rgb(guest, kind)
        diff = float(np.abs(n_rgb - g_rgb).mean())
        img = np.concatenate([n_rgb, g_rgb], 1)
        Image.fromarray((np.clip(img, 0, 1) * 255 + 0.5).astype(np.uint8), 'RGB').save(stem + '.png')
        compared += 1
        if a.over is not None:
            if diff <= a.over:
                os.remove(stem + '.png')
                continue
            apart.append(i)
        print('r%-3d fmt %-2d %4dx%-4d mean |diff| %.4f  native mean %.4f  stock mean %.4f  %s' % (
            i, m['dest_format'], w, h, diff, n_rgb.mean(), g_rgb.mean(), stem + '.png'))
    if a.over is not None:
        print('%s: %d resolves compared, %d further apart than %g%s' % (
            prefix, compared, len(apart), a.over, ' (first: r%d)' % apart[0] if apart else ''))


if __name__ == '__main__':
    main()
