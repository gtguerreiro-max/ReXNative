"""Runs on the Deck while a game plays: python3 states.py <exe name> <seconds>. For the game's
busiest threads: CPU (% of a core), how often it was found running, where it waits when not (wchan),
and its context switches a second (voluntary: it blocked or yielded; involuntary: preempted). A
thread at moderate CPU with thousands of voluntary switches a second is a polling loop."""
import collections
import glob
import os
import re
import sys
import time

name, secs = sys.argv[1], float(sys.argv[2])
hz = os.sysconf('SC_CLK_TCK')
pid = None
for p in glob.glob('/proc/[0-9]*/cmdline'):
    try:
        c = open(p, 'rb').read().replace(b'\0', b' ').decode('utf-8', 'replace')
    except OSError:
        continue
    if re.match(r'^Z:.*%s\.exe --log' % re.escape(name), c):
        pid = int(p.split('/')[2])
if not pid:
    sys.exit('no %s' % name)


def snap():
    out = {}
    for t in glob.glob('/proc/%d/task/*' % pid):
        try:
            st = open(t + '/stat').read()
            f = st[st.rfind(')') + 2:].split()
            s = open(t + '/status').read()
            v = int(re.search(r'voluntary_ctxt_switches:\s+(\d+)', s).group(1))
            nv = int(re.search(r'nonvoluntary_ctxt_switches:\s+(\d+)', s).group(1))
            out[int(t.rsplit('/', 1)[1])] = (open(t + '/comm').read().strip(), int(f[11]) + int(f[12]), v, nv)
        except (OSError, AttributeError):
            pass
    return out


a = snap()
t0 = time.time()
time.sleep(2)
b = snap()
top = sorted(((b[t][1] - a[t][1], t) for t in b if t in a), reverse=True)[:10]
seen = {t: collections.Counter() for _, t in top}
while time.time() - t0 < secs:
    for _, t in top:
        try:
            st = open('/proc/%d/task/%d/stat' % (pid, t)).read()
            state = st[st.rfind(')') + 2]
            w = open('/proc/%d/task/%d/wchan' % (pid, t)).read().strip() if state != 'R' else 'running'
        except OSError:
            continue
        seen[t][w or state] += 1
    time.sleep(0.005)
c = snap()
el = time.time() - t0
for _, t in top:
    if t not in c:
        continue
    comm, tk, v, nv = c[t]
    n = sum(seen[t].values()) or 1
    print('%5.1f%% %-16s vol %6.0f/s invol %5.0f/s | %s' % (
        100.0 * (tk - a[t][1]) / hz / el, comm, (v - a[t][2]) / el, (nv - a[t][3]) / el,
        ', '.join('%s %d%%' % (k, 100 * x // n) for k, x in seen[t].most_common(3))))
