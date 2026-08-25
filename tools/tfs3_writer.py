#!/usr/bin/env python3
"""tools/tfs3_writer.py -- host-side TFS3 v1 read/write tool.

The host-side TFS3 tool: gets files onto (or off of)
a TFS3 disk image without booting toy-os, and is the reference
implementation the kernel backend (kernel/fs/tfs3.c) is tested
against. Format spec: docs/tfs3-design.md (block groups, 128-byte
checksummed inodes, dirent blocks, epoch timestamps, superblock
backups, everything volume-relative).

Commands:
  format <img> [--size N] [--bytes-per-inode N] [--force] [--dry-run]
  info   <img>                     superblock + group summary
  ls     <img> [path]              directory listing (default /)
  read   <img> <src> [dst]         copy a file out (stdout if no dst)
  write  <img> <src> <dst>         copy a host file in
  mkdir  <img> <path>
  delete <img> <path>              refuses non-empty dirs, like the OS
  sync   <img> <seed-dir>          mirror a seed tree (once/ + sync/)
  trim   <img>                     punch holes through free blocks

Write scope is direct + single-indirect blocks per file (12 + 1024
blocks = ~4.05 MB), a deliberate cap -- see
docs/decisions.md for why; the seeding path never needs more.

Bit order in bitmaps: bit i of a group's bitmap is byte[i >> 3],
mask (1 << (i & 7)) -- LSB first, and the kernel matches this.

TIMESTAMPS: the kernel stores LOCAL-derived epochs (tz_rtc_to_epoch()
over rtc_read_local() -- see fs.h's fs_stat_info comment), so this
tool writes calendar.timegm(time.localtime()): the same "local civil
time read as if it were UTC" reckoning, keeping host-written and
kernel-written timestamps comparable.
"""

import argparse
import calendar
import hashlib
import os
import struct
import sys
import time

SECTOR = 512
BLOCK = 4096
SPB = BLOCK // SECTOR  # sectors per block

MAGIC = b"TFS3"
VERSION = 2          # what `format` writes
VERSION_MIN = 1      # oldest version this tool reads/edits

SB_BLOCK = 8
JOURNAL_HEADER_BLOCK = 9
GDT_BLOCKS = 16                       # fixed -- see the spec's rationale
GDT_CAPACITY = GDT_BLOCKS * BLOCK // 16
BLOCKS_PER_GROUP = 32768               # one 4 KiB bitmap's worth
# The last group may be shorter than that; this is the floor. Must match
# the kernel's T3_MIN_GROUP_BLOCKS -- the two format the same volumes.
MIN_GROUP_BLOCKS = 512

# Two complete geometries, one per format version -- the journal is a
# fixed region between the superblock and the group descriptors, so
# making it bigger moves everything after it. v1 had four slots, which
# cannot hold the five-block transaction a cross-directory directory
# move needs; v2 has 32. Keep in lockstep with kernel/fs/tfs3.c's
# T3_V1_*/T3_V2_* sets -- and note each version's numbers are
# CONSTANTS, which is what lets a reader with an unreadable primary
# superblock still find the backups by trying both.
GEOMETRY = {
    1: dict(jdata=10, jslots=4,  gdt=14, group0=30),
    2: dict(jdata=10, jslots=32, gdt=42, group0=58),
}

# The active geometry, set by set_geometry() before any structure whose
# position depends on it is touched. Mirrors the kernel's globals.
JOURNAL_DATA_BLOCK = GEOMETRY[1]["jdata"]
JOURNAL_SLOTS = GEOMETRY[1]["jslots"]
GDT_BLOCK = GEOMETRY[1]["gdt"]
GROUP0_START = GEOMETRY[1]["group0"]
FS_VERSION = 1


def set_geometry(version):
    global JOURNAL_DATA_BLOCK, JOURNAL_SLOTS, GDT_BLOCK, GROUP0_START, FS_VERSION
    g = GEOMETRY[version]
    JOURNAL_DATA_BLOCK, JOURNAL_SLOTS = g["jdata"], g["jslots"]
    GDT_BLOCK, GROUP0_START = g["gdt"], g["group0"]
    FS_VERSION = version


def journal_cksum_off(version):
    # Four v1 slot entries end exactly where v1 put the header
    # checksum, so v2 moves the slot table to 16 and the checksum to
    # the end of the sector. See tfs3.c's jh_cksum_off().
    return SECTOR - 4 if version >= 2 else 44


def journal_slots_off(version):
    return 16 if version >= 2 else 12
INODE_SIZE = 128
BACKUP_BLOCKS = GDT_BLOCKS + 1         # 16-block GDT snapshot + 1 superblock copy

DEFAULT_BYTES_PER_INODE = 16384

INO_NULL = 0
INO_ROOT = 1

TYPE_FILE = 0
TYPE_DIR = 1
TYPE_SYMLINK = 2

MAX_WRITE_BLOCKS = 12 + BLOCK // 4  # direct + one single-indirect block


def fnv1a(data: bytes) -> int:
    h = 0x811C9DC5
    for b in data:
        h ^= b
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h


def local_epoch() -> int:
    # See the module docstring's TIMESTAMPS note.
    return calendar.timegm(time.localtime())


def align4(n: int) -> int:
    return (n + 3) & ~3


# ---- on-disk structures -------------------------------------------------

def pack_superblock(total_blocks, bpg, ipg, gc, flags=0, version=None):
    version = VERSION if version is None else version
    body = struct.pack("<4sBBHIIIII", MAGIC, version, flags, 0,
                       total_blocks, bpg, ipg, gc, GEOMETRY[version]["group0"])
    if version >= 2:
        # The version's own layout, written down: validated on read,
        # never believed on its own (the constants are the authority).
        body += struct.pack("<III4x", GEOMETRY[version]["jdata"],
                            GEOMETRY[version]["jslots"], GEOMETRY[version]["gdt"])
    else:
        body += b"\x00" * 16
    assert len(body) == 44
    return body + struct.pack("<I", fnv1a(body))


