"""Title updates: find a game's latest title update, fetch it and install its XEX patches.

A title update is an STFS package (content type 0x000B0000) holding XEX patches (default.xexp,
and a .xexp per other module it changes): LZX deltas over the base image. ReXGlue recompiles the
image it loads, and its loader applies the patch beside the XEX (UserModule::LoadFromFile:
game:\\default.xex + "p"), for codegen as for the game: installed into game-data next to
default.xex, the patch is in the code codegen writes and in what the game loads (the app refuses
code recompiled from another image). So a title update is installed before codegen, and a game
that gets one is prepared again from codegen on.

Where they come from: XboxUnity's listing (TitleUpdateInfo.php?titleid=: per media id, each
update's id, number, base version, size and the package's SHA-1) and download
(TitleUpdate.php?tuid=), as Aurora, X360-TU-Manager and skate3recomp's installer use them. Its
HTTPS endpoint can hang (IPv6, skate3recomp's notes): HTTP is used, and every package is checked
against the listing's SHA-1 and opened as an STFS title update of the game's title before any
file of it is installed. Nothing is fetched unless asked (`rexnative prepare --title-update`;
`rexnative title-update <work>` only lists them).
"""
import glob
import hashlib
import json
import os
import http.client
import shutil
import time
import urllib.error
import urllib.request

from . import stfs, xex

INFO_URL = 'http://xboxunity.net/Resources/Lib/TitleUpdateInfo.php?titleid=%s'
FILE_URL = 'http://xboxunity.net/Resources/Lib/TitleUpdate.php?tuid=%s'
USER_AGENT = 'rexnative/1.0 (+https://github.com/gtguerreiro-max/ReXNative)'
TITLE_UPDATE = 0x000B0000


# XboxUnity answers 503 (and 502/504, or drops the connection) for a while at times: the
# request is made again after these pauses before it counts as failed.
RETRY_PAUSES = (5, 20, 60)


def _transient(e):
    if isinstance(e, urllib.error.HTTPError):
        return e.code in (429, 500, 502, 503, 504)
    return isinstance(e, (OSError, http.client.HTTPException))


def _get(url, timeout=60, log=None):
    for pause in RETRY_PAUSES + (None,):
        req = urllib.request.Request(url, headers={'User-Agent': USER_AGENT})
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return r.read()
        except (OSError, http.client.HTTPException) as e:
            if pause is None or not _transient(e):
                raise
            if log:
                log('  %s: %s; again in %d s' % (url, e, pause))
            time.sleep(pause)


class Unreachable(RuntimeError):
    """XboxUnity did not answer (after the retries): prepare goes on without a title update."""


def game_xex(work):
    """game-data's default.xex as the game names it (a disc's DEFAULT.XEX, a package's Default.xex:
    Linux tells the names apart), else the usual name."""
    from .prepare import entry_xex
    data = os.path.join(work, 'game-data')
    return entry_xex(data) or os.path.join(data, 'default.xex')


def available(title_id, media_id, log=None):
    """The title updates XboxUnity lists for this media id, newest first:
    [{'tuid', 'version' (int), 'base_version', 'sha1', 'size' (int, KB), 'date'}]."""
    try:
        raw = _get(INFO_URL % title_id, log=log)
    except (OSError, http.client.HTTPException) as e:
        raise Unreachable('the XboxUnity title update listing: %s' % e) from None
    data = json.loads(raw.decode('utf-8', 'replace'))
    out = []
    for m in data.get('MediaIDS') or []:
        if str(m.get('MediaID', '')).upper() != media_id.upper():
            continue
        for u in m.get('Updates') or []:
            out.append({'tuid': str(u['TitleUpdateID']), 'version': int(u['Version']),
                        'base_version': str(u.get('BaseVersion', '')).upper(),
                        'sha1': str(u.get('hash', '')).upper(), 'size': int(u.get('Size') or 0),
                        'date': u.get('UploadDate', '')})
    out.sort(key=lambda u: u['version'], reverse=True)
    return out


def latest(work):
    """(game info from xex.read, the newest title update for its media id and base version or
    None)."""
    info = xex.read(game_xex(work))
    ups = [u for u in available(info['title_id'], info['media_id'])
           if not u['base_version'] or u['base_version'] == info['version']]
    return info, (ups[0] if ups else None)


def check(path, update):
    """Problems with a downloaded package (empty: good): the listing's hash is the package's
    content id (the SHA-1 of its header, which holds the hash of its block hash tree), and the
    package must be whole against it (stfs.Package.verify)."""
    try:
        pkg = stfs.Package(path)
    except (OSError, ValueError) as e:
        return ['not an STFS package: %s' % e]
    try:
        problems = []
        if update['sha1'] and pkg.content_id() != update['sha1']:
            problems.append('content id %s, the listing says %s' % (pkg.content_id(), update['sha1']))
        return problems + pkg.verify()
    finally:
        pkg.close()


def fetch(work, update, log=print):
    """Downloads the package into <work>/title-update/ and checks it (check); returns its path."""
    folder = os.path.join(work, 'title-update')
    os.makedirs(folder, exist_ok=True)
    path = os.path.join(folder, 'TU_%d_%s.bin' % (update['version'], update['tuid']))
    if os.path.exists(path) and not check(path, update):
        return path
    log('  downloading title update %d (%d KB) from %s' % (
        update['version'], update['size'], FILE_URL % update['tuid']))
    try:
        data = _get(FILE_URL % update['tuid'], timeout=3600, log=log)
    except (OSError, http.client.HTTPException) as e:
        raise Unreachable('title update %s: %s' % (update['tuid'], e)) from None
    part = path + '.part'
    with open(part, 'wb') as f:
        f.write(data)
    problems = check(part, update)
    if problems:
        os.remove(part)
        raise RuntimeError('title update %s refused: %s' % (update['tuid'], '; '.join(problems[:4])))
    os.replace(part, path)
    log('  checked: content id %s, every block against it' % update['sha1'])
    return path


