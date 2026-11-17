"""Drives a built game on the Steam Deck on a schedule, for tests that need the game in a given
place: started from Steam with extra flags, keys sent to its window (xdotool; the game runs with
--hid.mnk_mode), and at given times the renderer's own captures, draw-debug switches and the APU's
power. At the end: its frame and upscaler lines, and the captures copied to <work>/deck/.

  python scripts/deck_drive.py <work>/<game> <label> "<at>:<item>,..." <end seconds> [flags...]

Items: a key for xdotool (Return, space, Escape, a, ...); cap (a capture, f<frame>-native.png);
dbg<mask> (draw-debug.txt, see scripts/live.ps1); pwr (the APU's average power, amdgpu hwmon, 10
readings a second apart). Example, two captures and the power in a level:
  python scripts/deck_drive.py D:/work/my-game try1 "20:Return,40:Return,90:cap,95:pwr,120:cap" 130 \
      --rexnative_upscaler=true --rexnative_upscaler_mode=native
The game must be installed and in Steam (python -m rexnative deck <work>/<game> --steam). The Deck
is left alone while someone plays on it. Its address and key: REXNATIVE_DECK, REXNATIVE_DECK_KEY.
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from rexnative import deck as dk  # noqa: E402

q = dk.q


def main():
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    work, label, sched, end = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
    extra = ' '.join(sys.argv[5:])
    d = dk.Deck()
    if dk.playing(d):
        sys.exit('the Deck is in use')
    g = dk.install(d, work, log=lambda s: None)
    appid = dk.app_id(d, g)
    if appid is None:
        sys.exit('not in Steam: python -m rexnative deck %s --steam' % work)
    home = d.run('echo $HOME').strip()
    cap = '%s/%s/capture-%s' % (home, g['dest'], label)
    d.run('rm -rf %s && mkdir -p %s' % (q(cap), q(cap)))
    flags = ('--hid.mnk_mode=true --rexnative_capture_dir=%s --rexnative_capture_xenos=false '
             '--rexnative_d3d_stats_frames=150 %s' % ('Z:' + cap.replace('/', '\\'), extra))
    dest = q(g['dest'])
    exe_re = re.escape(g['name']) + r'\.exe --log'
    d.run('cd %s && printf "%%s\\n" %s > try.flags' % (dest, q(flags)))
    env = ('export XDG_RUNTIME_DIR=/run/user/$(id -u) DISPLAY=:0 '
           'XAUTHORITY=$(ls /run/user/$(id -u)/xauth_* | head -1); ')
    # Keeps the screen awake (a sleeping Deck pauses the game).
    d.run(env + 'xdotool key shift; (for i in $(seq 1 %d); do sleep 30; xdotool key shift; done) '
          '>/dev/null 2>&1 < /dev/null & disown' % (end // 30 + 2), check=False)
    time.sleep(2)
    logtext, running = '', 'no'
    try:
        d.run('DISPLAY=:0 nohup steam steam://rungameid/%d >/dev/null 2>&1 &' % ((appid << 32) | 0x02000000),
              check=False)
        t0 = time.time()
        for item in [s for s in sched.split(',') if s]:
            at, key = item.split(':', 1)
            while time.time() - t0 < float(at):
                time.sleep(0.2)
            if key == 'cap':
                d.run('touch %s/capture.request' % q(cap), check=False)
                print('%5.1f s capture' % (time.time() - t0), flush=True)
            elif key == 'pwr':
                out = d.run('for i in $(seq 1 10); do for f in /sys/class/hwmon/hwmon*/power1_average '
                            '/sys/class/hwmon/hwmon*/power1_input; do [ -r "$f" ] && grep -q amdgpu '
                            '"$(dirname $f)/name" && cat "$f" && break; done; sleep 1; done', check=False)
                vals = [int(v) for v in out.split() if v.strip().isdigit()]
                if vals:
                    print('%5.1f s power %.2f W (min %.2f, max %.2f, %d readings)' % (
                        time.time() - t0, sum(vals) / len(vals) / 1e6, min(vals) / 1e6,
                        max(vals) / 1e6, len(vals)), flush=True)
                else:
                    print('%5.1f s power: no reading' % (time.time() - t0), flush=True)
            elif key.startswith('dbg'):
                d.run('printf "%%s\\n" %s > %s/draw-debug.txt' % (q(key[3:]), q(cap)), check=False)
                print('%5.1f s draw-debug %s' % (time.time() - t0, key[3:]), flush=True)
            else:
                out = d.run(env + 'w=$(xdotool search --name %s | head -1); [ -n "$w" ] && '
                            'xdotool windowactivate --sync $w 2>/dev/null; xdotool keydown %s; sleep 0.15; '
                            'xdotool keyup %s; echo "$w"' % (q(g['name']), q(key), q(key)), check=False)
                print('%5.1f s %s -> window %s' % (time.time() - t0, key, out.strip() or '(none)'), flush=True)
        while time.time() - t0 < end:
            time.sleep(0.5)
        time.sleep(3)
        running = d.run('pgrep -f %s >/dev/null && echo yes || echo no' % q(exe_re), check=False).strip()
        logtext = d.run('cd %s && l=$(find logs -maxdepth 1 -name "*.log" -newer try.flags -printf "%%T@ %%p\\n" '
                        '| sort -n | tail -1 | cut -d" " -f2-) && [ -n "$l" ] && cat "$l"' % dest, check=False)
    finally:
        d.run('p=$(pgrep -f %s); [ -n "$p" ] && kill $p; rm -f %s/try.flags; true' % (q(exe_re), dest),
              check=False)
    time.sleep(5)
    out = os.path.join(work, 'deck')
    os.makedirs(out, exist_ok=True)
    for s in d.run('cd %s && ls *.png draws-f*.txt 2>/dev/null' % q(cap), check=False).split():
        data = d.read(cap + '/' + s)
        if data:
            open(os.path.join(out, '%s-%s' % (label, s)), 'wb').write(data)
            print('capture', os.path.join(out, '%s-%s' % (label, s)))
    # The captures are copied: the Deck keeps none.
    d.run('rm -rf %s' % q(cap), check=False)
    open(os.path.join(out, label + '.log'), 'w', encoding='utf-8', newline='\n').write(logtext)
    print(label, 'running at the end' if running == 'yes' else 'NOT running at the end')
    for line in logtext.splitlines():
        if re.search(r'upscaler|scene: learned|camera cut|crash report|exception', line) and 'is now' not in line:
            print(' ', line[24:220])
    for line in [l for l in logtext.splitlines() if 'ReXNative: frames' in l][-4:]:
        print(' ', line[line.find('frames'):][:150])
    errors = [l for l in logtext.splitlines() if ' E ' in l[20:30] or ' C ' in l[20:30]]
    print('  errors:', len(errors), (errors[0][-160:] if errors else ''))


if __name__ == '__main__':
    main()
