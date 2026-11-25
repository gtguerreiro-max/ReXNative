"""Downloadable content (marketplace packages, content type 00000002) for a game `prepare` made.

Each STFS package is extracted where ReXGlue's content manager lists it for every profile,
run/userdata/0000000000000000/<title id>/00000002/<the package's file name>/, with the header file
ContentManager::InstallContent writes beside it (Headers/00000002/<name>.header: the
XCONTENT_AGGREGATE_DATA and the package's license mask), so the game finds it as installed
content. `rexnative deck` sends it along (userdata/ beside the game on the Deck).

  python -m rexnative dlc <work>/<game> <package | folder | .rar/.7z/.zip>...

Without packages, the game's DLC comes from the DLC sources you set: none come with the tool.
$REXNATIVE_DLC_SOURCES (several joined by `;`) or --dlc-source names each: a folder holding an
archive (.rar/.7z/.zip) per game, or an Internet Archive item holding one (its id, or its
archive.org/details/<id> address), each archive with <title id>/00000002/<package> inside:

  python -m rexnative dlc <work>/<game> | <prepare's --work>  [--list] [--dlc-source <source>]
  python -m rexnative prepare ... --dlc [--dlc-source <source>]

An archive is the game's when its name's words are the game's title's (roman numerals as
digits; "1", "Complete Edition" and "Director's Cut" left out); <work>/dlc-archives.txt names
them by hand, or says `none`. Archive.org's: one whose listing names other titles only is not
downloaded; each is checked against the item's MD5, installed as above (packages of another
title left out) and deleted; <work>/dlc.json records what each installed, and an archive is
fetched again only when the item's copy changes. Some items lend their files to logged-in
accounts only (HTTP 401 without): the account's cookies from the internetarchive tool's
configuration (`ia configure` writes ia.ini), or its S3 keys (IA_ACCESS_KEY_ID and
IA_SECRET_ACCESS_KEY, or ia.ini's [s3]). A folder's archives are installed from where they are
and left there. Nothing is downloaded unless asked, and nothing without a source.
"""
import configparser
import hashlib
import http.client
import json
import os
import re
import shutil
import struct
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

from . import prepare, stfs, title_update, xex

MARKETPLACE = 0x00000002
HDD = 1  # rex::xam::DummyDeviceID::kHDD, the device InstallContent gives
COMMON_XUID = '0' * 16  # marketplace content belongs to no profile


def packages_in(paths, tmp, log=print):
    """The STFS packages among paths: packages, folders (walked) and archives (extracted into
    tmp with 7-Zip)."""
    found = []
    for p in paths:
        if os.path.isdir(p):
            for d, dirs, files in os.walk(p):
                dirs.sort()
                found += [os.path.join(d, f) for f in sorted(files)
                          if stfs.is_package(os.path.join(d, f))]
        elif p.lower().endswith(prepare.ARCHIVES):
            out = tempfile.mkdtemp(dir=tmp)
            prepare.extract_archive(p, out, log)
            found += packages_in([out], tmp, log)
        elif stfs.is_package(p):
            found.append(p)
        else:
            raise SystemExit('%s: not an STFS package, a folder or an archive' % p)
    return found


def license_mask(path):
    """The license bits of the package's license entries that have flags, as
    ContentManager::BuildPackageInfo takes them (header 0x22C: 16 x id, bits, flags)."""
    with open(path, 'rb') as f:
        f.seek(0x22C)
        raw = f.read(0x100)
    mask = 0
    for i in range(0, len(raw), 0x10):
        _, bits, flags = struct.unpack('>QII', raw[i:i + 0x10])
        if flags:
            mask |= bits
    return mask


def header(display_name, file_name, title_id, mask):
    """The bytes of a content header file: XCONTENT_AGGREGATE_DATA (big endian fields, the
    host's padding: 0x148 bytes) and, when it has bits, the license mask (host order)."""
    name = file_name.encode('ascii')
    if len(name) > 42:
        raise ValueError('package file name longer than 42 characters: %s' % file_name)
    shown = display_name.encode('utf-16-be')[:254]
    data = (struct.pack('>II', HDD, MARKETPLACE) + shown.ljust(256, b'\0') + name.ljust(42, b'\0') +
            b'\0' * 2 + b'\0' * 4 + struct.pack('>QI', 0, title_id) + b'\0' * 4)
    assert len(data) == 0x148
    return data + (struct.pack('<I', mask) if mask else b'')