def install(work, package, title_id, log=print):
    """Extracts the package into <work>/title-update/content (run/title-update links to it: the
    app mounts it as update:, for whatever data files the update brings) and copies its XEX
    patches into game-data (default.xexp beside default.xex, for codegen and the loader);
    returns the patches installed. The package must be a title update of `title_id`."""
    pkg = stfs.Package(package)
    try:
        if pkg.content_type != TITLE_UPDATE:
            raise RuntimeError('%s is not a title update (content type %08X)' % (
                package, pkg.content_type))
        if '%08X' % pkg.title_id != title_id.upper():
            raise RuntimeError('%s is for title %08X, not %s' % (package, pkg.title_id, title_id))
        tmp = os.path.join(work, 'title-update', 'content')
        if os.path.isdir(tmp):
            shutil.rmtree(tmp)
        pkg.extract(tmp)
    finally:
        pkg.close()
    game = os.path.join(work, 'game-data')
    installed = []
    for root, _, files in os.walk(tmp):
        for name in files:
            if not name.lower().endswith('.xexp'):
                continue
            src = os.path.join(root, name)
            rel = os.path.relpath(src, tmp)
            # A delta patch (module flags 0x10 | 0x40) has no execution info of its own: the
            # package's title id was checked above, and the loader checks the base image's
            # digest before applying it.
            info = xex.read(src)
            if not info['patch']:
                raise RuntimeError('%s: not an XEX patch' % rel)
            dst = os.path.join(game, rel)
            # Named after the image it patches as that one is named (Default.xex: Default.xexp):
            # the loader looks for the image's name + "p", and Linux tells the cases apart.
            folder = os.path.dirname(dst)
            if os.path.isdir(folder):
                for f in os.listdir(folder):
                    if (f + 'p').lower() == os.path.basename(dst).lower() and f.lower().endswith('.xex'):
                        dst = os.path.join(folder, f + 'p')
                        break
            if not os.path.realpath(dst).startswith(os.path.realpath(game) + os.sep):
                raise RuntimeError('%s: outside the game folder' % rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(src, dst)
            installed.append(rel)
            log('  installed %s' % rel)
    if not installed:
        raise RuntimeError('%s holds no XEX patch' % package)
    return installed


def record(work, info, update, installed):
    """<work>/title-update/installed.json: what is installed, for prepare and the report."""
    with open(os.path.join(work, 'title-update', 'installed.json'), 'w', encoding='utf-8') as f:
        json.dump({'title_id': info['title_id'], 'media_id': info['media_id'],
                   'base_version': info['version'], 'title_update': update['version'],
                   'tuid': update['tuid'], 'sha1': update['sha1'], 'files': installed}, f, indent=1)


def installed(work):
    """The record of the installed title update, or None."""
    p = os.path.join(work, 'title-update', 'installed.json')
    return json.load(open(p, encoding='utf-8')) if os.path.exists(p) else None


# What a work folder derives from the image codegen loaded: made again for the patched one.
DERIVED = ('generated', 'layer', 'codegen.ok', 'switch_tables.toml', 'setjmp.toml', 'crt.toml',
           'title_hooks.toml',
           os.path.join('run', 'image.bin'))


def reset(work, log=print):
    """Removes what was derived from the image before the title update (hints.txt is kept as
    hints.base.txt: function starts of the old image could cut functions of the new one)."""
    for rel in DERIVED:
        p = os.path.join(work, rel)
        if os.path.isdir(p):
            shutil.rmtree(p)
        elif os.path.exists(p):
            os.remove(p)
    # The DLL modules' jump tables, their hooks and images (project.write_switch_tables).
    for pattern in ('switch_tables.*.toml', os.path.join('run', 'image.*.bin'),
                    os.path.join('src', 'cases_*.cpp')):
        for p in glob.glob(os.path.join(work, pattern)):
            os.remove(p)
    hints = os.path.join(work, 'hints.txt')
    if os.path.exists(hints):
        os.replace(hints, os.path.join(work, 'hints.base.txt'))
    log('  codegen, the layer and the hints start again from the patched image')


def update(work, log=print, version=None):
    """Installs the newest title update XboxUnity lists for the game's media id and version (or
    title update `version`), unless it is the one installed; then resets what was derived from
    the old image (reset). Returns the step's status for prepare.json."""
    info = xex.read(game_xex(work))
    ups = [u for u in available(info['title_id'], info['media_id'], log)
           if not u['base_version'] or u['base_version'] == info['version']]
    if version is not None:
        ups = [u for u in ups if u['version'] == version]
    if not ups:
        log('  title update: none listed for %s media %s version %s' % (
            info['title_id'], info['media_id'], info['version']))
        return {'title_update': None}
    u = ups[0]
    cur = installed(work)
    if cur and cur.get('tuid') == u['tuid']:
        return {'title_update': u['version'], 'tuid': u['tuid'], 'reused': True}
    pkg = fetch(work, u, log)
    if cur:  # another title update's patches go first
        for rel in cur.get('files', []):
            p = os.path.join(work, 'game-data', rel)
            if os.path.exists(p):
                os.remove(p)
    files = install(work, pkg, info['title_id'], log)
    record(work, info, u, files)
    reset(work, log)
    log('  title update %d installed (%s)' % (u['version'], ', '.join(files)))
    return {'title_update': u['version'], 'tuid': u['tuid'], 'files': files}
