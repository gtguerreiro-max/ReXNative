"""ReXNative's resolves against the stock renderer's, side by side, from a resolve dump.

    python scripts/resolves.py <dump dir> [--frame N] [--only 3,7,12] [--scale 1]

A dump comes from a game run with --rexnative_beside_xenos=true and
--rexnative_resolve_dump=<dir>, after <dir>\\dump.request is created: each resolve of one frame
as f<frame>-r<i>-native.bin (ReXNative's destination texture, linear, in its host format) and
f<frame>-r<i>-guest.bin (guest memory at the destination after the frame, which the Xenos
plugin wrote: tiled and byte swapped, in the guest format), described in f<frame>-resolves.jsonl.
For each 32-bit and 64-bit color resolve this writes f<frame>-r<i>.png (ReXNative left, stock
right, at the destination's size) and prints how far apart they are: the first resolve that
differs is where a pass went wrong. Needs numpy and Pillow.

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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir')
    ap.add_argument('--frame', type=int)
    ap.add_argument('--only', help='resolve indices, comma separated')
    a = ap.parse_args()
    metas = sorted(f for f in os.listdir(a.dir) if f.endswith('-resolves.jsonl'))
    if a.frame is not None:
        metas = [f for f in metas if f.startswith('f%d-' % a.frame)]
    if not metas:
        raise SystemExit('no resolve dump in ' + a.dir)
    meta = metas[-1]
    prefix = meta[:-len('-resolves.jsonl')]
    only = {int(i) for i in a.only.split(',')} if a.only else None
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
        print('r%-3d fmt %-2d %4dx%-4d mean |diff| %.4f  native mean %.4f  stock mean %.4f  %s' % (
            i, m['dest_format'], w, h, diff, n_rgb.mean(), g_rgb.mean(), stem + '.png'))


if __name__ == '__main__':
    main()
