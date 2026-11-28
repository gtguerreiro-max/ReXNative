"""Xbox 360 disc images (.iso): the game's files in its XDVDFS partition.

A disc game (not an Xbox Live Arcade package) comes as an image of the disc. Its files are in an
XDVDFS ("Xbox DVD file system") partition: a volume descriptor in sector 32 of the partition,
with the magic MICROSOFT*XBOX*MEDIA at its start and again at +0x7EC, the root directory's
sector and size after the first. A directory is a binary tree of entries, each 4-byte aligned:
left and right subtree offsets (in 4-byte units from the directory's start, 0 for none),
the file's first sector and size, its attributes (0x10 a directory) and its name. Sectors are
2048 bytes from the partition's start.

Where the partition starts depends on how the image was made: 0 for an image of the game
partition alone (extract-xiso, "XISO"), or past the video partition in a whole-disc image: XGD2
0xFD90000, XGD3 0x2080000, XGD1 (original Xbox) 0x18300000. The format as extract-xiso and
Xenia's disc_image_device.cc read it.
"""
import os
import struct

from . import meter

SECTOR = 2048
MAGIC = b'MICROSOFT*XBOX*MEDIA'
PARTITIONS = (0, 0xFD90000, 0x2080000, 0x18300000)
DIRECTORY = 0x10


def is_image(path):
    return os.path.isfile(path) and partition(path) is not None


def partition(path):
    """The game partition's offset in the image, or None when it is not an Xbox disc image."""
    try:
        with open(path, 'rb') as f:
            for base in PARTITIONS:
                f.seek(base + 32 * SECTOR)
                if f.read(len(MAGIC)) == MAGIC:
                    return base
    except OSError:
        return None
    return None


class Image:
    def __init__(self, path):
        self.path = path
        self.base = partition(path)
        if self.base is None:
            raise ValueError('%s: no XDVDFS volume (not an Xbox disc image)' % path)
        self.f = open(path, 'rb')
        self.f.seek(self.base + 32 * SECTOR + len(MAGIC))
        self.root_sector, self.root_size = struct.unpack('<II', self.f.read(8))

    def close(self):
        self.f.close()

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    def _read(self, sector, size):
        self.f.seek(self.base + sector * SECTOR)
        return self.f.read(size)

    def entries(self, sector, size):
        """(name, first sector, size, is a directory) of a directory, in its tree's order."""
        if not size:
            return []
        data = self._read(sector, size)
        out, stack, seen = [], [0], set()
        while stack:
            off = stack.pop()
            p = off * 4
            if off in seen or p + 14 > len(data):
                continue
            seen.add(off)
            left, right, first, length, attr, n = struct.unpack_from('<HHIIBB', data, p)
            if left == 0xFFFF:  # the padding at a sector's end
                continue
            name = data[p + 14:p + 14 + n].decode('latin-1')
            out.append((name, first, length, bool(attr & DIRECTORY)))
            if right:
                stack.append(right)
            if left:
                stack.append(left)
        return out

    def walk(self, sector=None, size=None, prefix=''):
        """(relative path, first sector, size) of every file under a directory (the root)."""
        if sector is None:
            sector, size = self.root_sector, self.root_size
        for name, first, length, is_dir in self.entries(sector, size):
            rel = prefix + name
            if is_dir:
                yield from self.walk(first, length, rel + '/')
            else:
                yield rel, first, length

    def extract(self, dst, log=print, chunk=8 << 20, skip=('$SystemUpdate/',)):
        """Writes every file under dst but those under `skip` (the console's system update,
        which discs carry for the console, not the game); returns how many. The bytes written
        against the files' total (the directory's sizes) go to log as meter lines."""
        files = [f for f in self.walk() if not f[0].startswith(skip)]
        m = meter.Meter(sum(f[2] for f in files), log)
        n = total = 0
        for rel, first, length in files:
            out = os.path.join(dst, *rel.split('/'))
            os.makedirs(os.path.dirname(out), exist_ok=True)
            self.f.seek(self.base + first * SECTOR)
            left = length
            with open(out, 'wb') as w:
                while left:
                    b = self.f.read(min(chunk, left))
                    if not b:
                        raise ValueError('%s: %s ends before its %d bytes' % (self.path, rel, length))
                    w.write(b)
                    left -= len(b)
                    m.bytes(total + length - left)
            n += 1
            total += length
        log('  %d files, %.1f GB' % (n, total / 1e9))
        return n
