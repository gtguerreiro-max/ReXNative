"""Files out of an Xbox 360 STFS package (CON / LIVE / PIRS: Xbox Live Arcade titles, title
updates, DLC). From FromDustRecomp's tools/stfs_extract.py, with the package's file names
checked: a package is untrusted data and must not write outside the output directory.
"""
import hashlib
import os
import struct

BLOCK = 0x1000
PER_LEVEL = 170
MAGICS = (b'CON ', b'LIVE', b'PIRS')
CONTENT_TYPES = {0x000D0000: 'Xbox Live Arcade title', 0x00000002: 'marketplace content (DLC)',
                 0x000B0000: 'title update', 0x00007000: 'Games on Demand',
                 0x00080000: 'demo', 0x00000001: 'saved game', 0x00009000: 'avatar item',
                 0x00020000: 'gamer picture', 0x00030000: 'theme'}


def u24le(b, o):
    return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16)


def u24be(b, o):
    return (b[o] << 16) | (b[o + 1] << 8) | b[o + 2]


def is_package(path):
    try:
        with open(path, 'rb') as f:
            return f.read(4) in MAGICS
    except OSError:
        return False


class Package:
    def __init__(self, path):
        self.f = open(path, 'rb')
        hdr = self.f.read(0x971A)
        self.magic = hdr[:4]
        if self.magic not in MAGICS:
            raise ValueError('not an STFS package: %r' % self.magic)
        self.header_size = struct.unpack('>I', hdr[0x340:0x344])[0]
        self.content_type = struct.unpack('>I', hdr[0x344:0x348])[0]
        self.title_id = struct.unpack('>I', hdr[0x360:0x364])[0]
        if struct.unpack('>I', hdr[0x3A9:0x3AD])[0] != 0:
            raise ValueError('SVOD package (Games on Demand), not STFS')
        vd = hdr[0x379:0x379 + 0x24]
        self.read_only = vd[2] & 1
        self.bpht = 1 if self.read_only else 2
        self.ft_blocks = struct.unpack('<H', vd[3:5])[0]
        self.ft_start = u24le(vd, 5)
        self.data_base = (self.header_size + BLOCK - 1) & ~(BLOCK - 1)
        self.display_name = hdr[0x411:0x411 + 0x80].decode('utf-16-be', 'ignore').split('\0')[0]

    def close(self):
        self.f.close()

    def backing_block(self, idx):
        base, block = PER_LEVEL, idx
        for _ in range(3):
            block += ((idx + base) // base) * self.bpht
            if idx < base:
                break
            base *= PER_LEVEL
        return block

    def hash_block(self, idx):
        if idx < PER_LEVEL:
            return 0
        block = (idx // PER_LEVEL) * (PER_LEVEL + self.bpht)
        block += ((idx // (PER_LEVEL * PER_LEVEL)) + 1) * self.bpht
        if idx < PER_LEVEL * PER_LEVEL:
            return block
        return block + self.bpht

    def hash_table_block(self, idx, level):
        """The block (from the data base) of the level-`level` hash table covering data block
        `idx` (Xenia's StfsContainerDevice::BlockToHashBlockNumberSTFS)."""
        step0 = PER_LEVEL + self.bpht
        step1 = PER_LEVEL * PER_LEVEL + (PER_LEVEL + 1) * self.bpht
        if level == 0:
            return self.hash_block(idx)
        if level == 1:
            if idx < PER_LEVEL * PER_LEVEL:
                return step0
            return (idx // (PER_LEVEL * PER_LEVEL)) * step1 + self.bpht
        return step1

    def verify(self):
        """Problems found checking the package against itself (an empty list: whole): the
        content id (the header's digest at 0x32C) is the SHA-1 of the header from 0x344 to its
        end at a 4 KB boundary, which holds the top hash table's hash (volume descriptor); each
        data block's SHA-1 is in its level-0 hash table, each table's in the level above, the
        top's in the header. A read-only (distribution) package keeps one copy of each table."""
        problems = []
        self.f.seek(0)
        hdr = self.f.read(self.data_base)
        content_id = hdr[0x32C:0x340]
        if hashlib.sha1(hdr[0x344:self.data_base]).digest() != content_id:
            problems.append('header: its SHA-1 is not the content id')
        vd = hdr[0x379:0x379 + 0x24]
        top_hash = vd[8:0x1C]
        allocated = struct.unpack('>I', vd[0x1C:0x20])[0]
        if not self.read_only:
            problems.append('not a read-only package: its hash tables are not checked')
            return problems

        def table(block):
            self.f.seek(self.data_base + block * BLOCK)
            return self.f.read(BLOCK)

        def entry(tbl, i):
            return tbl[i * 0x18:i * 0x18 + 0x14]

        cache = {}

        def cached(block):
            if block not in cache:
                cache[block] = table(block)
            return cache[block]

        for idx in range(allocated):
            data = self.read_block(idx)
            if hashlib.sha1(data).digest() != entry(cached(self.hash_table_block(idx, 0)), idx % PER_LEVEL):
                problems.append('data block %d: hash mismatch' % idx)
                if len(problems) > 8:
                    return problems
        top_level = 0 if allocated <= PER_LEVEL else 1 if allocated <= PER_LEVEL * PER_LEVEL else 2
        for level in range(1, top_level + 1):
            span = PER_LEVEL ** level  # data blocks one table of the level below covers
            for first in range(0, allocated, span):
                below = cached(self.hash_table_block(first, level - 1))
                above = cached(self.hash_table_block(first, level))
                if hashlib.sha1(below).digest() != entry(above, (first // span) % PER_LEVEL):
                    problems.append('level-%d hash table for block %d: hash mismatch' % (
                        level - 1, first))
        if allocated and hashlib.sha1(cached(self.hash_table_block(0, top_level))).digest() != top_hash:
            problems.append('top hash table: not the header\'s hash')
        return problems

    def content_id(self):
        self.f.seek(0x32C)
        return self.f.read(0x14).hex().upper()

    def read_block(self, idx):
        self.f.seek(self.data_base + self.backing_block(idx) * BLOCK)
        return self.f.read(BLOCK)

    def next_block(self, idx):
        # The primary hash table only: fine for read-only (distribution) packages.
        self.f.seek(self.data_base + self.hash_block(idx) * BLOCK + (idx % PER_LEVEL) * 0x18)
        return u24be(self.f.read(0x18), 0x15)

    def chain(self, start, count, contiguous):
        idx = start
        for _ in range(count):
            yield idx
            idx = idx + 1 if contiguous else self.next_block(idx)

    def entries(self):
        raw = b''.join(self.read_block(b) for b in self.chain(self.ft_start, self.ft_blocks, False))
        out = []
        for i in range(0, len(raw), 0x40):
            e = raw[i:i + 0x40]
            nlen = e[0x28] & 0x3F
            if nlen == 0:
                break
            out.append({
                'name': e[:nlen].decode('ascii', 'replace'),
                'dir': bool(e[0x28] & 0x80),
                'contig': bool(e[0x28] & 0x40),
                'blocks': u24le(e, 0x29),
                'start': u24le(e, 0x2F),
                'parent': struct.unpack('>h', e[0x32:0x34])[0],
                'size': struct.unpack('>I', e[0x34:0x38])[0],
            })
        return out

    def path_of(self, entries, i):
        parts, seen = [], set()
        while i != -1 and 0 <= i < len(entries) and i not in seen:
            seen.add(i)
            parts.append(entries[i]['name'])
            i = entries[i]['parent']
        return '/'.join(reversed(parts))

    def extract(self, out_dir, log=None):
        """Every file into out_dir; returns the number of files. Names that would leave out_dir
        (absolute, '..', drive letters) are refused."""
        root = os.path.realpath(out_dir)
        entries = self.entries()
        n = 0
        for i, e in enumerate(entries):
            rel = self.path_of(entries, i)
            parts = [p for p in rel.replace('\\', '/').split('/') if p not in ('', '.')]
            if any(p == '..' or ':' in p for p in parts) or not parts:
                raise ValueError('unsafe file name in package: %r' % rel)
            dst = os.path.realpath(os.path.join(root, *parts))
            if not (dst == root or dst.startswith(root + os.sep)):
                raise ValueError('file name leaves the output directory: %r' % rel)
            if e['dir']:
                os.makedirs(dst, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            remaining = e['size']
            with open(dst, 'wb') as o:
                for b in self.chain(e['start'], e['blocks'], e['contig']):
                    data = self.read_block(b)
                    o.write(data[:min(BLOCK, remaining)])
                    remaining -= BLOCK
                    if remaining <= 0:
                        break
            n += 1
            if log:
                log('  %12d  %s' % (e['size'], '/'.join(parts)))
        return n