def content_root(work, title_id):
    return os.path.join(work, 'run', 'userdata', COMMON_XUID, '%08X' % title_id)


def install(work, sources, log=print):
    """Installs the game's marketplace packages among sources; returns their names. Packages of
    another title or another content type are left out (logged)."""
    work = os.path.abspath(work)
    title_id = int(xex.read(title_update.game_xex(work))['title_id'], 16)
    root = content_root(work, title_id)
    kind = '%08X' % MARKETPLACE
    installed = []
    tmp = tempfile.mkdtemp(prefix='dlc-', dir=work)
    try:
        for path in packages_in(sources, tmp, log):
            name = os.path.basename(path)
            try:
                pkg = stfs.Package(path)
            except (OSError, ValueError) as e:
                log('  %s: not a package ReXNative reads (%s): left out' % (name, e))
                continue
            staging = None
            try:
                if pkg.content_type != MARKETPLACE:
                    log('  %s: content type %08X (%s), not downloadable content: left out' % (
                        name, pkg.content_type, stfs.CONTENT_TYPES.get(pkg.content_type, '?')))
                    continue
                if pkg.title_id != title_id:
                    log('  %s: for title %08X, not %08X: left out' % (name, pkg.title_id, title_id))
                    continue
                dest = os.path.join(root, kind, name)
                staging = dest + '.installing'
                # Everything that can refuse the package before any of it is in place: its header
                # file (a name longer than 42 characters or not ASCII), its files.
                mask = license_mask(path)
                head = header(pkg.display_name or name, name, title_id, mask)
                shutil.rmtree(staging, ignore_errors=True)
                files = pkg.extract(staging)
                shutil.rmtree(dest, ignore_errors=True)
                os.replace(staging, dest)
                hdr = os.path.join(root, 'Headers', kind, name + '.header')
                os.makedirs(os.path.dirname(hdr), exist_ok=True)
                with open(hdr, 'wb') as f:
                    f.write(head)
                log('  %s: "%s", %d files, license mask %08X' % (name, pkg.display_name, files,
                                                                    mask))
                installed.append(name)
            except (OSError, ValueError) as e:
                if staging:
                    shutil.rmtree(staging, ignore_errors=True)
                log('  %s: not installed (%s)' % (name, e))
            finally:
                pkg.close()
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if not installed:
        log('  no downloadable content for %08X found' % title_id)
    return installed


def deck_items(work):
    """[(local, relative)] of the installed content for `deck`: userdata/<common>/<title>/
    00000002 and its headers, and no other user data (the Deck's own saves stay)."""
    base = os.path.join(work, 'run', 'userdata', COMMON_XUID)
    items = []
    if not os.path.isdir(base):
        return items
    kind = '%08X' % MARKETPLACE
    for title in sorted(os.listdir(base)):
        for sub in (kind, os.path.join('Headers', kind)):
            d = os.path.join(base, title, sub)
            if not os.path.isdir(d):
                continue
            for r, dirs, files in os.walk(d):
                dirs[:] = sorted(x for x in dirs if not x.endswith('.installing'))
                for f in sorted(files):
                    p = os.path.join(r, f)
                    rel = os.path.relpath(p, base).replace(os.sep, '/')
                    items.append((p, 'userdata/%s/%s' % (COMMON_XUID, rel)))
    return items


# --- The Internet Archive's collection ----------------------------------------------------------

METADATA_URL = 'https://archive.org/metadata/%s'
DOWNLOAD_URL = 'https://archive.org/download/%s/%s'
ARCHIVE_ITEM = re.compile(r'(?:https?://)?(?:www\.)?archive\.org/(?:details|download|metadata)/'
                          r'([A-Za-z0-9._-]+)')
