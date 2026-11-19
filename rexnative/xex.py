"""XEX2 header facts the adapter needs: title id, version and the XDK static libraries.

The static library list (optional header 0x000200FF) is in the clear XEX header even when the
image is encrypted and compressed, so this reads only the header. From FromDustRecomp's
tools/xex_info.py.
"""
import struct

STATIC_LIBRARIES = 0x000200FF
EXECUTION_INFO = 0x00040006
ORIGINAL_PE_NAME = 0x000183FF


def _be32(b, o):
    return struct.unpack('>I', b[o:o + 4])[0]


def read(path):
    """{'title_id', 'version', 'pe_name', 'libs': {name: 'a.b.c.d'}, 'flags'} of a .xex/.xexp."""
    b = open(path, 'rb').read(1 << 20)
    if b[:4] != b'XEX2':
        raise ValueError('%s: not an XEX2 file' % path)
    flags, _, _, _, count = struct.unpack('>IIIII', b[4:0x18])
    out = {'path': path, 'flags': flags, 'libs': {}, 'title_id': None, 'version': None,
           'pe_name': None, 'patch': bool(flags & 0x10), 'media_id': None,
           'base_version': None}
    for i in range(count):
        key, val = struct.unpack('>II', b[0x18 + i * 8:0x20 + i * 8])
        if key == STATIC_LIBRARIES:
            size = _be32(b, val)
            for o in range(val + 4, val + size, 16):
                name = b[o:o + 8].split(b'\0')[0].decode('ascii', 'replace')
                ma, mi, bu, qfe = struct.unpack('>HHHH', b[o + 8:o + 16])
                out['libs'][name] = '%d.%d.%d.%d' % (ma, mi, bu, qfe & 0xFF)
        elif key == EXECUTION_INFO:
            # media id, version, base version, title id (a title update's version is the one it
            # brings the game to, its base version the one it applies to).
            mid, ver, base_ver, tid = struct.unpack('>IIII', b[val:val + 16])
            out['title_id'], out['version'] = '%08X' % tid, '%08X' % ver
            out['media_id'], out['base_version'] = '%08X' % mid, '%08X' % base_ver
        elif key == ORIGINAL_PE_NAME:
            size = _be32(b, val)
            out['pe_name'] = b[val + 4:val + size].split(b'\0')[0].decode('ascii', 'replace')
    return out


FILE_FORMAT_INFO = 0x000003FF
IMAGE_BASE_ADDRESS = 0x00010201


def image_span(path):
    """(base address, bytes the loader allocates) of an XEX, from its clear header: the image size
    of its security info, or for an uncompressed one the file's data after the header if that is
    larger (ReXGlue's XEXModule::Load, as Xenia's, allocates that much)."""
    b = open(path, 'rb').read(1 << 20)
    if b[:4] != b'XEX2':
        raise ValueError('%s: not an XEX2 file' % path)
    _, header_size, _, security, count = struct.unpack('>IIIII', b[4:0x18])
    image_size = _be32(b, security + 4)
    base = _be32(b, security + 0x110)
    compression = None
    for i in range(count):
        key, val = struct.unpack('>II', b[0x18 + i * 8:0x20 + i * 8])
        if key == IMAGE_BASE_ADDRESS:
            base = val
        elif key == FILE_FORMAT_INFO:
            compression = struct.unpack('>H', b[val + 6:val + 8])[0]
    alloc = image_size
    if compression == 0:
        import os
        alloc = max(alloc, os.path.getsize(path) - header_size)
    return base, alloc


def spa_title(image):
    """The title's name in its SPA (the XDBF resource every title links): the English string
    table's string 0x8000, from a dump of the loaded image (run/image.bin, which `project` writes
    with --rexnative_dump_image), so from the decrypted XEX. None when there is none."""
    try:
        b = open(image, 'rb').read()
    except OSError:
        return None
    i = b.find(b'XDBF')
    while i >= 0:
        _, table, count, free, _ = struct.unpack('>IIIII', b[i + 4:i + 24])
        if count < 4096 and table < 65536:
            entries, data = i + 24, i + 24 + table * 18 + free * 8
            for k in range(min(count, table)):
                ns, eid, off, _ = struct.unpack('>HQII', b[entries + 18 * k:entries + 18 * k + 18])
                s = data + off
                if ns != 3 or eid != 1 or b[s:s + 4] != b'XSTR':  # strings, English
                    continue
                o = s + 14
                for _ in range(struct.unpack('>H', b[s + 12:s + 14])[0]):
                    sid, n = struct.unpack('>HH', b[o:o + 4])
                    if sid == 0x8000:
                        return b[o + 4:o + 4 + n].decode('utf-8', 'replace')
                    o += 4 + n
        i = b.find(b'XDBF', i + 4)
    return None


def d3d_library(info):
    """('D3D9' or 'D3D9LTCG', version) from the static libraries, or (None, None)."""
    for name in ('D3D9LTCG', 'D3D9'):
        if name in info['libs']:
            return name, info['libs'][name]
    return None, None


def xdk_build(version):
    """2.0.20353.0 -> 20353."""
    try:
        return int(version.split('.')[2])
    except (AttributeError, IndexError, ValueError):
        return None
