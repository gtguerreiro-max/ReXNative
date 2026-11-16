"""ReXNative render target snapshots (draw-debug.txt mask 256, scripts/live.ps1 snap) as images.

    python scripts/snaps.py <capture dir> [--frame N] [--rows 720] [--out grid.png]

Each snapshot is snap-f<frame>-d<draw>-<target>.bin with a .txt holding
"width height row_bytes dxgi_format". RGBA8 targets (DXGI 27-29) and RGBA16F ones (DXGI 10: the 7e3 and float
targets, clamped to 0-1) become one PNG each, next to the .bin, and one grid of all of them in
draw order; other formats (depth) are listed only. Needs Pillow, and numpy for RGBA16F.
"""
import argparse
import glob
import os
import re

from PIL import Image, ImageDraw

RGBA8 = {27, 28, 29}  # R8G8B8A8 typeless, unorm, unorm sRGB
RGBA16F = 10  # R16G16B16A16_FLOAT


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir')
    ap.add_argument('--frame', type=int, help='only this frame (default: the latest)')
    ap.add_argument('--rows', type=int, default=720, help='rows kept (targets are EDRAM-tall)')
    ap.add_argument('--out', help='grid path (default: <dir>/snaps-f<frame>.png)')
    a = ap.parse_args()
    found = []
    for txt in glob.glob(os.path.join(a.dir, 'snap-f*-d*-*.txt')):
        m = re.search(r'snap-f(\d+)-d(\d+)-(\w+)\.txt$', txt)
        if m:
            found.append((int(m.group(1)), int(m.group(2)), m.group(3), txt))
    if not found:
        raise SystemExit('no snapshots in ' + a.dir)
    frame = a.frame if a.frame is not None else max(f for f, _, _, _ in found)
    tiles = []
    for f, draw, target, txt in sorted(x for x in found if x[0] == frame):
        w, h, row_bytes, fmt = map(int, open(txt).read().split())
        if fmt not in RGBA8 and fmt != RGBA16F:
            print('%s: format %d, not converted' % (os.path.basename(txt), fmt))
            continue
        rows = min(h, a.rows)
        data = open(txt[:-4] + '.bin', 'rb').read()
        if fmt == RGBA16F:
            import numpy as np
            px = np.frombuffer(data[:row_bytes * rows], dtype='<f2').reshape(rows, row_bytes // 2)
            px = px[:, :w * 4].reshape(rows, w, 4)[:, :, :3].astype(np.float32)
            px = np.nan_to_num(px, nan=1.0, posinf=1.0, neginf=0.0)
            img = Image.fromarray((np.clip(px, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8), 'RGB')
        else:
            img = Image.frombuffer('RGBA', (w, rows), data[:row_bytes * rows], 'raw', 'RGBA',
                                   row_bytes, 1).convert('RGB')
        png = txt[:-4] + '.png'
        img.save(png)
        print(png)
        tile = img.resize((480, int(480 * rows / w)))
        ImageDraw.Draw(tile).text((4, 4), 'draw %d %s' % (draw, target), fill=(255, 0, 255))
        tiles.append(tile)
    if not tiles:
        return
    cols = min(4, len(tiles))
    th = max(t.height for t in tiles)
    grid = Image.new('RGB', (cols * 480, ((len(tiles) + cols - 1) // cols) * th))
    for i, t in enumerate(tiles):
        grid.paste(t, ((i % cols) * 480, (i // cols) * th))
    out = a.out or os.path.join(a.dir, 'snaps-f%d.png' % frame)
    grid.save(out)
    print(out)


if __name__ == '__main__':
    main()
