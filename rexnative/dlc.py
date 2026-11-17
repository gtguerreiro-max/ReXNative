"""Downloadable content (marketplace packages, content type 00000002) for a game `prepare` made.

Each STFS package is extracted where ReXGlue's content manager lists it for every profile,
run/userdata/0000000000000000/<title id>/00000002/<the package's file name>/, with the header file
ContentManager::InstallContent writes beside it (Headers/00000002/<name>.header: the
XCONTENT_AGGREGATE_DATA and the package's license mask), so the game finds it as installed
content. `rexnative deck` sends it along (userdata/ beside the game on the Deck).

  python -m rexnative dlc <work>/<game> <package | folder | .rar/.7z/.zip>...
"""
import os
import shutil
import struct
import tempfile

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
            pkg = stfs.Package(path)
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
                shutil.rmtree(staging, ignore_errors=True)
                files = pkg.extract(staging)
                shutil.rmtree(dest, ignore_errors=True)
                os.replace(staging, dest)
                mask = license_mask(path)
                hdr = os.path.join(root, 'Headers', kind, name + '.header')
                os.makedirs(os.path.dirname(hdr), exist_ok=True)
                with open(hdr, 'wb') as f:
                    f.write(header(pkg.display_name or name, name, title_id, mask))
                log('  %s: "%s", %d files, license mask %08X' % (name, pkg.display_name, files,
                                                                    mask))
                installed.append(name)
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