def parse_superblock(sec: bytes):
    """Validate a superblock sector and, on success, switch the module's
    geometry to its version."""
    body = sec[:44]
    (magic, ver, flags, _res, total_blocks, bpg, ipg, gc,
     group0) = struct.unpack("<4sBBHIIIII", body[:28])
    cksum = struct.unpack("<I", sec[44:48])[0]
    if magic != MAGIC or not (VERSION_MIN <= ver <= VERSION):
        return None
    if fnv1a(body) != cksum:
        return None
    if group0 != GEOMETRY[ver]["group0"] or bpg != BLOCKS_PER_GROUP:
        return None
    if ver >= 2:
        jdata, jslots, gdt = struct.unpack("<III", body[28:40])
        if (jdata, jslots, gdt) != (GEOMETRY[ver]["jdata"], GEOMETRY[ver]["jslots"],
                                    GEOMETRY[ver]["gdt"]):
            return None
    set_geometry(ver)
    return dict(flags=flags, total_blocks=total_blocks, bpg=bpg,
                ipg=ipg, gc=gc, version=ver)


def pack_journal_header_empty(version=None):
    version = VERSION if version is None else version
    ck = journal_cksum_off(version)
    body = struct.pack("<4sBBHI", b"JRN3", 0, 0, 0, 0) + b"\x00" * (ck - 12)
    assert len(body) == ck
    return body + struct.pack("<I", fnv1a(body))


def pack_gdt_entry(free_blocks, free_inodes):
    body = struct.pack("<III", free_blocks, free_inodes, 0)
    return body + struct.pack("<I", fnv1a(body))


def pack_inode(typ, links, size, created, modified, ptrs):
    """ptrs: list of 15 u32s (12 direct + single + double + triple)."""
    assert len(ptrs) == 15
    head = struct.pack("<BBHQQQ", typ, 0, links, size, created, modified)
    assert len(head) == 28
    body = head + struct.pack("<15I", *ptrs)          # bytes 0..87
    tail = b"\x00" * 36                                # bytes 92..127
    cksum = fnv1a(body + tail)
    return body + struct.pack("<I", cksum) + tail


def parse_inode(raw: bytes):
    assert len(raw) == INODE_SIZE
    typ, _res, links = struct.unpack("<BBH", raw[0:4])
    size, created, modified = struct.unpack("<QQQ", raw[4:28])
    ptrs = list(struct.unpack("<15I", raw[28:88]))
    cksum = struct.unpack("<I", raw[88:92])[0]
    if fnv1a(raw[0:88] + raw[92:128]) != cksum:
        return None
    return dict(type=typ, links=links, size=size, created=created,
                modified=modified, ptrs=ptrs)


# ---- the image ----------------------------------------------------------

