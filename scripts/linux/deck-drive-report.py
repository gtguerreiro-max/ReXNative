#!/usr/bin/env python3
"""The frames of a deck-drive-latency.sh session, by ReXNative's frame latency.

    deck-drive-report.py <test dir> [heavy draws, default 2500]

From the test dir's newest log and per-frame CSV (--rexnative_csv), inside the time the session
alternated the latency (logs/drive-power.txt): every frame whose scene had at least the heavy
draws, put under the latency the frame lines around it name (the frames of a window in which the
latency changed are left out). Frame time is the game's, Swap to Swap.
"""
import bisect
import csv
import datetime
import glob
import os
import re
import sys

test = sys.argv[1]
heavy = int(sys.argv[2]) if len(sys.argv) > 2 else 2500
logs = os.path.join(test, 'logs')


def newest(pattern, skip=None):
    files = [f for f in glob.glob(os.path.join(logs, pattern)) if not (skip and skip in f)]
    return max(files, key=os.path.getmtime) if files else None


power = []
with open(os.path.join(logs, 'drive-power.txt')) as f:
    for line in f:
        t, lat, mw, busy, mhz = line.split()
        power.append((int(t), int(lat), int(mw), int(busy), int(mhz)))
t0, t1 = power[0][0] - 2, power[-1][0] + 2

stat = re.compile(r'ReXNative: frames (\d+) \| per frame: draws ([\d.]+),.*frame ([\d.]+) ms,'
                  r'.*begin to shown ([\d.]+) ms \(latency (\d+)\)')
gpu = re.compile(r'ReXNative GPU: frames (\d+) \| GPU ([\d.]+) ms/frame')
lines, gpus = [], []
log = newest('*.log')
with open(log, errors='replace') as f:
    for line in f:
        try:
            when = datetime.datetime.strptime(line[:19], '%Y-%m-%d %H:%M:%S').timestamp()
        except ValueError:
            continue
        if not t0 <= when <= t1:
            continue
        m = stat.search(line)
        if m:
            lines.append((int(m[1]), float(m[2]), float(m[3]), float(m[4]), int(m[5])))
            continue
        m = gpu.search(line)
        # The GPU lines count frames their own way: the latency of the frame lines before them.
        if m and len(lines) > 1 and lines[-1][4] == lines[-2][4]:
            gpus.append((lines[-1][4], float(m[2])))

# Frames (a, b] ran at latency L when the lines at a and at b both name L.
starts, ranges = [], []
for a, b in zip(lines, lines[1:]):
    if a[4] == b[4]:
        starts.append(a[0])
        ranges.append((a[0], b[0], b[4]))


def latency_of(frame):
    i = bisect.bisect_left(starts, frame) - 1
    if i >= 0 and ranges[i][0] < frame <= ranges[i][1]:
        return ranges[i][2]
    return None


frames, compiled = {}, {}
path = newest('*.csv', skip='-presents')
with open(path, newline='') as f:
    for row in csv.DictReader(f):
        lat = latency_of(int(row['frame']) + 1)
        if lat is None or float(row['draws']) < heavy:
            continue
        frames.setdefault(lat, []).append(float(row['frame_ms']))
        # Frames that waited for pipelines to compile (a first run, no pipeline cache yet).
        if float(row.get('pso_wait_us') or 0) > 0:
            compiled.setdefault(lat, []).append(float(row['frame_ms']))


def pct(values, p):
    s = sorted(values)
    return s[min(len(s) - 1, int(len(s) * p))]


print(f'LOG {os.path.basename(log)}  CSV {os.path.basename(path)}  heavy >= {heavy} draws')
for lat in sorted(frames, reverse=True):
    ms = frames[lat]
    mean = sum(ms) / len(ms)
    slow = sum(1 for v in ms if v >= 40.0) / len(ms) * 100
    sec = [l[2] for l in lines if l[4] == lat and l[1] >= heavy]
    shown = [l[3] for l in lines if l[4] == lat and l[1] >= heavy and l[3] > 0]
    g = [v for l, v in gpus if l == lat]
    c = compiled.get(lat, [])
    rest = sorted(ms)
    for v in c:
        rest.remove(v)
    p = [x for x in power if x[1] == lat]
    worst = f', worst second {1000 / max(sec):.1f} FPS' if sec else ''
    print(f'latency {lat}: {len(ms)} frames, {1000 / mean:.1f} FPS, frame avg {mean:.2f} ms, '
          f'99th pct {pct(ms, 0.99):.1f} ms, max {max(ms):.1f} ms, {slow:.1f} % of frames >= 40 ms'
          + worst)
    print(f'           begin to shown {sum(shown) / len(shown):.1f} ms' if shown else '           begin to shown -',
          f'| GPU {sum(g) / len(g):.2f} ms/frame' if g else '| GPU -',
          f'| SoC {sum(x[2] for x in p) / len(p) / 1000:.2f} W, GPU busy {sum(x[3] for x in p) / len(p):.0f} %, '
          f'{sum(x[4] for x in p) / len(p):.0f} MHz' if p else '')
    if rest:
        print(f'           without the {len(c)} frames that waited for pipelines: '
              f'{1000 * len(rest) / sum(rest):.1f} FPS, 99th pct {pct(rest, 0.99):.1f} ms, '
              f'max {max(rest):.1f} ms, '
              f'{sum(1 for v in rest if v >= 40.0) / len(rest) * 100:.1f} % >= 40 ms')
