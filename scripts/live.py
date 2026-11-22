#!/usr/bin/env python3
"""scripts/live.ps1 for Linux and Proton (the Steam Deck): a game kept running between experiments,
with the same commands, arguments and output, so one set of instructions fits both.

    scripts/live.sh start -Dir <game's build folder> -Exe game.exe -Extra "..." -Route "10:pad:A:200"
    scripts/live.sh frame before          # <live>/shots/before.png: the renderer's own capture
    scripts/live.sh control "renderer stock"
    scripts/live.sh keys "0:pad:START:150,3:pad:A:150"
    scripts/live.sh record 3              # <live>/trace/<exe>-f<frame>.rxt
    scripts/live.sh play <trace.rxt> -Extra "--rexnative_trace_play_loops=2 --rexnative_trace_play_dump=true"
    scripts/live.sh log "failed|removed" 20
    scripts/live.sh status | stop

How it differs from live.ps1 (see that file for what each command does):
- The game runs under $REXNATIVE_RUNNER (Proton, with a prefix of its own: on the Deck
  kit/bin/rexdeck-run) inside a headless gamescope (--backend headless): no window, never the
  screen, never Steam or a game someone plays. Its sound is muted (--apu.mute=true).
- Without a window there are no keyboard keys, clicks or window captures: routes and `keys` take
  `pad:` entries (the game's pad, through the control file), `shot` and `burst` are the renderer's
  own captures (as `frame`), `click` and `capture` (RenderDoc) are refused.
- The live game is one per state folder: $REXNATIVE_LIVE_STATE, else ~/.local/state/rexnative-live.
- compare and oracle run scripts/resolves.py with $REXNATIVE_PYTHON (needs numpy and Pillow).
"""
import datetime
import glob
import json
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
STRING = {'dir', 'exe', 'extra', 'title', 'route', 'renderdocdir'}
SWITCH = {'pulse', 'force', 'renderdoc', 'oracle', 'plain'}
STATE_DIR = os.environ.get('REXNATIVE_LIVE_STATE') or os.path.join(
    os.environ.get('XDG_STATE_HOME') or os.path.expanduser('~/.local/state'), 'rexnative-live')
STATE = os.path.join(STATE_DIR, 'state.json')
COMMANDS = ('start, play, compare, audit, idle, keys, click, shot, frame, burst, debug, snap, drawlog, '
            'control, trace, capture, record, oracle, log, status, stop')


class Fail(Exception):
    pass


def parse(argv):
    """live.ps1's arguments: the command and two positional values, -Name value, -Switch."""
    pos, opt = [], {k: '' for k in STRING}
    opt.update({k: False for k in SWITCH})
    i = 0
    while i < len(argv):
        a = argv[i]
        name = a.lstrip('-').lower() if a.startswith('-') and len(a) > 1 else None
        if name in STRING:
            if i + 1 >= len(argv):
                raise Fail('%s needs a value' % a)
            opt[name] = argv[i + 1]
            i += 2
            continue
        if name in SWITCH:
            opt[name] = True
        else:
            pos.append(a)
        i += 1
    pos += [''] * 3
    return pos[0] or 'status', pos[1], pos[2], opt


def wine(path):
    """A Linux path as the game under Wine sees it."""
    return 'Z:' + os.path.abspath(path).replace('/', '\\')


def read_state():
    try:
        with open(STATE, encoding='utf-8') as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def alive(s):
    if not s:
        return False
    try:
        os.kill(s['pid'], 0)
    except (OSError, KeyError):
        return False
    try:
        with open('/proc/%d/cmdline' % s['pid'], 'rb') as f:
            return s['exe'].encode() in f.read()
    except OSError:
        return False


def get_game(s):
    if not s:
        raise Fail('no live game: scripts/live.sh start -Dir <game dir> -Exe <exe>')
    if not alive(s):
        raise Fail('the live game (%s, pid %s) is not running' % (s['exe'], s['pid']))
    return s


def end(s, wait=20):
    """The live game's whole process group: the headless gamescope, the runner and Proton (the
    runner then ends what runs in its prefix)."""
    try:
        pgid = os.getpgid(s['pid'])
    except OSError:
        return
    for sig, secs in ((signal.SIGTERM, wait), (signal.SIGKILL, 5)):
        try:
            os.killpg(pgid, sig)
        except OSError:
            return
        t = time.time()
        while time.time() - t < secs:
            try:
                os.killpg(pgid, 0)
            except OSError:
                return
            time.sleep(0.3)


