"""A game `prepare --project` built, on the Steam Deck: copied over SSH, added to Steam, tried.

  python -m rexnative deck <work>/<game>... [--host deck@ADDRESS[:PORT]] [--key FILE]
         [--steam] [--try SECONDS]

Each game goes to ~/Games/ReXNative/<game>/ on the Deck: its Windows build from run/ (the
executable and DLLs, without NVIDIA's, which the Deck cannot use), its files in game/, its
title update's in title-update/ (mounted as update:), deck/play.sh (Proton with a prefix of the
game's own, FSR 4 through GE-Proton), deck/install.sh and deck/steam_shortcuts.py, and
<name>-deck.toml, the game's settings for the Deck's screen (full screen at 1280x800), which
install.sh makes its settings unless the Deck has some already. Only files whose size or time
differ are sent again, so a rebuilt game sends its executable alone.

--steam adds every game given to Steam's library (Steam is closed for it, `steam -shutdown`:
it writes its shortcuts file back when it exits; Game Mode starts it again). --try SECONDS
starts each game from Steam in turn, as a player would (the game then has Game Mode's focus),
and after SECONDS reads its log (the frame times, errors, a crash) and takes a capture of the
screen through the renderer (--rexnative_capture_dir), copied back beside the game's work folder
in deck/, then closes it.

The Deck's address and SSH key: --host / --key, or REXNATIVE_DECK ("deck@192.168.1.10:22")
and REXNATIVE_DECK_KEY. Nothing of the game leaves the computer but for the Deck.
"""
import io
import json
import os
import re
import shlex
import subprocess
import sys
import tarfile
import time

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KIT = os.path.join(HERE, 'deck')
REMOTE_ROOT = 'Games/ReXNative'  # under the deck user's home
SKIP_RUN = {'crash.txt', 'image.bin', 'discover.log'}
SKIP_DIRS = {'logs', 'live', 'userdata', 'game', 'title-update'}

# The Deck's settings, as Iron Brigade's deck/ironbrigade-deck.toml has them (ReXGlue v0.11
# sections): its 1280x800 panel, full screen, and the native renderer's anisotropic filtering.
# The video mode the game is told about stays the console's 720p: told 1280x800, a mode no Xbox
# 360 has, Castlevania: Symphony of the Night and 3D Ultra Minigolf stopped at their first frames.
DECK_SETTINGS = [
    ('ui', 'window_width', '1280'),
    ('ui', 'window_height', '800'),
    ('ui', 'fullscreen', 'true'),
    ('ui', 'resolution', '"720p"'),
    ('rexnative', 'anisotropy', '8'),
]


def toml_set(text, section, key, value):
    """`key = value` in [section] of a ReXGlue v0.11 settings file (in place of the key's line
    there, or at the section's end; the section is added when missing)."""
    lines = text.splitlines()
    head = next((i for i, l in enumerate(lines) if l.strip() == '[%s]' % section), None)
    if head is None:
        return text.rstrip() + '\n\n[%s]\n%s = %s\n' % (section, key, value)
    end = next((i for i in range(head + 1, len(lines)) if lines[i].lstrip().startswith('[')),
               len(lines))
    for i in range(head + 1, end):
        if re.match(r'\s*%s\s*=' % re.escape(key), lines[i]):
            lines[i] = '%s = %s' % (key, value)
            return '\n'.join(lines) + '\n'
    # Before the blank lines and the comment that open the next section (the comment is the next
    # section's: Orcs Must Die!'s ui.resolution went under "# The full game's license").
    while end > head + 1 and (not lines[end - 1].strip() or lines[end - 1].lstrip().startswith('#')):
        end -= 1
    lines.insert(end, '%s = %s' % (key, value))
    return '\n'.join(lines) + '\n'


