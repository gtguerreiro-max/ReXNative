"""A game `prepare --project` built, on the Steam Deck: copied over SSH, added to Steam, tried.

  python -m rexnative deck <work>/<game>... [--host deck@ADDRESS[:PORT]] [--key FILE]
         [--root FOLDER|sd] [--steam] [--try SECONDS]

Each game goes to ~/Games/ReXNative/<game>/ on the Deck: its Windows build from run/ (the
executable and DLLs, without NVIDIA's, which the Deck cannot use), its files in game/, its
title update's in title-update/ (mounted as update:), deck/play.sh (Proton with the prefix every
game shares, ~/Games/ReXNative/prefix, FSR 4 through GE-Proton), deck/install.sh and deck/steam_shortcuts.py, and
<name>-deck.toml, the game's settings for the Deck's screen (full screen at 1280x800), which
install.sh makes its settings unless the Deck has some already. Only files whose size or time
differ are sent again, so a rebuilt game sends its executable alone.

--root puts the games in another folder of the Deck (an absolute path, or one under the deck
user's home): `--root sd` is ReXNative/ on the microSD card (the first one mounted). The Proton
prefix stays the internal one (~/Games/ReXNative/prefix, which play.sh falls back to); AMD's
FidelityFX DLLs go to the card's ReXNative/common, beside the games, where the renderer looks.

--steam adds every game given to Steam's library (Steam is closed for it, `steam -shutdown`:
it writes its shortcuts file back when it exits; in Game Mode its unit is masked meanwhile, and
Steam started again after). --try SECONDS
starts each game from Steam in turn, as a player would (the game then has Game Mode's focus),
and after SECONDS reads its log (the frame times, errors, a crash) and takes a capture of the
screen through the renderer (--rexnative_capture_dir), copied back beside the game's work folder
in deck/, then closes it.

The Deck's address and SSH key: --host / --key, or REXNATIVE_DECK ("deck@192.168.1.10:22")
and REXNATIVE_DECK_KEY. Nothing of the game leaves the computer but for the Deck.
"""
import json
import os
import posixpath
import re
import shlex
import shutil
import subprocess
import tarfile
import time

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KIT = os.path.join(HERE, 'deck')
REMOTE_ROOT = 'Games/ReXNative'  # under the deck user's home
# <root>/.removed/<slug>: a game its user removed from the Deck (the file holds when). install()
# does not send it back unless asked (--reinstall): a batch that sends a list of games again
# would otherwise put back, without its Steam shortcut, what the user had just removed.
REMOVED = '.removed'
q = shlex.quote