class Tfs3Image:
    # THE VOLUME SEAM, and it is the host-side twin of the kernel's.
    # kernel/fs/tfs3.c addresses everything relative to
    # {base_lba, sector_count} and reaches the disk only through
    # vol_read()/vol_write(); this class does the same through
    # read_bytes()/write_bytes(). That is what lets one image hold a
    # partition table AND a filesystem inside a partition, with neither
    # half knowing about the other.
    #
    # base_lba=0 with no explicit size is the flat whole-image volume
    # every existing caller gets, unchanged.
    def __init__(self, path, writable=False, base_lba=0, sectors=0):
        self.path = path
        self.f = open(path, "r+b" if writable else "rb")
        self.f.seek(0, os.SEEK_END)
        image_size = self.f.tell()
        self.base = base_lba * SECTOR
        if self.base >= image_size:
            raise SystemExit(f"{path}: volume starts at LBA {base_lba}, past the "
                             f"end of a {image_size // SECTOR}-sector image")
        # file_size is the VOLUME's size from here down -- every
        # geometry calculation in this file already derives from it, so
        # nothing else needs to learn about partitions.
        self.file_size = sectors * SECTOR if sectors else image_size - self.base
        sb = parse_superblock(self.read_bytes(SB_BLOCK * BLOCK, SECTOR))
        if sb is None:
            sb = self._try_backups()
        if sb is None:
            raise SystemExit(f"{path}: not a TFS3 image (superblock and "
                             f"backups all invalid) -- refusing to touch it")
        self.sb = sb
        self.itb = sb["ipg"] * INODE_SIZE // BLOCK  # inode-table blocks per group

    def _try_backups(self):
        # Which version wrote the disk is exactly what the unreadable
        # primary would have said, so try every version's backup
        # positions -- there are only two, both constants.
        total_blocks = self.file_size // BLOCK
        for ver in range(VERSION, VERSION_MIN - 1, -1):
            group0 = GEOMETRY[ver]["group0"]
            if total_blocks <= group0:
                continue
            gc = (total_blocks - group0 + BLOCKS_PER_GROUP - 1) // BLOCKS_PER_GROUP
            for g in backup_groups(gc):
                gbase = group0 + g * BLOCKS_PER_GROUP
                span = min(total_blocks - gbase, BLOCKS_PER_GROUP)
                blk = gbase + span - 1
                sb = parse_superblock(self.read_bytes(blk * BLOCK, SECTOR))
                if sb is not None and sb["version"] == ver:
                    print(f"note: primary superblock invalid -- using the "
                          f"backup in group {g}", file=sys.stderr)
                    return sb
        return None

    def close(self):
        self.f.close()

    # The ONLY two places the volume base is applied. Every other
    # method goes through these, exactly as tfs3.c's do -- adding an
    # offset anywhere else would be a second place for it to be wrong.
    def read_bytes(self, off, n):
        self.f.seek(self.base + off)
        data = self.f.read(n)
        return data + b"\x00" * (n - len(data))

    def write_bytes(self, off, data):
        self.f.seek(self.base + off)
        self.f.write(data)

    def read_block(self, blk):
        return self.read_bytes(blk * BLOCK, BLOCK)

    def write_block(self, blk, data):
        assert len(data) == BLOCK
        self.write_bytes(blk * BLOCK, data)

    # -- groups --

    def group_base(self, g):
        return GROUP0_START + g * BLOCKS_PER_GROUP

    def inode_table_block(self, g):
        return self.group_base(g) + 2  # block bitmap, inode bitmap, then table

    def data_start(self, g):
        return self.inode_table_block(g) + self.itb

    def group_span(self, g):
        """Blocks group g actually has -- short for a partial last group."""
        base = GROUP0_START + g * BLOCKS_PER_GROUP
        rest = self.sb["total_blocks"] - base
        return min(rest, BLOCKS_PER_GROUP) if rest > 0 else 0

    def group_backup_block(self, g):
        """Where g's superblock/GDT backup lives: its LAST real block."""
        span = self.group_span(g)
        return GROUP0_START + g * BLOCKS_PER_GROUP + span - 1 if span else 0

    def is_backup_group(self, g):
        return g in backup_groups(self.sb["gc"])

    # -- inodes --

    def inode_pos(self, ino):
        g = ino // self.sb["ipg"]
        idx = ino % self.sb["ipg"]
        return self.inode_table_block(g) * BLOCK + idx * INODE_SIZE

    def read_inode(self, ino):
        raw = self.read_bytes(self.inode_pos(ino), INODE_SIZE)
        node = parse_inode(raw)
        if node is None:
            raise SystemExit(f"inode {ino}: checksum mismatch -- refusing to guess")
        return node

    def write_inode(self, ino, packed):
        self.write_bytes(self.inode_pos(ino), packed)

    # -- bitmaps --

    def _bit(self, bitmap_block, i, val=None):
        off = bitmap_block * BLOCK + (i >> 3)
        byte = self.read_bytes(off, 1)[0]
        mask = 1 << (i & 7)
        if val is None:
            return 1 if (byte & mask) else 0
        byte = (byte | mask) if val else (byte & ~mask)
        self.write_bytes(off, bytes([byte]))
        return val

    def block_bit(self, blk, val=None):
        g = (blk - GROUP0_START) // BLOCKS_PER_GROUP
        i = (blk - GROUP0_START) % BLOCKS_PER_GROUP
        return self._bit(self.group_base(g), i, val)

    def inode_bit(self, ino, val=None):
        g = ino // self.sb["ipg"]
        i = ino % self.sb["ipg"]
        return self._bit(self.group_base(g) + 1, i, val)

    # -- group descriptors (free-count caches) --

    def read_gdt_entry(self, g):
        raw = self.read_bytes(GDT_BLOCK * BLOCK + g * 16, 16)
        fb, fi, _res, cksum = struct.unpack("<IIII", raw)
        return fb, fi

    def bump_gdt(self, g, dblocks=0, dinodes=0):
        fb, fi = self.read_gdt_entry(g)
        self.write_bytes(GDT_BLOCK * BLOCK + g * 16,
                         pack_gdt_entry(fb + dblocks, fi + dinodes))

    # -- allocation (first-fit with a preferred group) --

    def alloc_block(self, prefer_group=0):
        gc = self.sb["gc"]
        order = list(range(prefer_group, gc)) + list(range(0, prefer_group))
        for g in order:
            start = self.data_start(g) - self.group_base(g)
            # The group's REAL extent: the last group may be partial.
            span = self.group_span(g)
            end = span - (BACKUP_BLOCKS if self.is_backup_group(g) else 0)
            bitmap = self.read_block(self.group_base(g))
            for i in range(start, end):
                if not (bitmap[i >> 3] >> (i & 7)) & 1:
                    blk = self.group_base(g) + i
                    self.block_bit(blk, 1)
                    self.bump_gdt(g, dblocks=-1)
                    return blk
        raise SystemExit("image is out of free blocks")

    def alloc_inode(self, prefer_group=0):
        gc = self.sb["gc"]
        ipg = self.sb["ipg"]
        order = list(range(prefer_group, gc)) + list(range(0, prefer_group))
        for g in order:
            bitmap = self.read_block(self.group_base(g) + 1)
            for i in range(ipg):
                if not (bitmap[i >> 3] >> (i & 7)) & 1:
                    ino = g * ipg + i
                    self.inode_bit(ino, 1)
                    self.bump_gdt(g, dinodes=-1)
                    return ino
        raise SystemExit("image is out of free inodes")

    def free_block(self, blk):
        g = (blk - GROUP0_START) // BLOCKS_PER_GROUP
        self.block_bit(blk, 0)
        self.bump_gdt(g, dblocks=1)

    def free_inode(self, ino):
        self.inode_bit(ino, 0)
        self.bump_gdt(ino // self.sb["ipg"], dinodes=1)

    # -- file block maps (direct + single indirect only, see docstring) --

    def file_blocks(self, node):
        """Every data block number of a file/dir, in order."""
        nblocks = (node["size"] + BLOCK - 1) // BLOCK
        out = []
        for i in range(min(nblocks, 12)):
            out.append(node["ptrs"][i])
        if nblocks > 12:
            single = node["ptrs"][12]
            if single == 0:
                raise SystemExit("file needs a single-indirect block it doesn't have")
            table = self.read_block(single)
            for i in range(nblocks - 12):
                out.append(struct.unpack_from("<I", table, i * 4)[0])
        if any(p == 0 for p in out):
            raise SystemExit("sparse files are outside this tool's write scope")
        return out

    def read_file_data(self, ino):
        node = self.read_inode(ino)
        data = b""
        for blk in self.file_blocks(node):
            data += self.read_block(blk)
        return data[:node["size"]]

    # -- directories --

    def dirents(self, ino):
        """Yield (name, ino) for every live entry, including . and .."""
        node = self.read_inode(ino)
        if node["type"] != TYPE_DIR:
            raise SystemExit(f"inode {ino} is not a directory")
        for blk in self.file_blocks(node):
            raw = self.read_block(blk)
            off = 0
            while off < BLOCK:
                e_ino, rec_len, name_len = struct.unpack_from("<IHB", raw, off)
                if rec_len < 8 or off + rec_len > BLOCK:
                    break  # corrupt chain -- stop rather than loop
                if e_ino != 0 and name_len:
                    name = raw[off + 7:off + 7 + name_len].decode("latin-1")
                    yield name, e_ino
                off += rec_len

    def lookup(self, path):
        """Resolve an absolute path to an inode number, or None."""
        ino = INO_ROOT
        for comp in [c for c in path.split("/") if c]:
            found = None
            for name, e_ino in self.dirents(ino):
                if name == comp:
                    found = e_ino
                    break
            if found is None:
                return None
            ino = found
        return ino

    def dir_insert(self, dir_ino, name, child_ino):
        """Insert a dirent, growing the directory by a block if needed."""
        need = align4(7 + len(name))
        node = self.read_inode(dir_ino)
        blocks = self.file_blocks(node)
        for blk in blocks:
            raw = bytearray(self.read_block(blk))
            off = 0
            while off < BLOCK:
                e_ino, rec_len, name_len = struct.unpack_from("<IHB", raw, off)
                if rec_len < 8 or off + rec_len > BLOCK:
                    break
                used = align4(7 + name_len) if e_ino != 0 else 0
                if rec_len - used >= need:
                    # Split this entry's slack.
                    if e_ino != 0:
                        struct.pack_into("<IHB", raw, off, e_ino, used, name_len)
                        new_off = off + used
                        new_len = rec_len - used
                    else:
                        new_off = off
                        new_len = rec_len
                    struct.pack_into("<IHB", raw, new_off, child_ino, new_len, len(name))
                    raw[new_off + 7:new_off + 7 + len(name)] = name.encode("latin-1")
                    self.write_block(blk, bytes(raw))
                    return
                off += rec_len
        # No room -- append a fresh block to the directory.
        if len(blocks) >= 12:
            raise SystemExit("directory grew past 12 direct blocks -- outside this tool's write scope")
        g = (self.group_base(0) if dir_ino == 0 else 0)
        newblk = self.alloc_block(prefer_group=(blocks[0] - GROUP0_START) // BLOCKS_PER_GROUP if blocks else 0)
        raw = bytearray(BLOCK)
        struct.pack_into("<IHB", raw, 0, child_ino, BLOCK, len(name))
        raw[7:7 + len(name)] = name.encode("latin-1")
        self.write_block(newblk, bytes(raw))
        node["ptrs"][len(blocks)] = newblk
        node["size"] += BLOCK
        self.write_inode(dir_ino, pack_inode(TYPE_DIR, node["links"], node["size"],
                                             node["created"], local_epoch(), node["ptrs"]))
        _ = g

    def dir_remove(self, dir_ino, name):
        node = self.read_inode(dir_ino)
        for blk in self.file_blocks(node):
            raw = bytearray(self.read_block(blk))
            off = 0
            prev_off = None
            while off < BLOCK:
                e_ino, rec_len, name_len = struct.unpack_from("<IHB", raw, off)
                if rec_len < 8 or off + rec_len > BLOCK:
                    break
                if e_ino != 0 and raw[off + 7:off + 7 + name_len].decode("latin-1") == name:
                    if prev_off is not None:
                        # Fold into the previous entry's rec_len.
                        p_ino, p_len, p_nl = struct.unpack_from("<IHB", raw, prev_off)
                        struct.pack_into("<IHB", raw, prev_off, p_ino, p_len + rec_len, p_nl)
                    else:
                        struct.pack_into("<IHB", raw, off, 0, rec_len, 0)
                    self.write_block(blk, bytes(raw))
                    return True
                prev_off = off
                off += rec_len
        return False


def backup_groups(gc):
    if gc <= 0:
        return []
    if gc == 1:
        return [0]
    if gc == 2:
        return [1]
    return [1, gc - 1]


# ---- format --------------------------------------------------------------

def cmd_format(args):
    # The volume base, in bytes. Everything below is VOLUME-relative
    # from here on -- the same seam Tfs3Image applies, repeated here
    # because format() writes the raw file directly rather than through
    # the class (it is building the thing the class reads).
    at = getattr(args, "at_lba", 0) * SECTOR
    vol_sectors = getattr(args, "sectors", 0)

    size = args.size
    exists = os.path.exists(args.disk)
    if at and not exists:
        raise SystemExit("--at-lba formats a volume INSIDE an existing image "
                         "-- create and partition the image first")
    if exists:
        # The VOLUME's size, not the image's: a filesystem in a
        # partition that thinks it owns the whole disk will happily
        # allocate blocks past its own end.
        size = vol_sectors * SECTOR if vol_sectors else os.path.getsize(args.disk) - at
        with open(args.disk, "rb") as f:
            f.seek(at + SB_BLOCK * BLOCK)
            sec = f.read(SECTOR)
        old = parse_superblock(sec + b"\x00" * (SECTOR - len(sec))) if sec else None
        if old and not args.force:
            raise SystemExit(f"{args.disk}: already a valid TFS3 v{old['version']} "
                             "image -- pass --force to reformat it")

    # parse_superblock() above may have switched the module to some
    # other image's geometry. A fresh filesystem is the newest version
    # unless --fs-version says otherwise -- which exists so the OLDER
    # layout stays reachable: the kernel still mounts v1, and a format
    # nothing can produce is a code path nothing can test. Same rule as
    # `ata nodma` making the PIO fallback reachable on purpose.
    fs_version = getattr(args, "fs_version", VERSION)
    if fs_version not in GEOMETRY:
        raise SystemExit(f"unknown format version {fs_version} "
                         f"(known: {sorted(GEOMETRY)})")
    set_geometry(fs_version)

    total_blocks = size // BLOCK
    # CEILING, matching the kernel: the last group may be PARTIAL, which
    # is ext2/3/4's rule and what makes a filesystem smaller than one
    # 128 MiB group possible. The floor is a group's metadata plus room
    # for a root directory, not a whole group.
    gc = (total_blocks - GROUP0_START + BLOCKS_PER_GROUP - 1) // BLOCKS_PER_GROUP
    if total_blocks <= GROUP0_START + MIN_GROUP_BLOCKS:
        raise SystemExit(f"image too small: need more than "
                         f"{(GROUP0_START + MIN_GROUP_BLOCKS) * BLOCK} bytes "
                         f"(one group's metadata plus a root directory)")
    if gc > GDT_CAPACITY:
        gc = GDT_CAPACITY  # 512 GiB -- unreachable under LBA28, but honest
    ipg = min(BLOCKS_PER_GROUP, BLOCK * BLOCKS_PER_GROUP // args.bytes_per_inode)
    ipg = (ipg // (BLOCK // INODE_SIZE)) * (BLOCK // INODE_SIZE)  # whole table blocks
    itb = ipg * INODE_SIZE // BLOCK

    if args.dry_run:
        print(f"would format {args.disk}: {total_blocks} blocks, {gc} groups, "
              f"{ipg} inodes/group ({itb} table blocks), backups in groups "
              f"{backup_groups(gc)}")
        return

    if not exists:
        with open(args.disk, "wb") as f:
            f.truncate(size)

    now = local_epoch()
    with open(args.disk, "r+b") as f:
        # The wipefs rule (see kernel/fs/vfs.c's fs_format_backend and
        # fs_ops.h's wipe contract): a TFS2 superblock at LBA 0 must
        # not survive this image becoming TFS3, or the kernel's probe
        # keeps claiming it as TFS2. Only touched when the magic
        # actually matches -- an MBR/GPT at LBA 0 is left alone.
        f.seek(at)
        if f.read(4) == b"TFS2":
            f.seek(at)
            f.write(b"\x00" * SECTOR)
        # Same rule WITHIN this format: an older TFS3 version's backup
        # superblocks sit at positions this version's layout never
        # writes, so leaving them means a future reader whose primary
        # is damaged can mount a corpse with the wrong geometry.
        for ver, geo in GEOMETRY.items():
            if ver == fs_version or total_blocks <= geo["group0"]:
                continue
            old_gc = (total_blocks - geo["group0"] + BLOCKS_PER_GROUP - 1) // BLOCKS_PER_GROUP
            for g in backup_groups(old_gc):
                gbase = geo["group0"] + g * BLOCKS_PER_GROUP
                span = min(total_blocks - gbase, BLOCKS_PER_GROUP)
                f.seek(at + (gbase + span - 1) * BLOCK)
                f.write(b"\x00" * SECTOR)
        def wblk(blk, data):
            f.seek(at + blk * BLOCK)
            f.write(data)

        sb = pack_superblock(total_blocks, BLOCKS_PER_GROUP, ipg, gc,
                             version=fs_version)
        wblk(SB_BLOCK, sb + b"\x00" * (BLOCK - len(sb)))
        jh = pack_journal_header_empty(fs_version)
        wblk(JOURNAL_HEADER_BLOCK, jh + b"\x00" * (BLOCK - len(jh)))
        for i in range(JOURNAL_SLOTS):
            wblk(JOURNAL_DATA_BLOCK + i, b"\x00" * BLOCK)

        # Root directory: inode 1 in group 0, one dirent block.
        root_block = GROUP0_START + 2 + itb  # first data block of group 0
        gdt = bytearray()
        for g in range(gc):
            meta = 2 + itb
            gbase = GROUP0_START + g * BLOCKS_PER_GROUP
            span = min(total_blocks - gbase, BLOCKS_PER_GROUP)
            free_b = span - meta
            free_i = ipg
            if g in backup_groups(gc):
                free_b -= BACKUP_BLOCKS
            if g == 0:
                free_b -= 1        # root dirent block
                free_i -= 2        # ino 0 (null) + ino 1 (root)
            gdt += pack_gdt_entry(free_b, free_i)
        gdt += b"\x00" * (GDT_BLOCKS * BLOCK - len(gdt))
        for i in range(GDT_BLOCKS):
            wblk(GDT_BLOCK + i, bytes(gdt[i * BLOCK:(i + 1) * BLOCK]))

        # Inode tables must read as zeros (a zero inode fails its
        # checksum on purpose; stale valid inodes from a previous
        # format must not survive). Writing ~73 MB of literal zeros
        # (71 groups x 256 blocks on 9 GiB) would cost exactly that
        # much host disk on a sparse image, so: a freshly truncated
        # image already reads as zeros -- skip; an existing image
        # gets holes punched instead, which is byte-equivalent and
        # keeps the image sparse. The kernel's own format
        # (fsformat) still writes zeros -- it has no hole-punch, and
        # its ~3 s cost is accepted there.
        punch = None
        if exists:
            import ctypes
            libc = ctypes.CDLL(None, use_errno=True)
            fd = f.fileno()
            # VOLUME-relative, like wblk() -- `at` is added here and
            # nowhere else. Getting this wrong is not a subtle bug: on a
            # volume inside a partition, a flat punch lands on top of
            # the superblock this format just wrote, and the result is
            # a "successfully formatted" image with nothing in it.
            def punch(off, length):
                rc = libc.fallocate(fd, 0x03,  # PUNCH_HOLE | KEEP_SIZE
                                    ctypes.c_long(at + off), ctypes.c_long(length))
                if rc != 0:  # fall back to literal zeros
                    f.seek(at + off)
                    f.write(b"\x00" * length)

        for g in range(gc):
            base = GROUP0_START + g * BLOCKS_PER_GROUP
            span = min(total_blocks - base, BLOCKS_PER_GROUP)
            bbm = bytearray(BLOCK)
            def setbit(bm, i):
                bm[i >> 3] |= 1 << (i & 7)
            for i in range(2 + itb):
                setbit(bbm, i)
            if g in backup_groups(gc):
                for i in range(span - BACKUP_BLOCKS, span):
                    setbit(bbm, i)
            ibm = bytearray(BLOCK)
            if g == 0:
                setbit(bbm, 2 + itb)  # root dirent block
                setbit(ibm, INO_NULL)
                setbit(ibm, INO_ROOT)
            wblk(base, bytes(bbm))
            wblk(base + 1, bytes(ibm))
            if punch:
                punch((base + 2) * BLOCK, itb * BLOCK)
            # fresh image: region is already zeros, write nothing

        # Root inode + its dirent block (. and .. both point at root).
        ptrs = [0] * 15
        ptrs[0] = root_block
        root = pack_inode(TYPE_DIR, 2, BLOCK, now, now, ptrs)
        f.seek(at + (GROUP0_START + 2) * BLOCK + INO_ROOT * INODE_SIZE)
        f.write(root)
        raw = bytearray(BLOCK)
        struct.pack_into("<IHB", raw, 0, INO_ROOT, 12, 1)
        raw[7:8] = b"."
        struct.pack_into("<IHB", raw, 12, INO_ROOT, BLOCK - 12, 2)
        raw[19:21] = b".."
        wblk(root_block, bytes(raw))

        # Backup regions: GDT snapshot + superblock copy, byte-identical.
        for g in backup_groups(gc):
            gbase = GROUP0_START + g * BLOCKS_PER_GROUP
            tail = gbase + min(total_blocks - gbase, BLOCKS_PER_GROUP) - 1
            for i in range(GDT_BLOCKS):
                wblk(tail - GDT_BLOCKS + i, bytes(gdt[i * BLOCK:(i + 1) * BLOCK]))
            wblk(tail, sb + b"\x00" * (BLOCK - len(sb)))

    print(f"formatted {args.disk} as TFS3 v{fs_version}: {total_blocks} blocks, {gc} groups, "
          f"{ipg} inodes/group, backups in groups {backup_groups(gc)}")


# ---- path plumbing shared by write/mkdir/sync ----------------------------

def split_parent(path):
    path = "/" + "/".join(c for c in path.split("/") if c)
    if path == "/":
        raise SystemExit("refusing to operate on /")
    parent, _, name = path.rpartition("/")
    if len(name.encode("latin-1")) > 255:
        raise SystemExit(f"name too long: {name}")
    return (parent or "/"), name


def write_file(img, data, dst):
    parent_path, name = split_parent(dst)
    parent = img.lookup(parent_path)
    if parent is None or img.read_inode(parent)["type"] != TYPE_DIR:
        raise SystemExit(f"no such directory: {parent_path}")
    existing = img.lookup(dst)
    if existing is not None:
        delete_path(img, dst)  # replace = delete + rewrite (refuses dirs)

    nblocks = (len(data) + BLOCK - 1) // BLOCK
    if nblocks > MAX_WRITE_BLOCKS:
        raise SystemExit(f"{dst}: {len(data)} bytes needs {nblocks} blocks, over "
                         f"this tool's direct+single-indirect cap ({MAX_WRITE_BLOCKS})")
    pg = parent // img.sb["ipg"]
    ino = img.alloc_inode(prefer_group=pg)
    blocks = [img.alloc_block(prefer_group=pg) for _ in range(nblocks)]
    for i, blk in enumerate(blocks):
        chunk = data[i * BLOCK:(i + 1) * BLOCK]
        img.write_block(blk, chunk + b"\x00" * (BLOCK - len(chunk)))
    ptrs = [0] * 15
    for i in range(min(nblocks, 12)):
        ptrs[i] = blocks[i]
    if nblocks > 12:
        single = img.alloc_block(prefer_group=pg)
        table = bytearray(BLOCK)
        for i, blk in enumerate(blocks[12:]):
            struct.pack_into("<I", table, i * 4, blk)
        img.write_block(single, bytes(table))
        ptrs[12] = single
    now = local_epoch()
    img.write_inode(ino, pack_inode(TYPE_FILE, 1, len(data), now, now, ptrs))
    img.dir_insert(parent, name, ino)
    return ino


def mkdir_path(img, path):
    parent_path, name = split_parent(path)
    parent = img.lookup(parent_path)
    if parent is None:
        raise SystemExit(f"no such directory: {parent_path}")
    if img.lookup(path) is not None:
        return img.lookup(path)  # mkdir -p behavior for sync's benefit
    pg = parent // img.sb["ipg"]
    ino = img.alloc_inode(prefer_group=pg)
    blk = img.alloc_block(prefer_group=pg)
    raw = bytearray(BLOCK)
    struct.pack_into("<IHB", raw, 0, ino, 12, 1)
    raw[7:8] = b"."
    struct.pack_into("<IHB", raw, 12, parent, BLOCK - 12, 2)
    raw[19:21] = b".."
    img.write_block(blk, bytes(raw))
    now = local_epoch()
    ptrs = [0] * 15
    ptrs[0] = blk
    img.write_inode(ino, pack_inode(TYPE_DIR, 2, BLOCK, now, now, ptrs))
    img.dir_insert(parent, name, ino)
    # Parent gains a link (the child's ..).
    p = img.read_inode(parent)
    img.write_inode(parent, pack_inode(TYPE_DIR, p["links"] + 1, p["size"],
                                       p["created"], p["modified"], p["ptrs"]))
    return ino


def delete_path(img, path):
    parent_path, name = split_parent(path)
    ino = img.lookup(path)
    if ino is None:
        raise SystemExit(f"no such path: {path}")
    node = img.read_inode(ino)
    parent = img.lookup(parent_path)
    if node["type"] == TYPE_DIR:
        live = [n for n, _ in img.dirents(ino) if n not in (".", "..")]
        if live:
            raise SystemExit(f"{path}: directory not empty (same refusal as the OS)")
    blocks = img.file_blocks(node)
    if not img.dir_remove(parent, name):
        raise SystemExit(f"{path}: dirent vanished mid-delete?")
    for blk in blocks:
        img.free_block(blk)
    if node["size"] > 12 * BLOCK:
        img.free_block(node["ptrs"][12])
    # Kill the inode: zero it (fails checksum by design) and free the bit.
    img.write_inode(ino, b"\x00" * INODE_SIZE)
    img.free_inode(ino)
    if node["type"] == TYPE_DIR:
        p = img.read_inode(parent)
        img.write_inode(parent, pack_inode(TYPE_DIR, p["links"] - 1, p["size"],
                                           p["created"], p["modified"], p["ptrs"]))


# ---- commands -------------------------------------------------------------

def cmd_info(args):
    img = Tfs3Image(args.disk, base_lba=getattr(args, "at_lba", 0), sectors=getattr(args, "sectors", 0))
    sb = img.sb
    print(f"TFS3 v{sb['version']}: {sb['total_blocks']} blocks, {sb['gc']} groups, "
          f"{sb['ipg']} inodes/group, flags={sb['flags']}")
    free_b = free_i = 0
    for g in range(sb["gc"]):
        fb, fi = img.read_gdt_entry(g)
        free_b += fb
        free_i += fi
    print(f"free: {free_b} blocks ({free_b * BLOCK // 1024} KB), {free_i} inodes")
    img.close()


def cmd_ls(args):
    img = Tfs3Image(args.disk, base_lba=getattr(args, "at_lba", 0), sectors=getattr(args, "sectors", 0))
    ino = img.lookup(args.path or "/")
    if ino is None:
        raise SystemExit(f"no such path: {args.path}")
    for name, e_ino in sorted(img.dirents(ino)):
        node = img.read_inode(e_ino)
        kind = "d" if node["type"] == TYPE_DIR else "-"
        print(f"{kind} {node['size']:>10} ino={e_ino:<6} {name}")
    img.close()


def cmd_read(args):
    img = Tfs3Image(args.disk, base_lba=getattr(args, "at_lba", 0), sectors=getattr(args, "sectors", 0))
    ino = img.lookup(args.src)
    if ino is None:
        raise SystemExit(f"no such file: {args.src}")
    data = img.read_file_data(ino)
    if args.dst:
        with open(args.dst, "wb") as f:
            f.write(data)
        print(f"read {len(data)} bytes -> {args.dst}")
    else:
        sys.stdout.buffer.write(data)
    img.close()


def cmd_write(args):
    with open(args.src, "rb") as f:
        data = f.read()
    img = Tfs3Image(args.disk, writable=True, base_lba=getattr(args, "at_lba", 0), sectors=getattr(args, "sectors", 0))
    ino = write_file(img, data, args.dst)
    print(f"wrote {len(data)} bytes -> {args.dst} (ino {ino})")
    img.close()


def cmd_mkdir(args):
    img = Tfs3Image(args.disk, writable=True, base_lba=getattr(args, "at_lba", 0), sectors=getattr(args, "sectors", 0))
    ino = mkdir_path(img, args.path)
    print(f"mkdir {args.path} (ino {ino})")
    img.close()


def cmd_delete(args):
    img = Tfs3Image(args.disk, writable=True, base_lba=getattr(args, "at_lba", 0), sectors=getattr(args, "sectors", 0))
    delete_path(img, args.path)
    print(f"deleted {args.path}")
    img.close()


def cmd_sync(args):
    """Mirror a seed tree: <seed>/once/ copied only if missing,
    <seed>/sync/ content-hash-synced -- same convention as
    the convention the seed tree has always used: `once/` is a first-run
    default a running system may then edit, `sync/` is build output that
    should always match."""
    img = Tfs3Image(args.disk, writable=True, base_lba=getattr(args, "at_lba", 0), sectors=getattr(args, "sectors", 0))
    wrote = skipped = 0
    for mode in ("once", "sync"):
        root = os.path.join(args.seed_dir, mode)
        if not os.path.isdir(root):
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            rel = os.path.relpath(dirpath, root)
            for d in sorted(dirnames):
                target = "/" + os.path.normpath(os.path.join(rel, d)).lstrip("./")
                mkdir_path(img, target)
            for fn in sorted(filenames):
                target = "/" + os.path.normpath(os.path.join(rel, fn)).lstrip("./")
                with open(os.path.join(dirpath, fn), "rb") as f:
                    data = f.read()
                existing = img.lookup(target)
                if existing is not None:
                    if mode == "once":
                        skipped += 1
                        continue
                    on_disk = img.read_file_data(existing)
                    if hashlib.sha256(on_disk).digest() == hashlib.sha256(data).digest():
                        skipped += 1
                        continue
                write_file(img, data, target)
                wrote += 1
    print(f"sync: {wrote} written, {skipped} unchanged/kept")
    img.close()


def cmd_trim(args):
    """Punch holes through every free block -- same job (and same
    safety argument) as the seed step: a free block holds nothing, so
    handing it back to the host costs nothing and keeps the image
    sparse."""
    import ctypes
    FALLOC_FL_KEEP_SIZE = 0x01
    FALLOC_FL_PUNCH_HOLE = 0x02
    img = Tfs3Image(args.disk, base_lba=getattr(args, "at_lba", 0), sectors=getattr(args, "sectors", 0))
    libc = ctypes.CDLL(None, use_errno=True)
    fd = os.open(args.disk, os.O_RDWR)
    punched = 0
    try:
        for g in range(img.sb["gc"]):
            base = img.group_base(g)
            bitmap = img.read_block(base)
            end = BLOCKS_PER_GROUP
            run_start = None
            for i in range(end + 1):
                free = (i < end and not (bitmap[i >> 3] >> (i & 7)) & 1)
                if free and run_start is None:
                    run_start = i
                elif not free and run_start is not None:
                    off = (base + run_start) * BLOCK
                    length = (i - run_start) * BLOCK
                    rc = libc.fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                                        ctypes.c_long(off), ctypes.c_long(length))
                    if rc != 0:
                        raise SystemExit(f"fallocate failed, errno {ctypes.get_errno()}")
                    punched += i - run_start
                    run_start = None
    finally:
        os.close(fd)
        img.close()
    print(f"trim: punched {punched} free blocks ({punched * BLOCK // 1024} KB)")


def cmd_corrupt(args):
    """Inject KNOWN damage so the kernel's fsck (and its backup/journal
    recovery) can be tested against inconsistencies whose exact shape
    is known in advance -- the same reasoning as the writer's
    corrupt command: the kernel deliberately avoids producing these,
    so without this fsck could only ever be proven to report 'clean'.
    """
    img = Tfs3Image(args.disk, writable=True, base_lba=getattr(args, "at_lba", 0), sectors=getattr(args, "sectors", 0))
    did = []

    if args.leak:
        n = args.leak
        g = 0
        start = img.data_start(g) - img.group_base(g)
        bitmap = bytearray(img.read_block(img.group_base(g)))
        leaked = 0
        for i in range(start, BLOCKS_PER_GROUP):
            if leaked >= n:
                break
            if not (bitmap[i >> 3] >> (i & 7)) & 1:
                bitmap[i >> 3] |= 1 << (i & 7)
                leaked += 1
        img.write_block(img.group_base(g), bytes(bitmap))
        did.append(f"leaked {leaked} blocks (allocated, referenced by nothing)")

    if args.free_referenced:
        # Clear the bitmap bits of blocks a real file owns.
        target = args.free_referenced_path or _largest_file(img)
        ino = img.lookup(target)
        if ino is None:
            raise SystemExit(f"no such file to damage: {target}")
        blocks = img.file_blocks(img.read_inode(ino))[:args.free_referenced]
        for blk in blocks:
            img.block_bit(blk, 0)
        did.append(f"freed {len(blocks)} blocks still referenced by {target}")

    if args.bad_link_count:
        ino = img.lookup(args.bad_link_count)
        if ino is None:
            raise SystemExit(f"no such path: {args.bad_link_count}")
        raw = bytearray(img.read_bytes(img.inode_pos(ino), INODE_SIZE))
        links = struct.unpack_from("<H", raw, 2)[0]
        struct.pack_into("<H", raw, 2, links + 3)
        body = bytes(raw[0:88]) + bytes(raw[92:128])
        struct.pack_into("<I", raw, 88, fnv1a(body))
        img.write_bytes(img.inode_pos(ino), bytes(raw))
        did.append(f"link count of {args.bad_link_count}: {links} -> {links + 3}")

    if args.smash_superblock:
        img.write_bytes(SB_BLOCK * BLOCK, b"\x00" * SECTOR)
        did.append("primary superblock zeroed (backups intact)")

    if args.stage_journal:
        ino = img.lookup(args.stage_journal)
        if ino is None:
            raise SystemExit(f"no such path: {args.stage_journal}")
        # A committed transaction whose one image is the inode-table
        # block with the target's `modified` set to a marker epoch --
        # replay applies it, so `stat` shows 1970-01-15 00:00:45
        # (epoch 1209645) exactly when (and only when) replay ran.
        pos = img.inode_pos(ino)
        blk = pos // BLOCK
        img_block = bytearray(img.read_block(blk))
        off = pos % BLOCK
        struct.pack_into("<Q", img_block, off + 20, 1209645)
        body = bytes(img_block[off:off + 88]) + bytes(img_block[off + 92:off + 128])
        struct.pack_into("<I", img_block, off + 88, fnv1a(body))
        img.write_block(JOURNAL_DATA_BLOCK, bytes(img_block))
        hdr = bytearray(SECTOR)
        hdr[0:4] = b"JRN3"
        hdr[4] = 1  # committed
        hdr[5] = 1  # one block
        struct.pack_into("<I", hdr, 8, 424242)
        slots = journal_slots_off(FS_VERSION)
        ck = journal_cksum_off(FS_VERSION)
        struct.pack_into("<II", hdr, slots, blk,
                         fnv1a(bytes(img_block)) if not args.stage_journal_torn
                         else (fnv1a(bytes(img_block)) ^ 0xDEADBEEF))
        struct.pack_into("<I", hdr, ck, fnv1a(bytes(hdr[:ck])))
        img.write_bytes(JOURNAL_HEADER_BLOCK * BLOCK, bytes(hdr))
        did.append(("staged a TORN committed transaction (replay must discard it)"
                    if args.stage_journal_torn else
                    f"staged a committed transaction: {args.stage_journal}'s mtime -> epoch 1209645 on replay"))

    img.close()
    if not did:
        raise SystemExit("corrupt: pass at least one damage flag (see --help)")
    for d in did:
        print(f"corrupt: {d}")


def _largest_file(img, dir_ino=INO_ROOT, prefix=""):
    best, best_size = None, -1
    for name, ino in img.dirents(dir_ino):
        if name in (".", ".."):
            continue
        node = img.read_inode(ino)
        path = f"{prefix}/{name}"
        if node["type"] == TYPE_DIR:
            sub = _largest_file(img, ino, path)
            if sub:
                sn = img.read_inode(img.lookup(sub))
                if sn["size"] > best_size:
                    best, best_size = sub, sn["size"]
        elif node["size"] > best_size:
            best, best_size = path, node["size"]
    return best


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("format")
    p.add_argument("disk")
    p.add_argument("--size", type=int, default=0, help="create the image at this size")
    p.add_argument("--bytes-per-inode", type=int, default=DEFAULT_BYTES_PER_INODE)
    p.add_argument("--force", action="store_true")
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("--at-lba", type=int, default=0, metavar="N",
                   help="format a volume starting at this sector of an existing "
                        "image, rather than the whole image -- i.e. inside a "
                        "partition. Pair with --sectors.")
    p.add_argument("--sectors", type=int, default=0, metavar="N",
                   help="the volume's size in sectors (default: to the end of "
                        "the image). REQUIRED with --at-lba when anything "
                        "follows the partition, such as GPT's backup header.")
    p.add_argument("--fs-version", type=int, default=VERSION, metavar="N",
                   help="on-disk format version to write (1 = the four-slot "
                        "journal, 2 = the 32-slot one, default). v1 exists so "
                        "the kernel's still-supported older layout stays "
                        "testable -- see tools/tfs3_v1_test.py")
    p.set_defaults(fn=cmd_format)

    # Every subcommand takes --at-lba/--sectors, because a volume in a
    # partition has to be reachable by all of them, not only by format:
    # `sync` is how the seed tree gets in, and `ls`/`info` are how a
    # human checks it landed.
    def vol_opts(p):
        p.add_argument("--at-lba", type=int, default=0, metavar="N",
                       help="the volume starts at this sector of the image")
        p.add_argument("--sectors", type=int, default=0, metavar="N",
                       help="the volume's size in sectors")
        return p

    p = vol_opts(sub.add_parser("info")); p.add_argument("disk"); p.set_defaults(fn=cmd_info)
    p = vol_opts(sub.add_parser("ls")); p.add_argument("disk"); p.add_argument("path", nargs="?", default="/"); p.set_defaults(fn=cmd_ls)
    p = vol_opts(sub.add_parser("read")); p.add_argument("disk"); p.add_argument("src"); p.add_argument("dst", nargs="?"); p.set_defaults(fn=cmd_read)
    p = vol_opts(sub.add_parser("write")); p.add_argument("disk"); p.add_argument("src"); p.add_argument("dst"); p.set_defaults(fn=cmd_write)
    p = vol_opts(sub.add_parser("mkdir")); p.add_argument("disk"); p.add_argument("path"); p.set_defaults(fn=cmd_mkdir)
    p = vol_opts(sub.add_parser("delete")); p.add_argument("disk"); p.add_argument("path"); p.set_defaults(fn=cmd_delete)
    p = vol_opts(sub.add_parser("sync")); p.add_argument("disk"); p.add_argument("seed_dir"); p.set_defaults(fn=cmd_sync)
    p = vol_opts(sub.add_parser("trim")); p.add_argument("disk"); p.set_defaults(fn=cmd_trim)

    p = vol_opts(sub.add_parser("corrupt"))
    p.add_argument("disk")
    p.add_argument("--leak", type=int, default=0, metavar="N")
    p.add_argument("--free-referenced", type=int, default=0, metavar="N")
    p.add_argument("--free-referenced-path", default=None)
    p.add_argument("--bad-link-count", default=None, metavar="PATH")
    p.add_argument("--smash-superblock", action="store_true")
    p.add_argument("--stage-journal", default=None, metavar="PATH")
    p.add_argument("--stage-journal-torn", action="store_true")
    p.set_defaults(fn=cmd_corrupt)

    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
