"""Runs on the Deck while a game plays: python3 sample.py <exe name> <seconds>. Prints the game's
CPU use per thread (from /proc, % of one core), its total, the APU's power, the GPU's busy share
and clock, and the CPU cores' frequencies, averaged over the seconds."""
import glob
import os
import re
import sys
import time

name, secs = sys.argv[1], float(sys.argv[2])
hz = os.sysconf('SC_CLK_TCK')


def game_pid():
    for p in glob.glob('/proc/[0-9]*/cmdline'):
        try:
            c = open(p, 'rb').read().replace(b'\0', b' ').decode('utf-8', 'replace')
        except OSError:
            continue
        if re.match(r'^Z:.*%s\.exe --log' % re.escape(name), c):
            return int(p.split('/')[2])
    return None


def threads(pid):
    out = {}
    for t in glob.glob('/proc/%d/task/*' % pid):
        try:
            st = open(t + '/stat').read()
            comm = open(t + '/comm').read().strip()
        except OSError:
            continue
        f = st[st.rfind(')') + 2:].split()
        out[int(t.rsplit('/', 1)[1])] = (comm, int(f[11]) + int(f[12]))
    return out


def hwmon(want):
    for d in glob.glob('/sys/class/hwmon/hwmon*'):
        try:
            if open(d + '/name').read().strip() == 'amdgpu':
                for f in want:
                    if os.path.exists(d + '/' + f):
                        return d + '/' + f
        except OSError:
            pass
    return None


pid = game_pid()
if not pid:
    sys.exit('no %s running' % name)
pw = hwmon(['power1_average', 'power1_input'])
busy = (glob.glob('/sys/class/drm/card*/device/gpu_busy_percent') or [None])[0]
sclk = (glob.glob('/sys/class/drm/card*/device/pp_dpm_sclk') or [None])[0]
a = threads(pid)
t0 = time.time()
powers, busys, clocks, freqs = [], [], [], []
while time.time() - t0 < secs:
    if pw:
        powers.append(int(open(pw).read()) / 1e6)
    if busy:
        busys.append(int(open(busy).read()))
    if sclk:
        m = re.search(r'(\d+)Mhz \*', open(sclk).read())
        if m:
            clocks.append(int(m.group(1)))
    fs = []
    for c in glob.glob('/sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_cur_freq'):
        fs.append(int(open(c).read()) / 1000)
    freqs.append(sum(fs) / len(fs) if fs else 0)
    time.sleep(0.5)
el = time.time() - t0
b = threads(pid)
rows = []
for tid, (comm, ticks) in b.items():
    if tid in a:
        rows.append((100.0 * (ticks - a[tid][1]) / hz / el, comm, tid))
rows.sort(reverse=True)
print('game %s pid %d over %.1f s: CPU %.0f%% of one core in all (%d threads)' % (
    name, pid, el, sum(r[0] for r in rows), len(rows)))
for pct, comm, tid in rows[:12]:
    if pct >= 1:
        print('  %5.1f%%  %s (%d)' % (pct, comm, tid))
avg = lambda v: sum(v) / len(v) if v else float('nan')
print('APU %.2f W (min %.1f, max %.1f) | GPU busy %.0f%% at %.0f MHz | CPU cores at %.0f MHz' % (
    avg(powers), min(powers or [0]), max(powers or [0]), avg(busys), avg(clocks), avg(freqs)))