# Words left out when names are compared: "Midnight Club: Los Angeles - Complete Edition" and
# "Deus Ex: Human Revolution - Director's Cut" are the base game's archives; "Halo 1" and
# "Overlord 1" are the first game's.
FILLER = {'the', 'complete', 'edition', 'director', 'directors', 's', 'cut', 'goty', '1'}
ROMAN = {'ii': '2', 'iii': '3', 'iv': '4', 'vi': '6', 'vii': '7', 'viii': '8', 'ix': '9'}
# A path in an archive: <title id>/<content type>/<package>.
PACKAGE_PATH = re.compile(r'(?<![0-9A-Fa-f])([0-9A-F]{8})[\\/](0000000[0-9A-F])[\\/]([0-9A-F]{8,64})')
LOGIN = ('archive.org lends this item\'s files to logged-in accounts only%s: sign in with `ia '
         'configure` (pip install internetarchive), or set IA_ACCESS_KEY_ID and '
         'IA_SECRET_ACCESS_KEY to the account\'s S3 keys (archive.org/account/s3.php)')


def words(name):
    """'Midnight Club: Los Angeles - Complete Edition' -> ['midnight', 'club', 'los', 'angeles']."""
    name = name.lower().replace('&', ' and ').replace("'", '')
    out = []
    for w in re.findall(r'[a-z0-9]+', name):
        w = ROMAN.get(w, w)
        if w not in FILLER:
            out.append(w)
    return out


def archive_game(name):
    """'Overlord.1.DLC.RF.X360-ZTM.rar' -> 'Overlord.1'."""
    return re.sub(r'\.DLC\..*$', '', name, flags=re.I)


def sources(extra=()):
    """The DLC sources: `extra`, then $REXNATIVE_DLC_SOURCES (joined by `;` or new lines). None
    come with the tool."""
    raw = list(extra or []) + re.split(r'[;\n]', os.environ.get('REXNATIVE_DLC_SOURCES', ''))
    out = []
    for s in (s.strip().strip('"') for s in raw):
        if s and s not in out:
            out.append(s)
    return out


def item_of(source):
    """An Internet Archive item's id for a source that names one; None for a folder."""
    if os.path.isdir(source):
        return None
    m = ARCHIVE_ITEM.match(source)
    if m:
        return m.group(1)
    return source if re.fullmatch(r'[A-Za-z0-9._-]+', source) else None


def catalog(source, cache={}):
    """A source's archives: [{'name', 'size' (int), 'md5', and 'item' (archive.org) or 'path'
    (a folder's file)}]. An item's metadata is public."""
    if source in cache:
        return cache[source]
    item = item_of(source)
    files = []
    if item:
        req = urllib.request.Request(METADATA_URL % urllib.parse.quote(item),
                                     headers={'User-Agent': title_update.USER_AGENT})
        with urllib.request.urlopen(req, timeout=120) as r:
            data = json.loads(r.read().decode('utf-8', 'replace'))
        # Plain file names only: a name is joined to the download folder.
        files = [{'name': f['name'], 'size': int(f.get('size') or 0),
                  'md5': f.get('md5', '').lower(), 'item': item}
                 for f in data.get('files', [])
                 if f.get('format') in ('RAR', '7z', 'ZIP') and _plain_name(f.get('name', ''))]
    elif os.path.isdir(source):
        for n in sorted(os.listdir(source)):
            p = os.path.join(source, n)
            if os.path.isfile(p) and n.lower().endswith(prepare.ARCHIVES):
                files.append({'name': n, 'size': os.path.getsize(p), 'md5': '', 'path': p})
    else:
        raise RuntimeError('%s: neither a folder nor an archive.org item' % source)
    cache[source] = files
    return files


def _plain_name(name):
    return (bool(name) and name not in ('.', '..') and not name.startswith('.') and
            not any(c in name for c in '/\\:') and os.path.basename(name) == name)