# The Deck's settings, as Iron Brigade's deck/ironbrigade-deck.toml has them (ReXGlue v0.11
# sections): its 1280x800 panel, full screen, and the native renderer's anisotropic filtering.
# The video mode the game is told about stays the console's 720p: told 1280x800, a mode no Xbox
# 360 has, Castlevania: Symphony of the Night and 3D Ultra Minigolf stopped at their first frames.
# The scene's antialiasing is AMD's FSR 4 (GE-Proton's FSR 4 upgrade, play.sh; FSR 3.1 where it
# is missing) at the game's own resolution (Native AA): on both Overlords' first levels it kept
# their 30 FPS, the GPU at 13 and 18 ms of 33, and looked better than the console's MSAA, which
# Quality mode saves only 1-3 ms over (2026-11-17). The settings menu (Back+Start) changes it.
DECK_SETTINGS = [
    ('ui', 'window_width', '1280'),
    ('ui', 'window_height', '800'),
    ('ui', 'fullscreen', 'true'),
    ('ui', 'resolution', '"720p"'),
    ('rexnative', 'anisotropy', '16'),
    ('rexnative', 'upscaler', 'true'),
    ('rexnative', 'upscaler_mode', '"native"'),
    # One frame begun ahead of the display: the picture 8-22 ms sooner after the pad than at 2.
    # Before the XDK's spin waits slept (renderer/src/spin_pause.h), the CPU spinning between frames
    # took the power the GPU needed and Midnight Club's races fell to 22-29 FPS at 1 (2026-11-18);
    # with them asleep, driving its city held 30 FPS at 1 (29.6 against 29.1 at 2, begin to shown
    # 33 ms against 46 ms, 0.2 W more), as did Jet Set Radio (2026-11-20). The settings HUD's Frame
    # latency changes it, live; 2 where a game's frames need the overlap.
    ('rexnative', 'frame_latency', '1'),
    # The game at its own resolution: 2x and 3x (the PC HUD's Resolution, not shown on the Deck) came
    # along from the PC's settings and halved Forza Horizon's frame rate there (2026-11-24). The
    # renderer also takes 1x on the Deck whatever the file says (renderer.cpp).
    ('rexnative', 'resolution_scale', '1'),
    # No NVIDIA GPU: DLSS 5 off (Forza Horizon's, on for the PC, had NGX tried and failing again and
    # again there) and the upscaler's type automatic, which is FSR on the Deck (a PC's "dlss" left
    # it without an upscaler).
    ('rexnative', 'dlss5', 'false'),
    ('rexnative', 'upscaler_backend', '"auto"'),
    # On the battery, the console's MSAA in place of the upscaler (renderer.cpp, WatchBattery): the
    # APU at 4.7 W against 6.2 with FSR 4 in Overlord II; back on the charger, the upscaler again.
    ('rexnative', 'battery_saver', 'true'),
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
        # A copy over Wi-Fi outlives a roam between the access point's bands (seconds without
        # packets); a Deck gone for two minutes ends ssh instead of leaving it waiting.
        cmd = ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10',
               '-o', 'ServerAliveInterval=15', '-o', 'ServerAliveCountMax=8']
        if self.port:
            cmd += ['-p', self.port]
        if self.key:
            cmd += ['-i', os.path.expanduser(self.key)]
        return cmd + [self.host, command]

    def call(self, command, timeout=600):
        """The command's CompletedProcess (returncode, stdout, stderr as text)."""
        return subprocess.run(self.ssh_cmd(command), capture_output=True, text=True,
                              errors='replace', timeout=timeout)

    def run(self, command, check=True, timeout=600):
        r = self.call(command, timeout)
        if check and r.returncode != 0:
            raise SystemExit('the Deck: `%s` failed (%d): %s' % (command, r.returncode,
                                                                   (r.stderr or r.stdout)[-600:]))
        return r.stdout

    def read(self, path, timeout=120):
        r = subprocess.run(self.ssh_cmd('cat %s' % q(path)), capture_output=True, timeout=timeout)
        return r.stdout if r.returncode == 0 else None

    def files(self, dest):
        """{relative path: (size, mtime)} under dest on the Deck."""
        out = self.run('mkdir -p %s && cd %s && find . -type f -printf "%%P\\t%%s\\t%%T@\\n"' % (
            q(dest), q(dest)))
        have = {}
        for line in out.splitlines():
            parts = line.split('\t')
            if len(parts) == 3:
                have[parts[0]] = (int(parts[1]), int(float(parts[2])))
        return have

    def _plan(self, dest, items, log):
        """(what the Deck has, [(local, relative, size)] to send): those the Deck has (same size
        and time) are left out, and what an older install left there that would be taken for the
        game's goes (another executable, which play.sh could start, or its deck settings; an
        older title update's files, mounted as update:; a patch file FPS Boost no longer
        wants, which the game would apply over code built without it)."""
        have = self.files(dest)
        wanted = {rel for _, rel in items}
        from .project import COMMON_DLLS
        stale = sorted(rel for rel in have if rel not in wanted and (
            ('/' not in rel and (rel.lower().endswith(('.exe', '-deck.toml')) or
                                 rel.lower() in COMMON_DLLS)) or
            rel.startswith('title-update/') or
            (rel.startswith('game/') and rel.lower().endswith('.patch.toml'))))
        if stale:
            log('  %d files of an older install removed' % len(stale))
            for i in range(0, len(stale), 200):
                self.run('cd %s && rm -f -- %s' % (q(dest), ' '.join(q(r) for r in stale[i:i + 200])))
        todo = []
        for src, rel in items:
            st = os.stat(src)
            if have.get(rel) != (st.st_size, int(st.st_mtime)):
                todo.append((src, rel, st.st_size))
        return have, todo

    def send(self, dest, items, log=print):
        """items: [(local path, relative path on the Deck)], copied into dest as _plan says, as
        one tar stream; returns the bytes sent."""
        have, todo = self._plan(dest, items, log)
        if not todo:
            return 0
        total = sum(t[2] for t in todo)
        log('  %d files, %.0f MB' % (len(todo), total / 1e6))
        # What the copy needs on the Deck's file system: less what the files it replaces hold
        # (tar unlinks each one before writing it), plus 256 MB the Deck keeps for itself.
        need = total - sum(have[rel][0] for _, rel, _ in todo if rel in have)
        free = self.call('df -B1 --output=avail %s | tail -1' % q(dest), timeout=60).stdout.strip()
        if free.isdigit() and need > int(free) - (256 << 20):
            raise SystemExit('the Deck: %.2f GB to copy into %s and %.2f GB free there: remove '
                             'games from the Deck, or --root sd for the microSD card' % (
                                 need / 1e9, dest, int(free) / 1e9))
        p = subprocess.Popen(self.ssh_cmd('tar -xf - -C %s' % q(dest)), stdin=subprocess.PIPE,
                             stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        # Progress every 3 s ("copied N of M MB": the Gestor app draws its bar from these).
        done = {'bytes': 0, 'said': time.time()}

        class Counted:
            def __init__(self, f):
                self.f = f

            def read(self, n=-1):
                b = self.f.read(n)
                done['bytes'] += len(b)
                if time.time() - done['said'] >= 3:
                    done['said'] = time.time()
                    log('  copied %d of %d MB' % (done['bytes'] // 1000000, total // 1000000))
                return b

        try:
            with tarfile.open(fileobj=p.stdin, mode='w|', format=tarfile.GNU_FORMAT) as tar:
                for src, rel, size in todo:
                    info = tar.gettarinfo(src, arcname=rel)
                    info.uid = info.gid = 1000
                    info.uname = info.gname = 'deck'
                    if rel.endswith('.sh'):
                        info.mode = 0o755
                    with open(src, 'rb') as f:
                        tar.addfile(info, Counted(f))
        except OSError as e:
            # ssh ended under the copy (the network dropped, the Deck slept or filled up); a file
            # of the PC that would not read leaves ssh waiting for the rest instead.
            try:
                p.wait(timeout=15)
            except subprocess.TimeoutExpired:
                p.kill()
                raise e
            err = p.stderr.read().decode(errors='replace').strip()
            raise SystemExit('the Deck: the copy into %s stopped after %.0f of %.0f MB (%s); '
                             'deck again resumes it: what arrived stays' % (
                                 dest, done['bytes'] / 1e6, total / 1e6, err[-400:] or e))
        p.stdin.close()
        err = p.stderr.read().decode(errors='replace')
        if p.wait() != 0:
            raise SystemExit('the Deck: copying into %s failed: %s' % (dest, err[-600:]))
        log('  copied %d of %d MB' % (total // 1000000, total // 1000000))
        return total


class LocalDeck(Deck):
    """This machine as the Deck: the tool runs on the Steam Deck itself (ReXNativeDeck). The same
    commands run in a local shell from the home folder, and the game's files are linked or copied
    into its folder instead of sent over SSH."""
    # The game's own files (its disc or package, its title update) never change once there: they
    # are hard links to the work folder's copy where both are on one file system, so a game the
    # Deck prepared for itself does not take its space twice. The rest (the executable, the DLLs,
    # the settings) is copied: a build rewrites those in place.
    LINKED = ('game/', 'title-update/')

    def __init__(self):
        self.host, self.port, self.key = 'localhost', None, None

    def ssh_cmd(self, command):
        return ['bash', '-c', 'cd ~ && ' + command]

    def send(self, dest, items, log=print):
        have, todo = self._plan(dest, items, log)
        if not todo:
            return 0
        root = dest if dest.startswith('/') else os.path.join(os.path.expanduser('~'), dest)
        copied = [t for t in todo if not t[1].startswith(self.LINKED)]
        total = sum(t[2] for t in todo)
        need = sum(t[2] for t in copied) - sum(have[rel][0] for _, rel, _ in copied if rel in have)
        free = shutil.disk_usage(root if os.path.isdir(root) else os.path.dirname(root)).free
        if need > free - (256 << 20):
            raise SystemExit('this Deck: %.2f GB to copy into %s and %.2f GB free there: remove '
                             'games, or --root sd for the microSD card' % (need / 1e9, root,
                                                                           free / 1e9))
        log('  %d files, %.0f MB' % (len(todo), total / 1e6))
        done, said, linked = 0, time.time(), 0
        for src, rel, size in todo:
            dst = os.path.join(root, *rel.split('/'))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            if os.path.lexists(dst):
                os.remove(dst)  # a link to the old file: never write through it
            if rel.startswith(self.LINKED):
                try:
                    os.link(src, dst)
                    linked += size
                except OSError:  # another file system (the microSD card)
                    shutil.copy2(src, dst)
            else:
                shutil.copy2(src, dst)
            if rel.endswith('.sh'):
                os.chmod(dst, 0o755)
            done += size
            if time.time() - said >= 3:
                said = time.time()
                log('  copied %d of %d MB' % (done // 1000000, total // 1000000))
        log('  copied %d of %d MB%s' % (total // 1000000, total // 1000000,
                                        ' (%d MB of it linked, not copied)' % (linked // 1000000)
                                        if linked >= 1000000 else ''))
        return total


def walk(root, prefix):
    out = []
    for d, dirs, fs in os.walk(root):
        dirs.sort()
        for f in sorted(fs):
            p = os.path.join(d, f)
            out.append((p, prefix + os.path.relpath(p, root).replace(os.sep, '/')))
    return out


def _tidy_title(name):
    """A title's name as the SPA has it, for a Steam shortcut: no trademark signs, and words in
    capitals alone given lower case ("ASURA'S WRATH" -> "Asura's Wrath"; "SSX" stays)."""
    name = re.sub('[™®©�]', '', name).strip()  # TM, R, C, broken
    if name.isupper() and ' ' in name:
        name = ' '.join(w[:1] + w[1:].lower() for w in name.split())
    return name


def title_of(work):
    """The game's name from its archive's: 'Jet Set Radio (World) (XBLA).7z' -> 'Jet Set Radio'.
    A scene release's ('Asuras.Wrath.USA.X360-ZTM.rar') gives the title's own name instead, from
    its SPA (xex.spa_title over run/image.bin) when there is one."""
    try:
        src = json.load(open(os.path.join(work, 'prepare.json'), encoding='utf-8'))['source']
    except (OSError, KeyError, ValueError):
        src = os.path.basename(os.path.abspath(work))
    base = re.sub(r'\.(7z|zip|rar|iso)$', '', os.path.basename(src), flags=re.I)
    if ' ' not in base and re.search(r'\.(X360|XBOX360)\b|-[A-Za-z0-9]+$', base):
        from . import xex
        own = xex.spa_title(os.path.join(work, 'run', 'image.bin'))
        if own and _tidy_title(own):
            return _tidy_title(own)
        base = re.sub(r'(\.(USA|PAL|EUR|NTSC|JPN|MULTI\d*|X360|XBOX360|DLC|RF))+(-[^.]+)?$', '',
                      base, flags=re.I).replace('.', ' ').replace('_', ' ')
    base = re.sub(r'\s*\([^)]*\)', '', base).strip()
    return base.replace(' - ', ': ', 1) if ' - ' in base else base


def package(work):
    """(executable name, [(local, relative)]) of a built game's Deck folder."""
    from . import project
    run = os.path.join(work, 'run')
    exes = [f for f in os.listdir(run) if f.lower().endswith('.exe')] if os.path.isdir(run) else []
    if len(exes) > 1:
        # The project's own executable (another one in run/ is a leftover of a test).
        own = project.manifest_name(work) + '.exe'
        exes = [f for f in exes if f.lower() == own.lower()]
    if len(exes) != 1:
        raise SystemExit('%s: no built game in run/ (rexnative prepare --project)' % work)
    name = exes[0][:-4]
    items = []
    for f in sorted(os.listdir(run)):
        p = os.path.join(run, f)
        if (os.path.isfile(p) and not f.lower().startswith('nvngx_') and
                f.lower() not in project.COMMON_DLLS and
                (f.lower().endswith('.dll') or f == name + '.exe')):
            items.append((p, f))
    data = os.path.join(work, 'game-data')
    items += walk(data, 'game/')
    tu = os.path.join(work, 'title-update', 'content')
    if os.path.isdir(tu):
        items += walk(tu, 'title-update/')
    # Downloadable content (rexnative dlc), in userdata/ beside the game as on the PC.
    from . import dlc
    items += dlc.deck_items(work)
    for f in ('play.sh', 'install.sh', 'steam_shortcuts.py'):
        items.append((os.path.join(KIT, f), f))
    # The title's achievements list (its SPA, a few KB from run/image.bin): the Manager on the Deck
    # shows the achievements of a game the PC built from it.
    from . import xex
    spa = xex.spa_block(os.path.join(run, 'image.bin'))
    if spa:
        spa_path = os.path.join(work, 'deck', 'achievements.spa')
        os.makedirs(os.path.dirname(spa_path), exist_ok=True)
        if not os.path.exists(spa_path) or open(spa_path, 'rb').read() != spa:
            with open(spa_path, 'wb') as f:
                f.write(spa)
        items.append((spa_path, 'achievements.spa'))
    # The shortcut's artwork (python -m rexnative art), which install.sh gives Steam.
    art = os.path.join(work, 'deck', 'art')
    if os.path.isdir(art):
        items += walk(art, 'art/')
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


def remote_dir(dest):
    """A folder of the Deck as a shell word: dest absolute, or under the deck user's home."""
    return q(dest) if dest.startswith('/') else '~/' + q(dest)


def sd_root(deck, log=print):
    """ReXNative/ on the Deck's microSD card (its first partition, mounted where SteamOS puts it:
    /run/media/deck/<label> or /run/media/mmcblk0p1), created if missing; exits when no card is
    mounted. Logs its file system and free space."""
    out = deck.run('for d in /dev/mmcblk0p1 /dev/mmcblk0; do '
                   'm=$(findmnt -rn -o TARGET,FSTYPE,AVAIL -S "$d" 2>/dev/null | head -1); '
                   '[ -n "$m" ] && { echo "$m"; break; }; done', check=False).strip()
    if not out:
        raise SystemExit('the Deck: no microSD card mounted (insert it; SteamOS formats it from '
                         'Settings > Storage)')
    target, fstype, avail = (out.split() + ['?', '?'])[:3]
    target = target.replace('\x20', ' ')
    log('microSD card at %s (%s, %s free)' % (target, fstype, avail))
    root = target.rstrip('/') + '/ReXNative'
    deck.run('mkdir -p %s' % q(root))
    return root


def install(deck, work, log=print, root=REMOTE_ROOT, reinstall=False):
    """The game copied to the Deck and set up; None when its user removed it from the Deck
    (<root>/.removed/<slug>) and reinstall is not set."""
    work = os.path.abspath(work)
    slug = os.path.basename(work)
    title = title_of(work)
    mark = '%s/%s/%s' % (root, REMOVED, slug)
    if reinstall:
        deck.run('rm -f %s' % q(mark), check=False)
    else:
        r = deck.call('cat %s' % q(mark), timeout=60)
        if r.returncode == 0:
            log('%s: removed from the Deck by its user (%s): not sent (--reinstall sends it '
                'again)' % (title, r.stdout.strip() or mark))
            return None
    name, items = package(work)
    dest = '%s/%s' % (root, slug)
    log('%s -> %s' % (title, dest if dest.startswith('/') else '~/' + dest))
    deck.send(dest, items, log=log)
    # The kit's play.sh in every game installed beside this one: a change to it (2026-11-25, the
    # GPU's clock floor while a game plays) reaches the games installed before at the next install.
    out = deck.run('cd %s && for d in */; do d=${d%%/}; if [ "$d" != %s ] && [ -f "$d/play.sh" ] '
                   '&& ! cmp -s %s/play.sh "$d/play.sh"; then cp %s/play.sh "$d/play.sh" && '
                   'echo "$d"; fi; done; true' % (q(root), q(slug), q(slug), q(slug)), check=False)
    if out.strip():
        log('  play.sh updated in %d other games' % len(out.split()))
    # AMD's FidelityFX DLLs, once for every game in ~/Games/ReXNative/common, where the renderer
    # looks when they are not beside the executable (renderer/src/native/common_dir.h): 87 MB a
    # game less on the Deck. NVIDIA's are not sent (the Deck has no RTX).
    from . import project
    common = []
    for n in project.COMMON_DLLS:
        if n.startswith('nvngx_'):
            continue
        for src in (os.path.join(work, 'run', n), os.path.join(project.common_dir(work), n)):
            if os.path.exists(src):
                common.append((src, n))
                break
    if common:
        deck.send('%s/common' % root, common, log=log)
    # The Proton prefix every game shares (play.sh): the first game's own prefix becomes it (with
    # GE-Proton's FSR 4 library already fetched), the others' go (400 MB each; a prefix holds no
    # game's files, saves or settings: those are in the game's folder). Games in another --root
    # use the internal one (play.sh falls back to it): a card's file system may hold no prefix.
    if root == REMOTE_ROOT:
        deck.run('cd %s && if [ -d %s/prefix ]; then if [ -d prefix ]; then rm -rf %s/prefix; '
                 'else mv %s/prefix prefix; fi; fi' % (q(root), q(slug), q(slug), q(slug)),
                 check=False)
    out = deck.run('cd %s && bash install.sh %s' % (q(dest), q(title)))
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
            log('the Deck is in use (a game, or another app, runs through Steam): waiting up to %d min'
                % minutes)
        time.sleep(30)
    return False


def add_to_steam(deck, games, log=print, minutes=120):
    """Every game's shortcut, with Steam closed for it (Game Mode starts it again): True when
    all were added, False when some were not, None when the Deck stayed in use (minutes: how long
    to wait for it; the ReXNative Manager waits 2, as the Manager open on the Deck counts as in use
    and it adds the shortcut itself when the Manager closes)."""
    if not wait_idle(deck, log, minutes):
        log('the Deck stayed in use: the shortcuts are not added')
        return None
    # In Game Mode gamescope-session.target upholds steam-launcher.service: Steam closed with
    # `steam -shutdown` is started again at once, before the shortcuts could be written (and
    # steam_shortcuts.py refuses while it runs). The unit is masked for this script alone
    # (--runtime: gone at a reboot too), and the trap unmasks it and starts Steam again whatever
    # happens. Steam is left 60 s after it started first: SteamOS's short session tracker
    # re-bootstraps a Steam that ends three times within a minute of its start.
    adds = '\n'.join('(cd %s && bash install.sh %s) || failed=1' % (remote_dir(g['dest']),
                                                                       q(g['title']))
                     for g in games)
    script = '''
export XDG_RUNTIME_DIR=/run/user/$(id -u) DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$(id -u)/bus
masked=
desktop=
failed=
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
  age=$(ps -o etimes= -p "$pid" | tr -d ' ')
  [ "${age:-0}" -lt 70 ] && sleep $(( 70 - ${age:-0} ))
  if systemctl --user is-active --quiet gamescope-session.target 2>/dev/null; then
    systemctl --user mask --runtime steam-launcher.service >/dev/null && masked=1
  fi
  # A game started while this waited (the 70 s above, or the copy before it): Steam stays.
  if pgrep -f "[S]teamLaunch AppId=" >/dev/null; then echo "a game started on the Deck: Steam not closed"; exit 3; fi
  echo "closing Steam to add the shortcuts"
  steam -shutdown >/dev/null 2>&1
  for i in $(seq 1 120); do pgrep -x steam >/dev/null || break; sleep 0.5; done
  if pgrep -x steam >/dev/null; then echo "Steam did not close"; exit 1; fi
fi
%s
[ -z "$failed" ]
''' % adds
    r = deck.call('bash -s <<"REXNATIVE_EOF"\n%s\nREXNATIVE_EOF' % script, timeout=300)
    for line in (r.stdout.strip() + '\n' + r.stderr.strip()).strip().splitlines():
        log('  ' + line)
    if r.returncode == 3:
        log('a game started on the Deck: the shortcuts are not added (Steam was not closed)')
        return None
    ok = r.returncode == 0
    if not ok:
        log('the shortcuts were not all added (exit %d)' % r.returncode)
    # Game Mode starts Steam again (the trap above, or by itself); a desktop session does not.
    for _ in range(90):
        if deck.run('pgrep -x steam >/dev/null && echo yes || echo no').strip() == 'yes':
            time.sleep(45)  # its library up before a game is started from it
            return ok
        time.sleep(2)
    log('Steam is not running again (a desktop session?): start it to see the games')
    return ok


def app_id(deck, g):
    out = deck.run('python3 %s/steam_shortcuts.py list' % q(g['dest']), check=False)
    for line in out.splitlines():
        parts = line.split(None, 1)
        if len(parts) == 2 and parts[1].startswith(g['title'] + '  '):
            return int(parts[0])
    return None


FRAME_LINE = re.compile(r'frames (\d+) \| per frame: draws ([\d.]+).*?frame ([\d.]+) ms')


def try_game(deck, g, seconds, log=print, minutes=120):
    """Starts the game from Steam, reads its log and takes a capture after `seconds`."""
    if not wait_idle(deck, log, minutes):
        log('%s: not tried, the Deck stayed in use' % g['title'])
        return None
    appid = app_id(deck, g)
    if appid is None:
        log('%s: not in Steam (--steam first)' % g['title'])
        return None
    home = deck.run('echo $HOME').strip()
    dest = q(g['dest'])
    cap = posixpath.join(home, g['dest'], 'capture')  # dest may be absolute (--root)
    wine_cap = 'Z:' + cap.replace('/', '\\')
    deck.run('rm -rf %s && mkdir -p %s' % (q(cap), q(cap)))
    # The capture folder for this try: try.flags, which play.sh passes on.
    gameid = (appid << 32) | 0x02000000
    # The renderer's frame statistics every 120 frames (FRAME_LINE reads the last of them).
    flags = ('--rexnative_capture_dir=%s --rexnative_capture_xenos=false '
             '--rexnative_d3d_stats_frames=120' % wine_cap)
    exe_re = re.escape(g['name']) + r'\.exe --log'
    deck.run('cd %s && printf "%%s\\n" %s > try.flags' % (dest, q(flags)))
    # With the panel off (Game Mode's idle, while nobody holds the Deck) no present is shown and a
    # game runs at about a frame a second (Midnight Club's "frame N not shown after 100 ms"):
    # Shift wakes it and keeps it on through the try (no game reads Shift alone).
    deck.run('XA=$(ls /run/user/1000/xauth_* 2>/dev/null | head -1); export DISPLAY=:0 '
             'XAUTHORITY=$XA; xdotool key shift; (for i in $(seq 1 %d); do sleep 30; '
             'xdotool key shift; done) >/dev/null 2>&1 < /dev/null & disown' % (seconds // 30 + 2),
             check=False)
    if playing(deck):  # a game started while this got ready: the try would end it
        deck.run('rm -f %s/try.flags' % dest, check=False)
        log('%s: not tried, a game started on the Deck' % g['title'])
        return None
    try:
        deck.run('DISPLAY=:0 nohup steam steam://rungameid/%d >/dev/null 2>&1 &' % gameid,
                 check=False)
        log('%s: started (%d s)' % (g['title'], seconds))
        time.sleep(seconds)
        deck.run('touch %s/capture.request' % q(cap), check=False)
        time.sleep(4)
        running = deck.run('pgrep -f %s >/dev/null && echo yes || echo no' % q(exe_re),
                           check=False).strip() == 'yes'
        # The newest log this launch wrote (newer than try.flags): a game that did not start
        # leaves the last run's, which would report its frame times as this one's.
        logtext = deck.run(
            'cd %s && l=$(find logs -maxdepth 1 -name "*.log" -newer try.flags -printf "%%T@ %%p\\n" '
            '2>/dev/null | sort -n | tail -1 | cut -d" " -f2-) && [ -n "$l" ] && cat "$l"' % dest,
            check=False)
    finally:
        # Also on Ctrl+C: try.flags left behind would go with every later start from Steam.
        deck.run('p=$(pgrep -f %s); [ -n "$p" ] && kill $p; rm -f %s/try.flags; true' % (
            q(exe_re), dest), check=False)
    time.sleep(10)  # Steam sees the game end before the next one starts
    frames = FRAME_LINE.findall(logtext)
    errors = [l for l in logtext.splitlines() if '[error]' in l or '[critical]' in l]
    fatal = [l for l in errors if 'FATAL' in l or 'access violation' in l or 'crash' in l.lower()]
    shots = deck.run('cd %s && ls *.png 2>/dev/null' % q(cap), check=False).split()
    out_dir = os.path.join(g['work'], 'deck')
    saved = []
    for s in shots:
        if 'xenos' in os.path.basename(s):
            continue
        data = deck.read(cap + '/' + s)
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
