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
           'pe_name': None, 'patch': bool(flags & 0x10)}
    for i in range(count):
        key, val = struct.unpack('>II', b[0x18 + i * 8:0x20 + i * 8])
        if key == STATIC_LIBRARIES:
            size = _be32(b, val)
            for o in range(val + 4, val + size, 16):
                name = b[o:o + 8].split(b'\0')[0].decode('ascii', 'replace')
                ma, mi, bu, qfe = struct.unpack('>HHHH', b[o + 8:o + 16])
                out['libs'][name] = '%d.%d.%d.%d' % (ma, mi, bu, qfe & 0xFF)
        elif key == EXECUTION_INFO:
            _, ver, _, tid = struct.unpack('>IIII', b[val:val + 16])
            out['title_id'], out['version'] = '%08X' % tid, '%08X' % ver
        elif key == ORIGINAL_PE_NAME:
            size = _be32(b, val)
            out['pe_name'] = b[val + 4:val + size].split(b'\0')[0].decode('ascii', 'replace')
    return out


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