def append(path, text):
    with open(path, 'a', encoding='ascii', newline='\n') as f:
        f.write(text + '\n')


def write(path, text):
    with open(path, 'w', encoding='ascii', newline='\n') as f:
        f.write(text + '\n')


def newest(folder, pattern, since=None):
    files = [f for f in glob.glob(os.path.join(folder, pattern))
             if since is None or os.path.getmtime(f) >= since]
    return max(files, key=os.path.getmtime) if files else None


def wait_stable(path, tries=100, step=0.4):
    """Written once its size is above zero and stops changing."""
    size = -1
    for _ in range(tries):
        time.sleep(step)
        try:
            n = os.path.getsize(path)
        except OSError:
            continue
        if n > 0 and n == size:
            return
        size = n


def log_path(s):
    path = os.path.join(s['live'], 'live.log')
    # ReXGlue writes --log_file=live\live.log as logs/live/live.log_NNN.log.
    rotated = newest(os.path.join(s['dir'], 'logs', 'live'), 'live.log*')
    if rotated and (not os.path.exists(path) or os.path.getmtime(rotated) > os.path.getmtime(path)):
        path = rotated
    return path


def tail(path, n):
    try:
        with open(path, encoding='utf-8', errors='replace') as f:
            return f.read().splitlines()[-n:]
    except OSError:
        return []


def log_since(s, pattern, since, n, strip):
    """Lines of the log matching the pattern, written from one second before `since` (the line's
    first 23 characters are its time, as live.ps1 reads them)."""
    out = []
    for line in tail(log_path(s), n):
        if not re.search(pattern, line):
            continue
        try:
            t = datetime.datetime.fromisoformat(line[:23].replace('/', '-'))
        except ValueError:
            continue
        if t.timestamp() >= since - 1:
            out.append(re.sub(strip, '', line))
    return out


def send_schedule(s, schedule):
    """`<seconds from now>:<key>` items, in order. Only pad: entries reach the game here."""
    start = time.time()
    for item in [k for k in schedule.split(',') if k]:
        i = item.index(':')
        at, key = float(item[:i]), item[i + 1:]
        while time.time() - start < at:
            time.sleep(0.1)
            if not alive(s):
                raise Fail('the game exited')
        at_text = ('%g' % at)
        if key.startswith('pad:'):
            parts = key.split(':')
            ms = parts[2] if len(parts) > 2 else '150'
            axes = (' ' + ' '.join(parts[3:])) if len(parts) > 3 else ''
            append(os.path.join(s['live'], 'control.txt'), 'pad %s%s %s' % (parts[1], axes, ms))
            print('pad %s%s %s ms at %ss' % (parts[1], axes, ms, at_text))
        else:
            print("skipped '%s' at %ss: no window here (keys and clicks need one; use pad:)" % (key, at_text))


def set_debug(s, line, pulse):
    path = os.path.join(s['live'], 'cap', 'draw-debug.txt')
    write(path, line)
    time.sleep(1.2)  # read every 15 frames
    if pulse:
        write(path, '0')
        time.sleep(1.2)


def frame(s, name):
    """The renderer's own capture of the next frame (capture.request in <live>/cap): its
    f<frame>-native.png copied to <live>/shots/<name>.png."""
    cap = os.path.join(s['live'], 'cap')
    before = time.time()
    open(os.path.join(cap, 'capture.request'), 'w').close()
    f = None
    for _ in range(50):
        time.sleep(0.2)
        f = newest(cap, 'f*-native.png', before - 0.5)
        if f:
            break
    if not f:
        # With the Xenos plugin drawing (control "renderer stock") the renderer takes no capture:
        # the Xenos copy saved beside each native one is that renderer's image of the same frame.
        raise Fail('no capture within 10 s (with "renderer stock" there is none: compare with the '
                   '<name>-xenos.png that frame saves beside the native one)')
    wait_stable(f, 50, 0.3)
    dst = os.path.join(s['live'], 'shots', name + '.png')
    shutil.copyfile(f, dst)
    # The Xenos plugin's image of the same frame (--rexnative_capture_xenos, on by default),
    # written right after the native one: what "renderer stock" would show, without switching.
    xenos = f[:-len('-native.png')] + '-xenos.png'
    for _ in range(25):
        if os.path.exists(xenos):
            wait_stable(xenos, 25, 0.3)
            shutil.copyfile(xenos, os.path.join(s['live'], 'shots', name + '-xenos.png'))
            break
        time.sleep(0.2)
    return dst


