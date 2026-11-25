"""Runs on the Deck while a game plays: python3 gpuclock.py <seconds> <out.csv> [interval ms].
Writes the GPU's clock, busy share and the APU's power every few milliseconds, each with
CLOCK_MONOTONIC_RAW in ms (Wine's QueryPerformanceCounter, so ReXNative's CSV t_ms column), to
set beside a per-frame CSV (--rexnative.csv): a frame whose GPU time grows with the same work was
the GPU's clock going down."""
import glob
import sys
import time

secs, out = float(sys.argv[1]), sys.argv[2]
step = (float(sys.argv[3]) if len(sys.argv) > 3 else 5.0) / 1000.0
hw = next(d for d in glob.glob('/sys/class/hwmon/hwmon*')
          if open(d + '/name').read().strip() == 'amdgpu')
dev = '/sys/class/drm/card0/device/'


def opener(path):
    try:
        return open(path)
    except OSError:
        return None


files = {'sclk_mhz': (opener(hw + '/freq1_input'), 1e-6),
         'busy_pct': (opener(dev + 'gpu_busy_percent'), 1.0),
         'apu_w': (opener(hw + '/power1_average') or opener(hw + '/power1_input'), 1e-6)}
files = {k: v for k, v in files.items() if v[0]}
with open(out, 'w') as o:
    o.write('t_ms,' + ','.join(files) + '\n')
    end = time.monotonic() + secs
    while time.monotonic() < end:
        vals = []
        for f, scale in files.values():
            f.seek(0)
            try:
                vals.append('%.1f' % (float(f.read().strip()) * scale))
            except ValueError:
                vals.append('')
        o.write('%.1f,%s\n' % (time.clock_gettime(time.CLOCK_MONOTONIC_RAW) * 1000, ','.join(vals)))
        time.sleep(step)
