"""Runs on the Deck while a game plays: python3 threadtrace.py <exe name> <seconds> <out.csv> [ms].
Every few milliseconds (default 20): each of the game's threads' run time in that step (from
/proc/<pid>/task/<tid>/schedstat, in ns), the CPU cores' clocks, the GPU's clock and busy share and
the APU's power, each with CLOCK_MONOTONIC_RAW in ms (ReXNative's CSV t_ms). Rows: t_ms,kind,
name,value: kind 'thr' (name "tid comm", value: % of one core over the step), 'cpu' (name the core,
MHz), 'gpu' (sclk MHz), 'busy' (%), 'apu' (W). For a frame that was slow because the APU hit its
power limit: which threads ran just before it."""
import glob
import os
import re
import sys
import time

name, secs, out = sys.argv[1], float(sys.argv[2]), sys.argv[3]
step = (float(sys.argv[4]) if len(sys.argv) > 4 else 20.0) / 1000.0


def game_pid():
    for p in glob.glob('/proc/[0-9]*/cmdline'):
        try:
            c = open(p, 'rb').read().replace(b'\0', b' ').decode('utf-8', 'replace')
        except OSError:
            continue
        if re.match(r'^Z:.*%s\.exe --log' % re.escape(name), c):
            return int(p.split('/')[2])
    return None


pid = game_pid()
if pid is None:
    sys.exit('no %s running' % name)
hw = next(d for d in glob.glob('/sys/class/hwmon/hwmon*')
          if open(d + '/name').read().strip() == 'amdgpu')


def read(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return ''


cpus = sorted(glob.glob('/sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_cur_freq'))
names = {}
last = {}
with open(out, 'w') as o:
    o.write('t_ms,kind,name,value\n')
    end = time.monotonic() + secs
    t_prev = None
    while time.monotonic() < end:
        t = time.clock_gettime(time.CLOCK_MONOTONIC_RAW) * 1000
        rows = []
        for task in os.listdir('/proc/%d/task' % pid):
            s = read('/proc/%d/task/%s/schedstat' % (pid, task))
            if not s:
                continue
            ns = int(s.split()[0])
            if task not in names:
                names[task] = read('/proc/%d/task/%s/comm' % (pid, task)).replace(',', ' ')
            if task in last and t_prev is not None and ns > last[task]:
                rows.append('%.1f,thr,%s %s,%.1f' % (t, task, names[task],
                                                     (ns - last[task]) / 1e4 / (t - t_prev)))
            last[task] = ns
        for c in cpus:
            rows.append('%.1f,cpu,%s,%d' % (t, c.split('/')[5], int(read(c) or 0) // 1000))
        rows.append('%.1f,gpu,sclk,%d' % (t, int(read(hw + '/freq1_input') or 0) // 1000000))
        rows.append('%.1f,busy,gpu,%s' % (t, read('/sys/class/drm/card0/device/gpu_busy_percent')))
        rows.append('%.1f,apu,power,%.2f' % (t, int(read(hw + '/power1_average') or 0) / 1e6))
        o.write('\n'.join(rows) + '\n')
        t_prev = t
        time.sleep(max(0.0, step - (time.clock_gettime(time.CLOCK_MONOTONIC_RAW) * 1000 - t) / 1000))
