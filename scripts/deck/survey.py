"""The Deck's game logs: per session, its gameplay windows (150 frames each, the most draws), the
game's target (30 or 60 fps), the share below it, and the GPU's time per frame where it was timed."""
import glob
import os
import re
import statistics
import sys

root = sys.argv[1]
out = []
for p in sorted(glob.glob(os.path.join(root, '*', 'logs', '*.log'))):
    game = os.path.normpath(p).split(os.sep)[-3]
    rows, gpu, up, scale = [], [], '', 1
    date = ''
    for l in open(p, encoding='utf-8', errors='replace'):
        if not date and l[:4] == '2026':
            date = l[5:16]
        m = re.search(r'ReXNative: frames (\d+) \|.*?draws ([0-9.]+).*?draw CPU ([0-9.]+) us.*?frame ([0-9.]+) ms, '
                      r'game thread ([0-9.]+) us \(waiting ([0-9.]+) us\)', l)
        if m:
            rows.append(tuple(float(x) for x in m.groups()))
            continue
        g = re.search(r'ReXNative GPU: frames \d+ \| GPU ([0-9.]+) ms/frame', l)
        if g:
            gpu.append(float(g.group(1)))
        u = re.search(r'ReXNative upscaler: (.*?) \(', l)
        if u and not up:
            up = u.group(1)[:44]
        s = re.search(r'surfaces at (\d)x', l)
        if s:
            scale = int(s.group(1))
    if len(rows) < 20:
        continue
    late = [r for r in rows if r[0] > 1500]
    if not late:
        continue
    top = max(r[1] for r in late)
    play = [r for r in late if r[1] >= 0.3 * top and r[1] > 20]
    if len(play) < 8:
        continue
    frames = [r[3] for r in play]
    med = statistics.median(frames)
    target = 33.34 if med > 24 else 16.67
    slow = [f for f in frames if f > target * 1.04]
    out.append((len(slow) / len(frames), game, date, len(play), target, med, max(frames),
                statistics.mean(r[2] for r in play) / 1000, statistics.mean(r[1] for r in play),
                statistics.mean(gpu) if gpu else None, up, scale, len(rows)))
print('%-34s %-11s %5s %4s %5s %6s %6s %7s %6s %7s %s' % ('game', 'date', 'wins', 'fps', 'slow%', 'median', 'worst',
                                                          'drawCPU', 'draws', 'GPUms', 'upscaler'))
for o in sorted(out, reverse=True):
    share, game, date, n, target, med, worst, dcpu, draws, g, up, scale, total = o
    print('%-34s %-11s %5d %4d %4.0f%% %6.1f %6.1f %6.1fms %6.0f %7s %s%s' % (
        game[:34], date, n, round(1000 / target), 100 * share, med, worst, dcpu, draws,
        '%.1f' % g if g is not None else '-', up, ' (scale %dx)' % scale if scale > 1 else ''))