def archives_for(work, files):
    """The collection's archives for this game: by hand (<work>/dlc-archives.txt: one name a line,
    or `none`), else those whose game words are the game's title's."""
    by_name = {f['name']: f for f in files}
    hand = os.path.join(work, 'dlc-archives.txt')
    if os.path.exists(hand):
        names = [ln.split('#')[0].strip() for ln in open(hand, encoding='utf-8')]
        names = [n for n in names if n and n.lower() != 'none']
        missing = [n for n in names if n not in by_name]
        if missing:
            raise SystemExit('%s: not in the DLC sources: %s' % (hand, ', '.join(missing)))
        return [by_name[n] for n in names]
    from .deck import title_of
    want = words(title_of(work) or os.path.basename(work))
    return [f for f in files if words(archive_game(f['name'])) == want]


def credentials():
    """The headers archive.org lends the collection's files for: an account's cookies or S3 keys
    (see the module's notes); {} when there are none. Never logged."""
    cp = configparser.RawConfigParser()
    home = os.path.expanduser('~')
    xdg = os.environ.get('XDG_CONFIG_HOME') or os.path.join(home, '.config')
    for p in (os.environ.get('IA_CONFIG_FILE'), os.path.join(xdg, 'internetarchive', 'ia.ini'),
              os.path.join(home, '.config', 'internetarchive', 'ia.ini'),
              os.path.join(home, '.config', 'ia.ini'), os.path.join(home, '.ia')):
        if p and os.path.exists(p):
            try:
                cp.read(p, encoding='utf-8-sig')
            except (configparser.Error, UnicodeDecodeError) as e:
                raise NotLoggedIn('%s could not be read (%s): run `ia configure` or '
                                  'scripts\\ia-keys.ps1 again' % (p, type(e).__name__)) from None
            break
    headers = {}
    cookies = []
    for k in ('logged-in-user', 'logged-in-sig'):
        v = cp.get('cookies', k, fallback='').split(';')[0].strip()
        if v:
            cookies.append('%s=%s' % (k, v))
    if len(cookies) == 2:
        headers['Cookie'] = '; '.join(cookies)
    access = os.environ.get('IA_ACCESS_KEY_ID') or cp.get('s3', 'access', fallback='')
    secret = os.environ.get('IA_SECRET_ACCESS_KEY') or cp.get('s3', 'secret', fallback='')
    if access and secret:
        headers['Authorization'] = 'LOW %s:%s' % (access, secret)
    return headers


class NotLoggedIn(SystemExit):
    pass


def _archive_https(url):
    p = urllib.parse.urlsplit(url)
    host = (p.hostname or '').lower()
    return p.scheme == 'https' and (host == 'archive.org' or host.endswith('.archive.org'))