def headless():
    """The headless gamescope to run the game in, or None (REXNATIVE_LIVE_HEADLESS=0, or none)."""
    if os.environ.get('REXNATIVE_LIVE_HEADLESS', '1') == '0':
        return None
    gs = shutil.which('gamescope')
    if not gs:
        return None
    try:
        h = subprocess.run([gs, '--help'], capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if 'headless' not in (h.stdout + h.stderr):
        return None
    return [gs, '--backend', 'headless', '-w', '1280', '-h', '720', '-W', '1280', '-H', '720', '--']


def python():
    return os.environ.get('REXNATIVE_PYTHON') or sys.executable


def main(argv):
    command, arg, arg2, o = parse(argv)
    s = read_state()
    more = []
    if command == 'play':
        # The game's own executable plays a frame trace instead of the game (live.ps1 play).
        if not arg:
            raise Fail('play needs a trace: scripts/live.sh play <trace.rxt>')
        trace = os.path.abspath(arg)
        if not os.path.exists(trace):
            raise Fail('no trace %s' % arg)
        if not o['dir']:
            o['dir'] = os.path.dirname(os.path.dirname(os.path.dirname(trace)))
        if not o['exe']:
            o['exe'] = re.sub(r'-f\d+$', '', os.path.splitext(os.path.basename(trace))[0]) + '.exe'
        if not os.path.exists(os.path.join(o['dir'], o['exe'])):
            raise Fail('play: no %s in %s (pass -Dir and -Exe: the game the trace was recorded from)'
                       % (o['exe'], o['dir']))
        more = ['--rexnative_trace_play=' + wine(trace)]
        same = s and os.path.abspath(s['dir']) == os.path.abspath(o['dir'])
        if same:
            o['force'] = True
        command = 'start'

    if command == 'start':
        if alive(s):
            if not o['force']:
                raise Fail('a live game is running (pid %s): stop it or pass -Force' % s['pid'])
            end(s, 10)
        if not o['dir'] or not o['exe']:
            raise Fail('start needs -Dir and -Exe')
        if o['renderdoc']:
            raise Fail('RenderDoc is not set up on this machine (live.ps1 -RenderDoc is Windows only)')
        runner = shlex.split(os.environ.get('REXNATIVE_RUNNER', ''))
        if not runner:
            raise Fail('set REXNATIVE_RUNNER to the command that runs Windows programs (on the Deck: '
                       'kit/bin/rexdeck-run)')
        game_dir = os.path.abspath(o['dir'])
        exe = os.path.join(game_dir, o['exe'])
        if not os.path.exists(exe):
            raise Fail('no %s' % exe)
        live = os.path.join(game_dir, 'live')
        for d in ('cap', 'trace', 'shots', 'rdoc', 'resolves', 'resolves-play'):
            os.makedirs(os.path.join(live, d), exist_ok=True)
        write(os.path.join(live, 'cap', 'draw-debug.txt'), '0')
        dumps = 'resolves-play' if more else 'resolves'
        for f in glob.glob(os.path.join(live, dumps, '*')):
            try:
                os.remove(f)
            except OSError:
                pass
        native = [] if o['plain'] else [
            '--rexnative_capture_dir=' + wine(os.path.join(live, 'cap')),
            '--rexnative_d3d_control=' + wine(os.path.join(live, 'control.txt')),
            '--rexnative_d3d_trace_dir=' + wine(os.path.join(live, 'trace')),
            '--rexnative_resolve_dump=' + wine(os.path.join(live, dumps)),
            '--rexnative_texture_audit=1']
        if o['oracle']:
            native += ['--rexnative=false', '--rexnative_beside_xenos=true',
                       '--rexnative_render_thread=false', '--rexnative_hdr=false']
        title = o['title'] or '%s - %s (scripts/live.sh): please do not play in this window' % (
            os.path.splitext(o['exe'])[0], 'plays a frame trace' if more else 'live test')
        args = ['--log_file=live\\live.log', '--mnk_mode=true', '--fullscreen=false',
                '--window_width=1280', '--window_height=720', '--ui.title=' + title.replace('"', "'"),
                '--apu.mute=true'] + native + more + [a for a in o['extra'].split(' ') if a]
        wrap = headless() or []
        env = dict(os.environ)
        # The headless gamescope opens its Wayland socket there (an SSH session has none set).
        run_dir = '/run/user/%d' % os.getuid()
        if not env.get('XDG_RUNTIME_DIR') and os.path.isdir(run_dir):
            env['XDG_RUNTIME_DIR'] = run_dir
        out = open(os.path.join(live, 'run.out'), 'ab')
        p = subprocess.Popen(wrap + runner + [exe] + args, cwd=game_dir, stdout=out, env=env,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
        os.makedirs(STATE_DIR, exist_ok=True)
        s = {'pid': p.pid, 'exe': exe, 'dir': game_dir, 'live': live,
             'started': datetime.datetime.now().isoformat(timespec='seconds'), 'extra': o['extra'],
             'renderdoc': False, 'oracle': o['oracle'], 'playing': bool(more), 'headless': bool(wrap)}
        with open(STATE, 'w', encoding='utf-8') as f:
            json.dump(s, f, indent=1)
        print('started pid %d: %s %s' % (p.pid, exe, ' '.join(args)))
        if o['route']:
            send_schedule(s, o['route'])
            print('route done')
        return

    if command == 'keys':
        send_schedule(get_game(s), arg)
    elif command == 'click':
        raise Fail('click needs a window: none here (the runtime dialogs take the pad too)')
    elif command in ('shot', 'frame'):
        get_game(s)
        print(frame(s, arg or time.strftime('%H%M%S')))
    elif command == 'burst':
        get_game(s)
        name = arg or time.strftime('%H%M%S')
        for i in range(int(arg2) if arg2 else 6):
            print(frame(s, '%s-%d' % (name, i)))
    elif command == 'debug':
        get_game(s)
        set_debug(s, arg, o['pulse'])
        print('draw-debug.txt: %s%s' % (arg, ' (then 0)' if o['pulse'] else ''))
    elif command == 'snap':
        get_game(s)
        set_debug(s, ('256 0 %s 0' % arg) if re.fullmatch(r'\d+', arg) else ('256 0 0 0 %s' % arg), True)
        snaps = sorted(glob.glob(os.path.join(s['live'], 'cap', 'snap-*.txt')), key=os.path.getmtime,
                       reverse=True)[:12]
        print('\n'.join(snaps))
    elif command == 'drawlog':
        get_game(s)
        set_debug(s, '16', True)
        f = newest(os.path.join(s['live'], 'cap'), 'draws-f*.txt')
        print(f or '')
    elif command == 'control':
        get_game(s)
        write(os.path.join(s['live'], 'control.txt'), '\n'.join(arg.split(';')))
        time.sleep(1.5)
        print('control: %s' % arg)
    elif command == 'trace':
        get_game(s)
        tag, frames = arg or 'frame', arg2 or '1'
        control = os.path.join(s['live'], 'control.txt')
        write(control, 'tap on\ntrace %s %s' % (tag, frames))
        time.sleep(3)
        write(control, 'tap off')
        time.sleep(1.5)
        files = sorted(glob.glob(os.path.join(s['live'], 'trace', 'rexnative-d3d-%s-f*.txt' % tag)),
                       key=os.path.getmtime, reverse=True)[:int(frames)]
        print('\n'.join(files))
    elif command == 'capture':
        raise Fail('RenderDoc is not set up on this machine (live.ps1 capture is Windows only)')
    elif command == 'record':
        # A frame trace of the next N frames (default 2), for `play`.
        get_game(s)
        n = int(arg) if re.fullmatch(r'\d+', arg) else 2
        start = time.time()
        append(os.path.join(s['live'], 'control.txt'), 'set rexnative_trace_record %d' % n)
        f = None
        for _ in range(100):
            time.sleep(0.3)
            f = newest(os.path.join(s['live'], 'trace'), '*.rxt', start - 0.5)
            if f:
                break
        if not f:
            raise Fail('no trace within 30 s (the render thread must be on; see `log trace`)')
        wait_stable(f)
        # The game also dumps the resolves of the trace's first frame, for `compare`; written a
        # few frames later and slower under Wine: waited for, or a `play` right after (which
        # replaces this game) would leave it empty.
        resolves = os.path.join(s['live'], 'resolves')
        for _ in range(100):
            meta = newest(resolves, 'f*-resolves.jsonl', start - 0.5)
            if meta and os.path.getsize(meta) > 0:
                wait_stable(meta, 25, 0.4)
                break
            time.sleep(0.3)
        print('%s (%.1f MB)' % (f, os.path.getsize(f) / 1048576))
    elif command == 'compare':
        get_game(s)
        if not s.get('playing'):
            raise Fail('compare works on a played trace: live.sh record 1, then live.sh play <trace>')
        mine, play = os.path.join(s['live'], 'resolves'), os.path.join(s['live'], 'resolves-play')
        if not glob.glob(os.path.join(mine, 'f*-resolves.jsonl')):
            raise Fail('no resolve dump of the game in %s (record the trace with this live.sh)' % mine)
        meta = None
        for _ in range(200):
            time.sleep(0.3)
            found = glob.glob(os.path.join(play, 'f*-resolves.jsonl'))
            if found:
                meta = found[0]
                break
        if not meta:
            raise Fail('the player wrote no resolve dump within 60 s')
        wait_stable(meta)
        time.sleep(1.5)
        subprocess.run([python(), os.path.join(HERE, 'resolves.py'), mine, '--against', play,
                        '--over', arg or '0.004'])
    elif command == 'audit':
        get_game(s)
        control = os.path.join(s['live'], 'control.txt')
        start = time.time()
        append(control, 'set rexnative_texture_audit 2')
        time.sleep(1.5)
        append(control, 'set rexnative_texture_audit 3')
        time.sleep(2.5)
        print('\n'.join(log_since(s, 'ReXNative texture audit', start, 2000, r'^.*ReXNative texture audit: ')))
    elif command == 'idle':
        get_game(s)
        n = arg if re.fullmatch(r'\d+', arg) else '60'
        start = time.time()
        append(os.path.join(s['live'], 'control.txt'), 'set rexnative_resolve_drop_idle %s' % n)
        time.sleep(2.5)
        print('\n'.join(log_since(s, r'ReXNative resolve: (idle image|[0-9]+ images idle)', start, 400,
                                  r'^.*ReXNative resolve: ')))
    elif command == 'oracle':
        get_game(s)
        if not s.get('oracle'):
            raise Fail('the live game was not started with -Oracle')
        folder = os.path.join(s['live'], 'resolves')
        start = time.time()
        open(os.path.join(folder, 'dump.request'), 'w').close()
        meta = None
        for _ in range(100):
            time.sleep(0.3)
            meta = newest(folder, 'f*-resolves.jsonl', start - 0.5)
            if meta:
                break
        if not meta:
            raise Fail('no resolve dump within 30 s')
        wait_stable(meta)
        time.sleep(1.5)
        fr = re.match(r'^f(\d+)-', os.path.basename(meta)).group(1)
        subprocess.run([python(), os.path.join(HERE, 'resolves.py'), folder, '--frame', fr,
                        '--over', arg or '0.02'])
    elif command == 'log':
        if not s:
            raise Fail('no live game')
        lines = tail(log_path(s), 4000)
        if arg:
            lines = [l for l in lines if re.search(arg, l)]
        print('\n'.join(lines[-(int(arg2) if arg2 else 30):]))
    elif command == 'status':
        if not s:
            print('no live game')
        elif alive(s):
            print("running: pid %s, %s, since %s, extra '%s'" % (s['pid'], s['exe'], s['started'], s['extra']))
        else:
            print('not running (last: %s)' % s['exe'])
    elif command == 'stop':
        # stop -Dir <folder> stops only the game started from that folder.
        if s and o['dir'] and os.path.abspath(o['dir']) != os.path.abspath(s['dir']):
            print('not stopped: the live game is %s (pid %s), not one in %s' % (s['exe'], s['pid'], o['dir']))
            return
        if s:
            if alive(s):
                end(s)
            try:
                os.remove(STATE)
            except OSError:
                pass
        print('stopped')
    else:
        raise Fail("unknown command '%s' (%s)" % (command, COMMANDS))


if __name__ == '__main__':
    try:
        main(sys.argv[1:])
    except Fail as e:
        print(e, file=sys.stderr)
        sys.exit(1)