def toml_merge(text, extra):
    """The settings of `extra` (a deck.toml: [section] keys, or flat old names, which ReXGlue
    resolves through its aliases) set in `text`. Flat keys go before the first section."""
    section = None
    flat = []
    for line in extra.splitlines():
        s = line.split('#')[0].strip()
        m = re.match(r'\[([A-Za-z0-9_.]+)\]$', s)
        if m:
            section = m.group(1)
            continue
        m = re.match(r'([A-Za-z0-9_]+)\s*=\s*(.+)$', s)
        if not m:
            continue
        if section:
            text = toml_set(text, section, m.group(1), m.group(2))
        else:
            text = re.sub(r'(?m)^%s\s*=.*\n' % re.escape(m.group(1)), '', text)
            flat.append('%s = %s' % (m.group(1), m.group(2)))
    if flat:
        lines = text.splitlines()
        first = next((i for i, l in enumerate(lines) if l.lstrip().startswith('[')), len(lines))
        lines[first:first] = ['# This game on the Deck (deck.toml in its work folder).'] + flat + ['']
        text = '\n'.join(lines) + '\n'
    return text


class Deck:
    def __init__(self, host=None, key=None):
        spec = host or os.environ.get('REXNATIVE_DECK')
        if not spec:
            raise SystemExit('the Deck: pass --host deck@ADDRESS[:PORT] or set REXNATIVE_DECK')
        self.port = None
        if re.search(r':\d+$', spec):
            spec, port = spec.rsplit(':', 1)
            self.port = port
        self.host = spec if '@' in spec else 'deck@' + spec
        self.key = key or os.environ.get('REXNATIVE_DECK_KEY')

    def ssh_cmd(self, command):
        cmd = ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10']
        if self.port:
            cmd += ['-p', self.port]
        if self.key:
            cmd += ['-i', os.path.expanduser(self.key)]
        return cmd + [self.host, command]

    def run(self, command, check=True, timeout=600):
        r = subprocess.run(self.ssh_cmd(command), capture_output=True, text=True,
                           errors='replace', timeout=timeout)
        if check and r.returncode != 0:
            raise SystemExit('the Deck: `%s` failed (%d): %s' % (command, r.returncode,
                                                                   (r.stderr or r.stdout)[-600:]))
        return r.stdout

    def read(self, path):
        r = subprocess.run(self.ssh_cmd('cat %s' % shlex.quote(path)), capture_output=True)
        return r.stdout if r.returncode == 0 else None

    def files(self, dest):
        """{relative path: (size, mtime)} under dest on the Deck."""
        out = self.run('mkdir -p %s && cd %s && find . -type f -printf "%%P\\t%%s\\t%%T@\\n"' % (
            dest, dest))
        have = {}
        for line in out.splitlines():
            parts = line.split('\t')
            if len(parts) == 3:
                have[parts[0]] = (int(parts[1]), int(float(parts[2])))
        return have

    def send(self, dest, items, log=print):
        """items: [(local path, relative path on the Deck)]; those the Deck has (same size and
        time) are left out. Sent as one tar stream; returns the bytes sent."""
        have = self.files(dest)
        todo = []
        for src, rel in items:
            st = os.stat(src)
            if have.get(rel) != (st.st_size, int(st.st_mtime)):
                todo.append((src, rel, st.st_size))
        if not todo:
            return 0
        total = sum(t[2] for t in todo)
        log('  %d files, %.0f MB' % (len(todo), total / 1e6))
        p = subprocess.Popen(self.ssh_cmd('tar -xf - -C %s' % dest), stdin=subprocess.PIPE,
                             stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        with tarfile.open(fileobj=p.stdin, mode='w|', format=tarfile.GNU_FORMAT) as tar:
            for src, rel, _ in todo:
                info = tar.gettarinfo(src, arcname=rel)
                info.uid = info.gid = 1000
                info.uname = info.gname = 'deck'
                if rel.endswith('.sh'):
                    info.mode = 0o755
                with open(src, 'rb') as f:
                    tar.addfile(info, f)
        p.stdin.close()
        err = p.stderr.read().decode(errors='replace')
        if p.wait() != 0:
            raise SystemExit('the Deck: copying into %s failed: %s' % (dest, err[-600:]))
        return total


def walk(root, prefix):
    out = []
    for d, dirs, fs in os.walk(root):
        dirs.sort()
        for f in sorted(fs):
            p = os.path.join(d, f)
            out.append((p, prefix + os.path.relpath(p, root).replace(os.sep, '/')))
    return out


def title_of(work):
    """The game's name from its archive's: 'Jet Set Radio (World) (XBLA).7z' -> 'Jet Set Radio'."""
    try:
        src = json.load(open(os.path.join(work, 'prepare.json'), encoding='utf-8'))['source']
    except (OSError, KeyError, ValueError):
        src = os.path.basename(os.path.abspath(work))
    base = re.sub(r'\.(7z|zip|rar|iso)$', '', os.path.basename(src), flags=re.I)
    base = re.sub(r'\s*\([^)]*\)', '', base).strip()
    return base.replace(' - ', ': ', 1) if ' - ' in base else base


def package(work):
    """(executable name, [(local, relative)]) of a built game's Deck folder."""
    run = os.path.join(work, 'run')
    exes = [f for f in os.listdir(run) if f.lower().endswith('.exe')] if os.path.isdir(run) else []
    if len(exes) > 1:
        # The project's own executable (another one in run/ is a leftover of a test).
        from . import project
        own = project.manifest_name(work) + '.exe'
        exes = [f for f in exes if f.lower() == own.lower()]
    if len(exes) != 1:
        raise SystemExit('%s: no built game in run/ (rexnative prepare --project)' % work)
    name = exes[0][:-4]
    items = []
    for f in sorted(os.listdir(run)):
        p = os.path.join(run, f)
        if (os.path.isfile(p) and f not in SKIP_RUN and not f.lower().startswith('nvngx_') and
                (f.lower().endswith('.dll') or f == name + '.exe')):
            items.append((p, f))
    data = os.path.join(work, 'game-data')
    items += walk(data, 'game/')
    tu = os.path.join(work, 'title-update', 'content')
    if os.path.isdir(tu):
        items += walk(tu, 'title-update/')
    for f in ('play.sh', 'install.sh', 'steam_shortcuts.py'):
        items.append((os.path.join(KIT, f), f))
    # The game's settings for the Deck: the PC's, with the Deck's screen in place of a window.
    toml = os.path.join(run, name + '.toml')
    text = open(toml, encoding='utf-8').read() if os.path.exists(toml) else ''
    text = re.sub(r'(?m)^# A window first: a new port is tested windowed\.\n',
                  '# The Steam Deck (python -m rexnative deck): its 1280x800 panel, full screen.\n',
                  text)
    new = text
    for section, key, value in DECK_SETTINGS:
        new = toml_set(new, section, key, value)
    # <work>/deck.toml: this game's own settings on the Deck, in place of the same keys above
    # (Brothers - A Tale of Two Sons: [rexnative] enabled = false, the stock renderer, until
    # ReXNative draws its scene).
    own = os.path.join(work, 'deck.toml')
    if os.path.exists(own):
        new = toml_merge(new, open(own, encoding='utf-8').read())
    deck_toml = os.path.join(work, 'deck', name + '-deck.toml')
    os.makedirs(os.path.dirname(deck_toml), exist_ok=True)
    if not os.path.exists(deck_toml) or open(deck_toml, encoding='utf-8').read() != new:
        with open(deck_toml, 'w', encoding='utf-8', newline='\n') as f:
            f.write(new)
    items.append((deck_toml, name + '-deck.toml'))
    return name, items


def install(deck, work, log=print):
    work = os.path.abspath(work)
    slug = os.path.basename(work)
    title = title_of(work)
    name, items = package(work)
    dest = '%s/%s' % (REMOTE_ROOT, slug)
    log('%s -> ~/%s' % (title, dest))
    deck.send(dest, items, log=log)
    out = deck.run('cd %s && bash install.sh %s' % (dest, shlex.quote(title)))
    for line in out.strip().splitlines()[-2:]:
        log('  ' + line)
    return {'work': work, 'slug': slug, 'title': title, 'name': name, 'dest': dest}


def playing(deck):
    """Whether a game runs on the Deck (Steam's reaper for it): the Deck is in use."""
    # [S]: the pattern must not match the remote shell that runs pgrep, whose command line has it.
    return deck.run('pgrep -f "[S]teamLaunch AppId=" >/dev/null && echo yes || echo no',
                    check=False).strip() == 'yes'


def wait_idle(deck, log=print, minutes=120):
    """Waits while someone plays on the Deck (closing Steam or starting a game would end it);
    False when it stayed in use."""
    for i in range(minutes * 2):
        if not playing(deck):
            return True
        if i == 0:
            log('a game is running on the Deck: waiting for it to end')
        time.sleep(30)
    return False


def add_to_steam(deck, games, log=print):
    """Every game's shortcut, with Steam closed for it (Game Mode starts it again)."""
    if not wait_idle(deck, log):
        log('the Deck stayed in use: the shortcuts are not added')
        return False
    # In Game Mode gamescope-session.target upholds steam-launcher.service: Steam closed with
    # `steam -shutdown` is started again at once, before the shortcuts could be written (and
    # steam_shortcuts.py refuses while it runs). The unit is masked for this script alone
    # (--runtime: gone at a reboot too), and the trap unmasks it and starts Steam again whatever
    # happens. Steam is left 60 s after it started first: SteamOS's short session tracker
    # re-bootstraps a Steam that ends three times within a minute of its start.
    adds = '\n'.join('cd ~/%s && bash install.sh %s' % (g['dest'], shlex.quote(g['title']))
                     for g in games)
    script = '''
export XDG_RUNTIME_DIR=/run/user/$(id -u) DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$(id -u)/bus
masked=
desktop=
restore() {
  if [ -n "$masked" ]; then
    systemctl --user unmask --runtime steam-launcher.service >/dev/null 2>&1
    systemctl --user start steam-launcher.service >/dev/null 2>&1
  elif [ -n "$desktop" ]; then
    # The desktop session starts Steam only at login: started again here, in the session's
    # environment (the user manager has its display).
    systemd-run --user --quiet --collect steam -silent >/dev/null 2>&1
  fi
}
trap restore EXIT
if pgrep -x steam >/dev/null; then
  systemctl --user is-active --quiet gamescope-session.target 2>/dev/null || desktop=1
  pid=$(pgrep -o -x steam)
  age=$(( $(date +%%s) - $(stat -c %%Y /proc/$pid) ))
  [ "$age" -lt 70 ] && sleep $(( 70 - age ))
  if systemctl --user is-active --quiet gamescope-session.target 2>/dev/null; then
    systemctl --user mask --runtime steam-launcher.service >/dev/null && masked=1
  fi
  echo "closing Steam to add the shortcuts"
  steam -shutdown >/dev/null 2>&1
  for i in $(seq 1 120); do pgrep -x steam >/dev/null || break; sleep 0.5; done
  if pgrep -x steam >/dev/null; then echo "Steam did not close"; exit 1; fi
fi
%s
''' % adds
    out = deck.run('bash -s <<"REXNATIVE_EOF"\n%s\nREXNATIVE_EOF' % script, check=False,
                   timeout=300)
    for line in out.strip().splitlines():
        log('  ' + line)
    # Game Mode starts Steam again (the trap above, or by itself); a desktop session does not.
    for _ in range(90):
        if deck.run('pgrep -x steam >/dev/null && echo yes || echo no').strip() == 'yes':
            time.sleep(45)  # its library up before a game is started from it
            return True
        time.sleep(2)
    log('Steam is not running again (a desktop session?): start it to see the games')
    return True


def app_id(deck, g):
    out = deck.run('python3 %s/steam_shortcuts.py list' % g['dest'], check=False)
    for line in out.splitlines():
        parts = line.split(None, 1)
        if len(parts) == 2 and parts[1].startswith(g['title'] + '  '):
            return int(parts[0])
    return None


FRAME_LINE = re.compile(r'frames (\d+) \| per frame: draws ([\d.]+).*?frame ([\d.]+) ms')


def try_game(deck, g, seconds, log=print):
    """Starts the game from Steam, reads its log and takes a capture after `seconds`."""
    if not wait_idle(deck, log):
        log('%s: not tried, the Deck stayed in use' % g['title'])
        return None
    appid = app_id(deck, g)
    if appid is None:
        log('%s: not in Steam (--steam first)' % g['title'])
        return None
    home = deck.run('echo $HOME').strip()
    cap = '%s/%s/capture' % (home, g['dest'])
    wine_cap = 'Z:' + cap.replace('/', '\\')
    deck.run('rm -rf %s && mkdir -p %s' % (cap, cap))
    # The capture folder for this try: try.flags, which play.sh passes on.
    gameid = (appid << 32) | 0x02000000
    # The renderer's frame statistics every 120 frames (FRAME_LINE reads the last of them).
    flags = ('--rexnative_capture_dir=%s --rexnative_capture_xenos=false '
             '--rexnative_d3d_stats_frames=120' % wine_cap)
    deck.run('cd %s && printf "%%s\\n" %s > try.flags' % (g['dest'], shlex.quote(flags)))
    deck.run('DISPLAY=:0 nohup steam steam://rungameid/%d >/dev/null 2>&1 &' % gameid, check=False)
    log('%s: started (%d s)' % (g['title'], seconds))
    time.sleep(seconds)
    deck.run('touch %s/capture.request' % cap, check=False)
    time.sleep(4)
    exe_re = re.escape(g['name']) + r'\.exe --log'
    running = deck.run('pgrep -f %s >/dev/null && echo yes || echo no' % shlex.quote(exe_re),
                       check=False).strip() == 'yes'
    logtext = deck.run('cd %s && l=$(ls -1t logs/*.log 2>/dev/null | head -1) && cat "$l"' %
                       g['dest'], check=False)
    deck.run('p=$(pgrep -f %s); [ -n "$p" ] && kill $p; rm -f %s/try.flags; true' % (
        shlex.quote(exe_re), g['dest']), check=False)
    time.sleep(10)  # Steam sees the game end before the next one starts
    frames = FRAME_LINE.findall(logtext)
    errors = [l for l in logtext.splitlines() if '[error]' in l or '[critical]' in l]
    fatal = [l for l in errors if 'FATAL' in l or 'access violation' in l or 'crash' in l.lower()]
    shots = deck.run('ls %s/*.png 2>/dev/null' % cap, check=False).split()
    out_dir = os.path.join(g['work'], 'deck')
    saved = []
    for s in shots:
        if 'xenos' in os.path.basename(s):
            continue
        data = deck.read(s)
        if data:
            p = os.path.join(out_dir, 'try-' + os.path.basename(s))
            open(p, 'wb').write(data)
            saved.append(p)
    with open(os.path.join(out_dir, 'try.log'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(logtext)
    last = frames[-1] if frames else None
    res = {'title': g['title'], 'running': running, 'frames': int(last[0]) if last else 0,
           'draws': float(last[1]) if last else 0.0, 'frame_ms': float(last[2]) if last else 0.0,
           'fatal': fatal[:3], 'errors': len(errors), 'captures': saved}
    log('%s: %s, %s%s' % (
        g['title'], 'running' if running else 'NOT running',
        ('%d frames, %.1f ms a frame (%.0f FPS), %.0f draws' % (
            res['frames'], res['frame_ms'], 1000.0 / res['frame_ms'] if res['frame_ms'] else 0,
            res['draws'])) if last else 'no frame stats',
        ('; ' + fatal[0][-160:]) if fatal else ''))
    return res