class _Redirects(urllib.request.HTTPRedirectHandler):
    """Redirects that keep the account's headers only to archive.org over HTTPS (urllib
    forwards every header to wherever a redirect points)."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        new = super().redirect_request(req, fp, code, msg, headers, newurl)
        if new is not None and not _archive_https(new.full_url):
            for h in ('Authorization', 'Cookie'):
                new.remove_header(h)
        return new


_opener = urllib.request.build_opener(_Redirects)


def _open(url, auth, offset=0, end=None, timeout=120):
    headers = dict(auth if _archive_https(url) else {}, **{'User-Agent': title_update.USER_AGENT})
    if offset or end is not None:
        headers['Range'] = 'bytes=%d-%s' % (offset, '' if end is None else end)
    try:
        return _opener.open(urllib.request.Request(url, headers=headers), timeout=timeout)
    except urllib.error.HTTPError as e:
        if e.code in (401, 403):
            raise NotLoggedIn(LOGIN % (' (HTTP %d)' % e.code)) from None
        raise


def archive_packages(entry, auth):
    """The (title id, content type, package) paths an archive.org archive holds, from archive.org's
    listing of it; else from its first 64 KB (each file's header comes before its data); [] when
    neither names one."""
    found = []
    url = DOWNLOAD_URL % (urllib.parse.quote(entry['item']), urllib.parse.quote(entry['name']))
    try:
        with _open(url + '/', auth) as r:
            found = PACKAGE_PATH.findall(urllib.parse.unquote(r.read().decode('utf-8', 'replace')))
    except NotLoggedIn:
        raise
    except (OSError, ValueError, http.client.HTTPException):
        pass
    if not found:
        try:
            with _open(url, auth, 0, 65535) as r:
                found = PACKAGE_PATH.findall(r.read(65536).decode('latin-1'))
        except NotLoggedIn:
            raise
        except (OSError, ValueError, http.client.HTTPException):
            pass
    return sorted(set(found))


def download(entry, folder, auth, log=print):
    """The archive in folder (a part a stopped run left goes on from where it stopped), checked
    against the item's size and MD5. A connection that stops early keeps the part for the next
    run (RuntimeError)."""
    if not _plain_name(entry['name']):
        raise RuntimeError('%r: not a plain file name' % entry['name'])
    os.makedirs(folder, exist_ok=True)
    path = os.path.join(folder, entry['name'])
    part = path + '.part'
    url = DOWNLOAD_URL % (urllib.parse.quote(entry['item']), urllib.parse.quote(entry['name']))
    size = entry['size']
    parts = CONNECTIONS > 1 and size >= 2 * CHUNK
    if parts and os.path.exists(part + '.json') and not os.path.exists(path):
        # A run over several connections stopped: its part is the whole size, with holes.
        try:
            return _download_parts(entry, url, path, part, auth, log)
        except _NoRanges:
            parts = _no_parts(part, log)
    md5 = hashlib.md5()
    done = 0
    if os.path.exists(path) and not os.path.exists(part):
        os.replace(path, part)  # an archive a run downloaded but did not install: checked again
    if os.path.exists(part):
        with open(part, 'rb') as f:
            for chunk in iter(lambda: f.read(1 << 20), b''):
                md5.update(chunk)
                done += len(chunk)
    if done > size:
        os.remove(part)
        md5, done = hashlib.md5(), 0
    if parts and done < size:
        try:
            return _download_parts(entry, url, path, part, auth, log)
        except _NoRanges:
            _no_parts(part, log)
            md5, done = hashlib.md5(), 0
    if done < size:
        log('  downloading %s (%.1f MB%s)' % (entry['name'], size / 1e6,
                                             ', from %.1f MB' % (done / 1e6) if done else ''))
        t0, start, shown = time.time(), done, done
        with _open(url, auth, done) as r:
            if done and r.status != 206:  # the server sends it whole
                md5, done, start, shown = hashlib.md5(), 0, 0, 0
            with open(part, 'ab' if done else 'wb') as f:
                try:
                    for chunk in iter(lambda: r.read(1 << 20), b''):
                        f.write(chunk)
                        md5.update(chunk)
                        done += len(chunk)
                        if done - shown >= max(size // 10, 50 << 20):
                            shown = done
                            log('    %d%% (%.1f MB/s)' % (
                                done * 100 // max(size, 1),
                                (done - start) / 1e6 / max(time.time() - t0, 1e-3)))
                except (OSError, http.client.HTTPException) as e:
                    raise RuntimeError('%s: the connection stopped at %.1f of %.1f MB (%s); '
                                       'run again to go on' % (entry['name'], done / 1e6,
                                                               size / 1e6, e)) from None
    if done != size:
        raise RuntimeError('%s: %.1f of %.1f MB came; run again to go on from there' % (
            entry['name'], done / 1e6, size / 1e6))
    if entry['md5'] and md5.hexdigest() != entry['md5']:
        os.remove(part)
        raise RuntimeError('%s: MD5 %s, the item lists %s (removed)' % (
            entry['name'], md5.hexdigest(), entry['md5']))
    os.replace(part, path)
    return path


# An archive.org file comes over several connections at once, each asking for its own part (a
# Range request): one connection gave a 2.3 GB DLC archive 0.6 MB/s. $REXNATIVE_DLC_CONNECTIONS
# sets how many (1: one stream, as before).
CONNECTIONS = max(1, int(os.environ.get('REXNATIVE_DLC_CONNECTIONS') or 4))
CHUNK = 32 << 20


class _NoRanges(Exception):
    """The server answered a part's request with the whole file."""


def _no_parts(part, log):
    log('  the server sends no parts: one connection')
    for p in (part, part + '.json'):
        if os.path.exists(p):
            os.remove(p)
    return False


def _download_parts(entry, url, path, part, auth, log):
    """The archive in parts over CONNECTIONS connections, each part written where it goes in
    part; the parts done are kept in part.json after each one, so a stopped run goes on with the
    rest (a single stream's start counts as its whole parts). Checked against the item's MD5 at
    the end, as one stream is."""
    size = entry['size']
    side = part + '.json'
    n = (size + CHUNK - 1) // CHUNK
    done = set()
    try:
        with open(side, encoding='utf-8') as fh:
            s = json.load(fh)
        if s.get('size') == size and s.get('chunk') == CHUNK and os.path.exists(part):
            done = {int(k) for k in s.get('done') or []}
    except (OSError, ValueError):
        if os.path.exists(part):  # a single stream's start
            done = set(range(min(n, os.path.getsize(part) // CHUNK)))
    todo = [k for k in range(n) if k not in done]
    lock = threading.Lock()
    got = [sum(min(CHUNK, size - k * CHUNK) for k in done)]
    first, tries, errors = got[0], {}, []
    if todo:  # the line the Manager's progress reads, as one stream's
        log('  downloading %s (%.1f MB)' % (entry['name'], size / 1e6))
        log('    over %d connections%s' % (CONNECTIONS, ', from %.1f MB' % (first / 1e6) if first else ''))
    with open(part, 'r+b' if os.path.exists(part) else 'w+b') as f:
        if os.path.getsize(part) < size:
            f.truncate(size)

    def save():
        tmp = side + '.tmp'
        with open(tmp, 'w', encoding='utf-8') as fh:
            json.dump({'size': size, 'chunk': CHUNK, 'done': sorted(done)}, fh)
        os.replace(tmp, side)

    def worker():
        with open(part, 'r+b') as f:  # a handle of its own: the parts do not overlap
            while True:
                with lock:
                    if errors or not todo:
                        return
                    k = todo.pop(0)
                a, b = k * CHUNK, min(size, (k + 1) * CHUNK) - 1
                pos = a
                try:
                    with _open(url, auth, a, b) as r:
                        if getattr(r, 'status', 206) != 206:
                            raise _NoRanges()
                        f.seek(a)
                        while pos <= b:
                            buf = r.read(min(1 << 20, b + 1 - pos))
                            if not buf:
                                break
                            f.write(buf)
                            pos += len(buf)
                            with lock:
                                got[0] += len(buf)
                    if pos <= b:
                        raise OSError('the connection ended %d KB into the part' % ((pos - a) // 1024))
                    f.flush()
                    with lock:
                        done.add(k)
                        save()
                except (_NoRanges, NotLoggedIn) as x:
                    with lock:
                        errors.append(x)
                    return
                except (OSError, http.client.HTTPException) as x:
                    with lock:
                        got[0] -= pos - a
                        tries[k] = tries.get(k, 0) + 1
                        if tries[k] >= 3:
                            errors.append(RuntimeError(
                                '%s: a part failed 3 times (%s); run again to go on from the %.1f MB '
                                'that came' % (entry['name'], x, got[0] / 1e6)))
                            return
                        todo.append(k)  # asked again later

    t0, shown = time.time(), first
    threads = [threading.Thread(target=worker, daemon=True) for _ in range(min(CONNECTIONS, len(todo)))]
    for t in threads:
        t.start()
    while any(t.is_alive() for t in threads):
        time.sleep(1)
        with lock:
            now = got[0]
        if now - shown >= max(size // 10, 50 << 20):
            shown = now
            log('    %d%% (%.1f MB/s)' % (now * 100 // max(size, 1),
                                         (now - first) / 1e6 / max(time.time() - t0, 1e-3)))
    if errors:
        raise errors[0]
    if len(done) != n:
        raise RuntimeError('%s: %d of %d parts came; run again to go on' % (entry['name'], len(done), n))
    md5 = hashlib.md5()
    with open(part, 'rb') as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b''):
            md5.update(chunk)
    os.remove(side)
    if entry['md5'] and md5.hexdigest() != entry['md5']:
        os.remove(part)
        raise RuntimeError('%s: MD5 %s, the item lists %s (removed)' % (
            entry['name'], md5.hexdigest(), entry['md5']))
    os.replace(part, path)
    return path


def _record(work):
    p = os.path.join(work, 'dlc.json')
    return json.load(open(p, encoding='utf-8')) if os.path.exists(p) else {}


def _save_record(work, rec):
    with open(os.path.join(work, 'dlc.json'), 'w', encoding='utf-8', newline='\n') as f:
        json.dump(rec, f, indent=1, sort_keys=True)


def fetch(work, log=print, list_only=False, keep=False, extra_sources=()):
    """Fetches the game's archives from the DLC sources (sources()) and installs their packages;
    returns {archive: {'md5', 'titles', 'installed'}}. list_only: says what it would fetch,
    downloads nothing. keep: the downloaded archives stay in <work>/dlc-download. Without a
    source it does nothing."""
    work = os.path.abspath(work)
    srcs = sources(extra_sources)
    if not srcs:
        log('  no DLC source set ($REXNATIVE_DLC_SOURCES or --dlc-source): skipped')
        return {}
    title_id = int(xex.read(title_update.game_xex(work))['title_id'], 16)
    tid, kind = '%08X' % title_id, '%08X' % MARKETPLACE
    files = []
    for s in srcs:
        try:
            files += catalog(s)
        except (OSError, ValueError, RuntimeError, http.client.HTTPException) as x:
            log('  DLC source %s: %s' % (s, x))
    entries = archives_for(work, files)
    if not entries:
        log('  no archive in the DLC sources for %s (%s); <work>/dlc-archives.txt names them by '
            'hand' % (os.path.basename(work), tid))
        return {}
    online = any('item' in e for e in entries)
    auth = {} if list_only or not online else credentials()
    if online and not list_only and not auth:
        raise NotLoggedIn(LOGIN % '')
    try:
        rec = _record(work)
    except ValueError:
        log('  dlc.json could not be read: every archive is looked at again')
        rec = {}
    root = content_root(work, title_id)
    out, failed = {}, []
    for e in entries:
        have = rec.get(e['name'])
        # A folder's archive has no MD5 listed: its size stands in for it.
        stamp = e['md5'] or 'size:%d' % e['size']
        if have and have.get('md5') == stamp:
            log('  %s: installed before (%d packages)' % (e['name'], len(have.get('installed', []))))
            out[e['name']] = have
            continue
        if list_only:
            log('  %s: %.1f MB, to fetch' % (e['name'], e['size'] / 1e6))
            continue
        try:
            names, titles = _fetch_one(work, e, auth, root, tid, kind, keep, log)
        except NotLoggedIn:
            raise
        except (OSError, ValueError, RuntimeError, http.client.HTTPException) as x:
            log('  %s: FAILED: %s' % (e['name'], x))
            failed.append(e['name'])
            continue
        rec[e['name']] = {'md5': stamp, 'titles': titles, 'installed': sorted(names)}
        _save_record(work, rec)
        out[e['name']] = rec[e['name']]
    if failed:
        raise RuntimeError('not fetched: %s (installed: %d of %d archives)' % (
            ', '.join(failed), len(out), len(entries)))
    return out


def _fetch_one(work, e, auth, root, tid, kind, keep, log):
    """One archive: (the packages it installed, the titles its listing names)."""
    if 'path' in e:
        # A folder's archive: installed from where it is (packages of another title left out)
        # and left there.
        return install(work, [e['path']], log), []
    paths = archive_packages(e, auth)
    titles = sorted({p[0] for p in paths})
    mine = sorted(p[2] for p in paths if p[0] == tid and p[1] == kind)
    if titles and tid not in titles:
        log('  %s: packages of %s only, not %s: left out' % (e['name'], ', '.join(titles), tid))
        names = []
    elif mine and all(os.path.isdir(os.path.join(root, kind, n)) and
                      os.path.exists(os.path.join(root, 'Headers', kind, n + '.header'))
                      for n in mine):
        log('  %s: its %d packages are installed already' % (e['name'], len(mine)))
        names = mine
    else:
        folder = os.path.join(work, 'dlc-download')
        path = download(e, folder, auth, log)
        names = install(work, [path], log)  # on a failure the archive stays for the next run
        if not keep:
            os.remove(path)
            if not os.listdir(folder):
                os.rmdir(folder)
    return names, titles
