#!/usr/bin/env python3
"""Host-side read/write tool for TFS2 v3 (block-addressed) disk images
-- lets you get files onto disk.img without booting toy-os at all, and
inspect what's already there. Byte-exact against docs/tfs2-spec.md; the
`read`/`ls` code paths started as that doc's own reference reader.

Subcommands:
  format <disk.img> [--force]                 initialize a blank/foreign image as an empty TFS2 v3 image
  write  <disk.img> <tfs-path> <local-file>   write one local file in
  read   <disk.img> <tfs-path> [-o out]       read one file out (stdout by default)
  ls     <disk.img> <tfs-path>                list a directory's direct children
  sync   <disk.img> <seed-dir> [--dest /]     mirror a seed directory in
  corrupt <disk.img> --leak N | --free-referenced N | --bad-pointer PATH
                                              inject a known inconsistency, for
                                              testing the kernel's `fsck`

See `sync`'s docstring below for the seed-directory convention
(once/ vs sync/). Run any subcommand with -h for its own options.

*** WRITE SCOPE ***: this tool only allocates direct + single-indirect
blocks (12 + 1024 blocks = ~4.03 MB max per file) -- plenty for ELF
binaries and config/text files, which is everything it was built for.
double_indirect/triple_indirect are read (a file written some other
way, e.g. by toy-os itself, could use them) but this tool will refuse
to WRITE a file that doesn't fit in direct+single-indirect rather than
silently truncate it. Extend build_single_indirect()'s caller if a
real need for double-indirect writes ever comes up.

*** SAFETY ***: writes go straight to the image you point it at, same
as toy-os's own writes do -- there's no automatic disk.img.new copy.
Pass --dry-run on `write`/`sync` to preview what would change without
touching the image at all.

*** TIMESTAMPS ***: toy-os's own `created`/`modified` fields are
broken-down LOCAL time from its own RTC (see fs.h's fs_stat() comment)
-- this tool has no access to that clock, so it stamps new/updated
records with the HOST machine's current local time instead. Close
enough for "when was this seeded" purposes; don't treat it as
authoritative toy-os wall-clock time.
"""
import argparse
import hashlib
import os
import struct
import sys
from datetime import datetime

# ---- constants (mirror kernel/fs/tfs.c exactly -- see
# docs/tfs2-spec.md for the byte-level derivation of every one of these) ----

SECTOR = 512
BLOCK = 4096
BLOCK_SECTORS = BLOCK // SECTOR  # 8
PTRS_PER_BLOCK = BLOCK // 4  # 1024 uint32 block numbers per indirect block

FS_DISK_VERSION = 3  # tfs.c's FS_DISK_VERSION -- v3 = FS_MAX_FILES 256 (was 32 in v2)

FS_PATH_MAX = 64
FS_MAX_FILES = 256  # tfs.c/fs.h -- raised from 32 in the v3 layout
FS_N_DIRECT = 12

FS_DISK_TOTAL_BYTES = 9 * 1024 * 1024 * 1024  # 9 GiB, tfs.c's FS_DISK_TOTAL_BYTES
FS_DISK_TOTAL_SECTORS = FS_DISK_TOTAL_BYTES // SECTOR
FS_DISK_TOTAL_BLOCKS = FS_DISK_TOTAL_SECTORS // BLOCK_SECTORS

RECORD_SECTORS = 1
RECORD_BYTES = RECORD_SECTORS * SECTOR
SUPERBLOCK_LBA = 0
JOURNAL_HEADER_LBA = 1
JOURNAL_DATA_LBA = 2
TABLE_START_LBA = 3
BITMAP_START_LBA = TABLE_START_LBA + FS_MAX_FILES * RECORD_SECTORS
BITMAP_BYTES = (FS_DISK_TOTAL_BLOCKS + 7) // 8
BITMAP_SECTORS = (BITMAP_BYTES + SECTOR - 1) // SECTOR
DATA_START_LBA_RAW = BITMAP_START_LBA + BITMAP_SECTORS
DATA_START_BLOCK = (DATA_START_LBA_RAW + BLOCK_SECTORS - 1) // BLOCK_SECTORS

REC_OFF_TYPE = FS_PATH_MAX  # 64
REC_OFF_USED = FS_PATH_MAX + 1  # 65
REC_OFF_SIZE = FS_PATH_MAX + 2  # 66, 8 bytes uint64 LE
REC_OFF_CREATED = REC_OFF_SIZE + 8  # 74
REC_OFF_MODIFIED = REC_OFF_CREATED + 7  # 81
REC_OFF_DIRECT = REC_OFF_MODIFIED + 7  # 88, 12 * uint32
REC_OFF_SINGLE = REC_OFF_DIRECT + FS_N_DIRECT * 4  # 136
REC_OFF_DOUBLE = REC_OFF_SINGLE + 4  # 140
REC_OFF_TRIPLE = REC_OFF_DOUBLE + 4  # 144

FS_TYPE_FILE = 0
FS_TYPE_DIR = 1

MAX_WRITE_BLOCKS = FS_N_DIRECT + PTRS_PER_BLOCK  # 1036 -- direct + single-indirect only
MAX_WRITE_BYTES = MAX_WRITE_BLOCKS * BLOCK  # ~4.03 MB, see WRITE SCOPE note above

FNV_OFFSET = 0x811C9DC5
FNV_PRIME = 0x01000193


def fnv1a(data):
    h = FNV_OFFSET
    for b in data:
        h ^= b
        h = (h * FNV_PRIME) & 0xFFFFFFFF
    return h


def build_journal_header(commit, slot, checksum):
    buf = bytearray(SECTOR)
    buf[0:4] = b"JRN1"
    buf[4] = commit
    struct.pack_into("<I", buf, 5, slot)
    struct.pack_into("<I", buf, 9, checksum)
    return bytes(buf)


def normalize(path):
    """Mirrors tfs.c's normalize(): bare names get a leading '/', and
    the result must fit FS_PATH_MAX and have no '.'/'..'/trailing-slash
    components (other than the bare root)."""
    if not path.startswith("/"):
        path = "/" + path
    if len(path.encode("utf-8")) >= FS_PATH_MAX:
        raise ValueError(f"path too long (max {FS_PATH_MAX - 1} bytes): {path}")
    if path != "/":
        if path.endswith("/"):
            raise ValueError(f"trailing slash not allowed: {path}")
        for part in path.split("/")[1:]:
            if part in (".", ".."):
                raise ValueError(f"'.'/'..' path components not allowed: {path}")
            if part == "":
                raise ValueError(f"empty path component (double slash?): {path}")
    return path


def path_parent(norm_path):
    if norm_path == "/":
        return "/"
    idx = norm_path.rfind("/")
    return "/" if idx == 0 else norm_path[:idx]


# ---- low-level sector/block I/O ----

class Image:
    def __init__(self, path, dry_run=False):
        self.path = path
        self.dry_run = dry_run
        mode = "r+b"
        self.f = open(path, mode)
        self.f.seek(0, os.SEEK_END)
        size = self.f.tell()
        if size < DATA_START_LBA_RAW * SECTOR:
            raise ValueError(
                f"{path} is only {size} bytes -- too small to be a real "
                f"TFS2 image (expected at least {DATA_START_LBA_RAW * SECTOR})"
            )
        self.check_superblock()
        self.check_journal()
        self.bitmap = self._read_bitmap()
        self.records = self._read_table()  # list of FS_MAX_FILES dicts
        self._bitmap_scan_hint = DATA_START_BLOCK

    def close(self):
        self.f.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    # -- superblock / journal --

    def check_superblock(self):
        self.f.seek(SUPERBLOCK_LBA * SECTOR)
        sb = self.f.read(SECTOR)
        if sb[0:4] != b"TFS2" or sb[4] != FS_DISK_VERSION:
            raise ValueError(
                f"{self.path} is not a TFS2 v{FS_DISK_VERSION} image "
                f"(bad magic/version; a v2 image from before the FS_MAX_FILES "
                f"bump reads as foreign here, same as the kernel treats it) -- "
                "refusing to touch it"
            )

    def check_journal(self):
        self.f.seek(JOURNAL_HEADER_LBA * SECTOR)
        hdr = self.f.read(SECTOR)
        if hdr[0:4] != b"JRN1":
            return
        commit = hdr[4]
        if commit:
            slot = struct.unpack_from("<I", hdr, 5)[0]
            print(
                f"WARNING: {self.path} has a pending unapplied journal entry "
                f"for slot {slot} (unclean shutdown). Boot toy-os once to let "
                f"it finish recovering before trusting/editing this image.",
                file=sys.stderr,
            )

    # -- raw sector/block helpers --

    def read_sector(self, lba):
        self.f.seek(lba * SECTOR)
        data = self.f.read(SECTOR)
        return data if len(data) == SECTOR else data + bytes(SECTOR - len(data))

    def write_sector(self, lba, data):
        assert len(data) == SECTOR
        if self.dry_run:
            return
        self.f.seek(lba * SECTOR)
        self.f.write(data)

    def read_block(self, block):
        if block == 0:
            return bytes(BLOCK)
        self.f.seek(block * BLOCK_SECTORS * SECTOR)
        data = self.f.read(BLOCK)
        return data if len(data) == BLOCK else data + bytes(BLOCK - len(data))

    def write_block(self, block, data):
        assert block != 0, "refusing to write to block 0 (the null sentinel)"
        assert len(data) == BLOCK
        if self.dry_run:
            return
        self.f.seek(block * BLOCK_SECTORS * SECTOR)
        self.f.write(data)

    # -- bitmap --

    def _read_bitmap(self):
        buf = bytearray()
        for s in range(BITMAP_SECTORS):
            buf += self.read_sector(BITMAP_START_LBA + s)
        return buf[:BITMAP_BYTES]

    def _persist_bitmap_bit(self, block):
        sector = (block // 8) // SECTOR
        off = sector * SECTOR
        self.write_sector(BITMAP_START_LBA + sector, bytes(self.bitmap[off:off + SECTOR]))

    def _bit(self, b):
        return (self.bitmap[b // 8] >> (b % 8)) & 1

    def _set_bit(self, b, val):
        if val:
            self.bitmap[b // 8] |= 1 << (b % 8)
        else:
            self.bitmap[b // 8] &= ~(1 << (b % 8)) & 0xFF

    def alloc_block(self):
        for pass_ in range(2):
            start = self._bitmap_scan_hint if pass_ == 0 else DATA_START_BLOCK
            end = FS_DISK_TOTAL_BLOCKS if pass_ == 0 else self._bitmap_scan_hint
            for b in range(start, end):
                if not self._bit(b):
                    self._set_bit(b, 1)
                    self._persist_bitmap_bit(b)
                    self._bitmap_scan_hint = b + 1
                    return b
        raise RuntimeError("disk full -- no free blocks left")

    def free_block(self, b):
        if b == 0 or b < DATA_START_BLOCK:
            return
        self._set_bit(b, 0)
        self._persist_bitmap_bit(b)
        if b < self._bitmap_scan_hint:
            self._bitmap_scan_hint = b

    def free_tree(self, block, depth):
        """Mirrors tfs.c's free_tree(): frees `block` and, if depth > 0,
        every block it indirectly points to first."""
        if block == 0:
            return
        if depth > 0:
            ptrs = struct.unpack(f"<{PTRS_PER_BLOCK}I", self.read_block(block))
            for child in ptrs:
                if child:
                    self.free_tree(child, depth - 1)
        self.free_block(block)

    def free_all_blocks(self, rec):
        for b in rec["direct"]:
            if b:
                self.free_block(b)
        self.free_tree(rec["single_indirect"], 1)
        self.free_tree(rec["double_indirect"], 2)
        self.free_tree(rec["triple_indirect"], 3)

    # -- table --

    def _parse_record(self, buf):
        path = buf[0:FS_PATH_MAX].split(b"\x00", 1)[0].decode("utf-8", "replace")
        return {
            "path": path,
            "type": buf[REC_OFF_TYPE],
            "used": bool(buf[REC_OFF_USED]),
            "size": struct.unpack_from("<Q", buf, REC_OFF_SIZE)[0],
            "created": self._parse_rtc(buf, REC_OFF_CREATED),
            "modified": self._parse_rtc(buf, REC_OFF_MODIFIED),
            "direct": list(struct.unpack_from(f"<{FS_N_DIRECT}I", buf, REC_OFF_DIRECT)),
            "single_indirect": struct.unpack_from("<I", buf, REC_OFF_SINGLE)[0],
            "double_indirect": struct.unpack_from("<I", buf, REC_OFF_DOUBLE)[0],
            "triple_indirect": struct.unpack_from("<I", buf, REC_OFF_TRIPLE)[0],
        }

    @staticmethod
    def _parse_rtc(buf, off):
        hour, minute, second, day, month = buf[off:off + 5]
        year = struct.unpack_from("<H", buf, off + 5)[0]
        try:
            return datetime(year, month, day, hour, minute, second)
        except ValueError:
            return None

    @staticmethod
    def _pack_rtc(buf, off, dt):
        buf[off] = dt.hour
        buf[off + 1] = dt.minute
        buf[off + 2] = dt.second
        buf[off + 3] = dt.day
        buf[off + 4] = dt.month
        struct.pack_into("<H", buf, off + 5, dt.year)

    def _read_table(self):
        recs = []
        for i in range(FS_MAX_FILES):
            recs.append(self._parse_record(self.read_sector(TABLE_START_LBA + i)))
        return recs

    def find(self, norm_path):
        for i, r in enumerate(self.records):
            if r["used"] and r["path"] == norm_path:
                return i, r
        return None, None

    def find_free_index(self):
        for i, r in enumerate(self.records):
            if not r["used"]:
                return i
        return None

    def is_dir(self, norm_path):
        if norm_path == "/":
            return True
        _, r = self.find(norm_path)
        return r is not None and r["type"] == FS_TYPE_DIR

    def parent_is_dir(self, norm_path):
        parent = path_parent(norm_path)
        return self.is_dir(parent)

    def build_record_bytes(self, path, is_dir, size, direct, single, double, triple, created, modified):
        buf = bytearray(RECORD_BYTES)
        pb = path.encode("utf-8")
        buf[0:len(pb)] = pb
        buf[REC_OFF_TYPE] = FS_TYPE_DIR if is_dir else FS_TYPE_FILE
        buf[REC_OFF_USED] = 1
        struct.pack_into("<Q", buf, REC_OFF_SIZE, size)
        self._pack_rtc(buf, REC_OFF_CREATED, created)
        self._pack_rtc(buf, REC_OFF_MODIFIED, modified)
        for i in range(FS_N_DIRECT):
            struct.pack_into("<I", buf, REC_OFF_DIRECT + i * 4, direct[i] if i < len(direct) else 0)
        struct.pack_into("<I", buf, REC_OFF_SINGLE, single)
        struct.pack_into("<I", buf, REC_OFF_DOUBLE, double)
        struct.pack_into("<I", buf, REC_OFF_TRIPLE, triple)
        return bytes(buf)

    def persist_record(self, index, raw512):
        """Mirrors tfs.c's persist_record(): stage -> commit -> apply -> clear."""
        checksum = fnv1a(raw512)
        self._write_journal_header(1, index, checksum, raw512)
        self.write_sector(TABLE_START_LBA + index, raw512)
        self._write_journal_header(0, 0, 0, None)
        self.records[index] = self._parse_record(raw512)

    def _write_journal_header(self, commit, slot, checksum, data):
        self.write_sector(JOURNAL_HEADER_LBA, build_journal_header(commit, slot, checksum))
        if commit and data is not None:
            self.write_sector(JOURNAL_DATA_LBA, data)

    def delete_record(self, index):
        """Zeroes a slot's `used` byte -- doesn't touch its path/pointer
        bytes (matches tfs.c's own delete, which only clears `used`)."""
        buf = bytearray(self.read_sector(TABLE_START_LBA + index))
        buf[REC_OFF_USED] = 0
        self.persist_record(index, bytes(buf))
        self.records[index]["used"] = False

    # -- directory creation (mirrors tfs_mkdir()'s parent-must-exist rule) --

    def ensure_dir_chain(self, norm_dir_path, now, log=None):
        """Creates every missing directory component of norm_dir_path,
        shallowest first (mirrors what a chain of fs_mkdir() calls from
        toy-os itself would do). Errors if a component exists but isn't
        a directory. Returns the list of directories actually created."""
        if norm_dir_path == "/":
            return []
        parts = norm_dir_path.split("/")[1:]
        created = []
        cur = ""
        for part in parts:
            cur = cur + "/" + part
            idx, rec = self.find(cur)
            if rec is not None:
                if rec["type"] != FS_TYPE_DIR:
                    raise ValueError(f"{cur} exists and is not a directory")
                continue
            slot = self.find_free_index()
            if slot is None:
                raise RuntimeError("table full (32 entries) -- can't create directory " + cur)
            raw = self.build_record_bytes(cur, True, 0, [0] * FS_N_DIRECT, 0, 0, 0, now, now)
            self.persist_record(slot, raw)
            created.append(cur)
            if log:
                log(f"mkdir  {cur}")
        return created

    # -- file write --

    def write_file(self, norm_path, data, now, force=False, log=None):
        if len(data) > MAX_WRITE_BYTES:
            raise ValueError(
                f"{norm_path} is {len(data)} bytes -- exceeds this tool's "
                f"{MAX_WRITE_BYTES} byte (direct+single-indirect) write limit; "
                f"see this file's WRITE SCOPE note"
            )
        if not self.parent_is_dir(norm_path):
            raise ValueError(f"parent of {norm_path} doesn't exist as a directory -- create it first")

        idx, existing = self.find(norm_path)
        if existing is not None:
            if existing["type"] != FS_TYPE_FILE:
                raise ValueError(f"{norm_path} already exists and is a directory")
            if not force:
                raise ValueError(f"{norm_path} already exists (pass force=True/--force to overwrite)")
            self.free_all_blocks(existing)
            created = existing["created"] or now
        else:
            idx = self.find_free_index()
            if idx is None:
                raise RuntimeError("table full (32 entries)")
            created = now

        n_blocks = (len(data) + BLOCK - 1) // BLOCK if data else 0
        direct = [0] * FS_N_DIRECT
        single_ptrs = []
        for i in range(n_blocks):
            b = self.alloc_block()
            chunk = data[i * BLOCK:(i + 1) * BLOCK]
            if len(chunk) < BLOCK:
                chunk = chunk + bytes(BLOCK - len(chunk))
            self.write_block(b, chunk)
            if i < FS_N_DIRECT:
                direct[i] = b
            else:
                single_ptrs.append(b)

        single_indirect = 0
        if single_ptrs:
            single_indirect = self.alloc_block()
            ptrs = single_ptrs + [0] * (PTRS_PER_BLOCK - len(single_ptrs))
            self.write_block(single_indirect, struct.pack(f"<{PTRS_PER_BLOCK}I", *ptrs))

        raw = self.build_record_bytes(norm_path, False, len(data), direct, single_indirect, 0, 0, created, now)
        self.persist_record(idx, raw)
        if log:
            verb = "overwrote" if existing is not None else "wrote"
            log(f"{verb}  {norm_path}  ({len(data)} bytes)")

    # -- file read --

    def block_for_index(self, rec, index):
        if index < FS_N_DIRECT:
            return rec["direct"][index]
        index -= FS_N_DIRECT
        single_cap = PTRS_PER_BLOCK
        double_cap = PTRS_PER_BLOCK * PTRS_PER_BLOCK

        def walk(top, depth, idx):
            if top == 0:
                return 0
            cur = top
            remaining = idx
            for level in range(depth, 0, -1):
                child_capacity = PTRS_PER_BLOCK ** (level - 1)
                slot_i = remaining // child_capacity
                remaining = remaining % child_capacity
                ptrs = struct.unpack(f"<{PTRS_PER_BLOCK}I", self.read_block(cur))
                child = ptrs[slot_i]
                if level == 1:
                    return child
                if child == 0:
                    return 0
                cur = child
            return 0

        if index < single_cap:
            return walk(rec["single_indirect"], 1, index)
        index -= single_cap
        if index < double_cap:
            return walk(rec["double_indirect"], 2, index)
        index -= double_cap
        return walk(rec["triple_indirect"], 3, index)

    def read_file_data(self, rec):
        size = rec["size"]
        out = bytearray()
        n_blocks = (size + BLOCK - 1) // BLOCK
        for i in range(n_blocks):
            blk = self.block_for_index(rec, i)
            out.extend(self.read_block(blk))
        return bytes(out[:size])

    # -- listing --

    def list_dir(self, norm_dir):
        """Direct children only -- same semantics as fs_list(): every
        used entry whose path starts with norm_dir + '/' and has no
        further '/' after that prefix."""
        if not self.is_dir(norm_dir) and norm_dir != "/":
            raise ValueError(f"{norm_dir} is not a directory")
        prefix = "" if norm_dir == "/" else norm_dir
        out = []
        for r in self.records:
            if not r["used"] or r["path"] == norm_dir:
                continue
            if not r["path"].startswith(prefix + "/"):
                continue
            rest = r["path"][len(prefix) + 1:]
            if "/" in rest:
                continue
            out.append(r)
        return out


# ---- format (initialize a blank/foreign image as an empty TFS2 v3 image) ----

def format_image(path, dry_run=False, force=False, log=None):
    """Mirrors tfs.c's tfs_init() format-fresh path exactly: superblock,
    a cleared journal header, a bitmap with every reserved metadata
    block pre-marked allocated, and FS_MAX_FILES blank (used=0) table
    records written through the same journal stage-commit-apply-clear
    sequence real records use. Grows `path` to fit the metadata region
    if it's smaller (mirrors the Makefile's own $(DISK_IMG) -- a sparse
    file only costs real disk space for the parts actually written).

    Refuses to touch an image that already has a valid TFS2 v3
    superblock unless `force` is set -- this is the one operation in
    this tool capable of discarding a whole filesystem's worth of data
    at once, so it gets its own explicit guard on top of the general
    in-place-writes-are-real caution the rest of this file carries.
    Returns True if it formatted (or would have, under --dry-run),
    False if it left an already-valid image alone.
    """
    with open(path, "r+b") as f:
        f.seek(0, os.SEEK_END)
        if f.tell() < DATA_START_LBA_RAW * SECTOR:
            f.truncate(DATA_START_LBA_RAW * SECTOR)

        f.seek(SUPERBLOCK_LBA * SECTOR)
        sb = f.read(SECTOR)
        already_valid = len(sb) >= 5 and sb[0:4] == b"TFS2" and sb[4] == FS_DISK_VERSION
        if already_valid and not force:
            if log:
                log(f"{path} is already a valid TFS2 v3 image -- leaving it alone (pass --force to wipe it)")
            return False
        if already_valid and force and log:
            log(f"WARNING: {path} already has a valid TFS2 filesystem -- --force passed, wiping it")
        # An image carrying an OLDER TFS2 version is "foreign" by the
        # same rule the kernel applies, so it gets reformatted here
        # without --force -- but say so plainly rather than silently
        # discarding someone's files. This is the expected path exactly
        # once per format change (v2 -> v3 moved the record table).
        if not already_valid and len(sb) >= 5 and sb[0:4] == b"TFS2" and sb[4] != FS_DISK_VERSION and log:
            log(f"WARNING: {path} is a TFS2 v{sb[4]} image and this tool writes "
                f"v{FS_DISK_VERSION} -- reformatting it (its files are lost; the kernel "
                f"would do the same on next boot)")

        def write_sector(lba, data):
            assert len(data) == SECTOR
            if dry_run:
                return
            f.seek(lba * SECTOR)
            f.write(data)

        sb_buf = bytearray(SECTOR)
        sb_buf[0:4] = b"TFS2"
        sb_buf[4] = FS_DISK_VERSION
        write_sector(SUPERBLOCK_LBA, bytes(sb_buf))
        write_sector(JOURNAL_HEADER_LBA, build_journal_header(0, 0, 0))

        bitmap = bytearray(BITMAP_BYTES)
        for b in range(DATA_START_BLOCK):
            bitmap[b // 8] |= 1 << (b % 8)
        for s in range(BITMAP_SECTORS):
            off = s * SECTOR
            write_sector(BITMAP_START_LBA + s, bytes(bitmap[off:off + SECTOR]))

        blank = bytes(RECORD_BYTES)  # all-zero -> used == 0
        checksum = fnv1a(blank)
        for i in range(FS_MAX_FILES):
            write_sector(JOURNAL_HEADER_LBA, build_journal_header(1, i, checksum))
            write_sector(JOURNAL_DATA_LBA, blank)
            write_sector(TABLE_START_LBA + i, blank)
            write_sector(JOURNAL_HEADER_LBA, build_journal_header(0, 0, 0))

    if log:
        log(f"formatted {path} as an empty TFS2 v3 image ({FS_MAX_FILES} free slots)" + (" (dry run)" if dry_run else ""))
    return True


# ---- content hashing (for sync's change detection) ----

def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


# ---- subcommands ----

def cmd_format(args):
    format_image(args.disk, dry_run=args.dry_run, force=args.force, log=print)


def cmd_write(args):
    format_image(args.disk, dry_run=args.dry_run, force=False, log=print)
    with Image(args.disk, dry_run=args.dry_run) as img:
        norm = normalize(args.tfs_path)
        with open(args.local_file, "rb") as f:
            data = f.read()
        now = datetime.now()
        img.ensure_dir_chain(path_parent(norm), now, log=print)
        img.write_file(norm, data, now, force=args.force, log=print)
        if args.dry_run:
            print("(dry run -- nothing written)")


def cmd_read(args):
    with Image(args.disk, dry_run=True) as img:
        norm = normalize(args.tfs_path)
        _, rec = img.find(norm)
        if rec is None:
            print(f"no such file: {norm}", file=sys.stderr)
            sys.exit(1)
        if rec["type"] != FS_TYPE_FILE:
            print(f"{norm} is a directory, not a file", file=sys.stderr)
            sys.exit(1)
        data = img.read_file_data(rec)
        if args.out:
            with open(args.out, "wb") as f:
                f.write(data)
            print(f"wrote {len(data)} bytes to {args.out}")
        else:
            sys.stdout.buffer.write(data)


def cmd_ls(args):
    with Image(args.disk, dry_run=True) as img:
        norm = normalize(args.tfs_path)
        entries = sorted(img.list_dir(norm), key=lambda r: r["path"])
        if not entries:
            print(f"(empty: {norm})")
            return
        for r in entries:
            kind = "DIR " if r["type"] == FS_TYPE_DIR else "FILE"
            size = "" if r["type"] == FS_TYPE_DIR else f"  {r['size']:>10} B"
            print(
                f"{kind}  {r['path']:<40}{size}  "
                f"created={r['created']}  modified={r['modified']}"
            )


def cmd_sync(args):
    """Mirrors a seed directory into the image.

    Seed directory convention:
      <seed-dir>/once/...   copy-once files -- written if missing on
                             the image, never touched again once
                             present (config files, anything the OS or
                             a user is expected to be free to edit
                             after first boot).
      <seed-dir>/sync/...   content-hash-synced files -- written if
                             missing, REWRITTEN if the local file's
                             content differs from what's on the image,
                             left alone if identical (binaries/assets
                             you rebuild and want kept current).

    Either subdirectory may be absent. A file's TFS destination path is
    --dest joined with its path relative to once/ or sync/ -- e.g.
    <seed-dir>/sync/bin/lspci -> <dest>/bin/lspci (dest defaults to "/").

    Auto-formats `disk.img` first if it isn't already a valid TFS2 v3
    image (a no-op if it already is) -- this is what lets a completely
    blank, freshly-truncated disk.img be seeded in one call, e.g. from
    the Makefile, without a separate `format` step or a toy-os boot in
    between.
    """
    format_image(args.disk, dry_run=args.dry_run, force=False, log=print)
    with Image(args.disk, dry_run=args.dry_run) as img:
        now = datetime.now()
        dest_root = normalize(args.dest)
        n_written = n_skipped = n_unchanged = 0

        for policy, subdir in (("once", "once"), ("sync", "sync")):
            root = os.path.join(args.seed_dir, subdir)
            if not os.path.isdir(root):
                continue
            for dirpath, _dirnames, filenames in os.walk(root):
                for name in sorted(filenames):
                    local_path = os.path.join(dirpath, name)
                    rel = os.path.relpath(local_path, root)
                    dest_path = normalize(
                        (dest_root.rstrip("/") + "/" + rel.replace(os.sep, "/"))
                        if dest_root != "/"
                        else "/" + rel.replace(os.sep, "/")
                    )
                    dest_dir = path_parent(dest_path)
                    img.ensure_dir_chain(dest_dir, now, log=print)

                    with open(local_path, "rb") as f:
                        data = f.read()

                    _, existing = img.find(dest_path)
                    if policy == "once":
                        if existing is not None:
                            n_skipped += 1
                            continue
                        img.write_file(dest_path, data, now, force=False, log=print)
                        n_written += 1
                    else:  # sync
                        if existing is not None and existing["type"] == FS_TYPE_FILE:
                            on_disk_hash = sha256_bytes(img.read_file_data(existing))
                            if on_disk_hash == sha256_bytes(data):
                                n_unchanged += 1
                                continue
                            img.write_file(dest_path, data, now, force=True, log=print)
                            n_written += 1
                        else:
                            img.write_file(dest_path, data, now, force=False, log=print)
                            n_written += 1

        print(
            f"sync done: {n_written} written, {n_skipped} skipped (once, already present), "
            f"{n_unchanged} unchanged (sync, same content)"
            + (" [dry run -- nothing actually written]" if args.dry_run else "")
        )


# ---- corrupt (fault injection, for testing the kernel's `fsck`) ----


def cmd_corrupt(args):
    """Injects a specific, known inconsistency into an image so the
    kernel's `fsck` can be tested against a disk whose exact damage is
    known in advance.

    This exists because the inconsistencies `fsck` repairs are ones the
    kernel goes out of its way NOT to produce -- a leaked block needs an
    operation interrupted between persisting a record and updating the
    bitmap, which can't be triggered on demand from inside a running
    toy-os. Without a way to manufacture them, `fsck` could only ever be
    tested against a clean disk, which proves it reports "clean" and
    nothing else.

    Modes (mirroring fs_check()'s three repairable classes):
      --leak N              mark N currently-free data blocks as
                            allocated, referenced by nothing
      --free-referenced N   clear the bitmap bit of N blocks that ARE
                            referenced by a file (the dangerous
                            direction -- the allocator would hand them
                            out a second time)
      --bad-pointer PATH    point PATH's first direct block pointer at a
                            block number past the end of the disk

    Every mode prints exactly what it changed, so a test can assert the
    kernel's own counts match.
    """
    with Image(args.disk, dry_run=args.dry_run) as img:
        if args.leak:
            made = []
            b = DATA_START_BLOCK
            while len(made) < args.leak and b < FS_DISK_TOTAL_BLOCKS:
                if not img._bit(b):
                    img._set_bit(b, 1)
                    img._persist_bitmap_bit(b)
                    made.append(b)
                b += 1
            print(f"leaked {len(made)} block(s): {made[:8]}{' ...' if len(made) > 8 else ''}")

        if args.free_referenced:
            refs = []
            for r in img.records:
                if not r["used"]:
                    continue
                for blk in r["direct"]:
                    if blk and len(refs) < args.free_referenced:
                        refs.append((r["path"], blk))
            for path, blk in refs:
                img._set_bit(blk, 0)
                img._persist_bitmap_bit(blk)
            print(f"marked {len(refs)} referenced block(s) free: {refs}")

        if args.stage_journal:
            # Leaves the image in the exact state a crash between "journal
            # entry committed" and "table slot written" produces: a
            # commit=1 header describing a record that is NOT yet in its
            # table slot. A correct kernel replays it on the next mount;
            # `--stage-journal-torn` corrupts the staged bytes afterward so
            # the checksum fails and the entry must be DISCARDED instead.
            # This is the only way to exercise replay_journal() without an
            # actual power loss mid-write.
            norm = normalize(args.stage_journal)
            slot = img.find_free_index()
            if slot is None:
                raise SystemExit("no free table slot to stage into")
            now = datetime.now()
            rec = img.build_record_bytes(norm, False, 0, [], 0, 0, 0, now, now)
            checksum = fnv1a(rec)
            staged = rec
            if args.stage_journal_torn:
                torn = bytearray(rec)
                torn[0] ^= 0xFF  # same length, different bytes -> checksum must fail
                staged = bytes(torn)
            img.write_sector(JOURNAL_DATA_LBA, staged)
            img.write_sector(JOURNAL_HEADER_LBA, build_journal_header(1, slot, checksum))
            kind = "TORN (checksum will not match)" if args.stage_journal_torn else "valid"
            print(f"staged a {kind} journal entry for {norm} into slot {slot}, "
                  f"table slot left unwritten -- boot toy-os to see it "
                  f"{'discarded' if args.stage_journal_torn else 'replayed'}")

        if args.bad_pointer:
            norm = normalize(args.bad_pointer)
            idx, rec = img.find(norm)
            if idx is None:
                raise SystemExit(f"{norm} not found in {args.disk}")
            buf = bytearray(img.read_sector(TABLE_START_LBA + idx))
            bogus = FS_DISK_TOTAL_BLOCKS + 1234
            struct.pack_into("<I", buf, REC_OFF_DIRECT, bogus)
            img.persist_record(idx, bytes(buf))
            print(f"pointed {norm}'s first direct pointer at block {bogus} (past end of disk)")


def main():
    p = argparse.ArgumentParser(description="Host-side TFS2 v3 read/write tool for toy-os disk images")
    sub = p.add_subparsers(dest="command", required=True)

    p_format = sub.add_parser("format", help="initialize a blank/foreign image as an empty TFS2 v3 image")
    p_format.add_argument("disk")
    p_format.add_argument("--force", action="store_true", help="wipe an already-valid TFS2 image too")
    p_format.add_argument("--dry-run", action="store_true", help="preview without writing")
    p_format.set_defaults(func=cmd_format)

    p_write = sub.add_parser("write", help="write one local file into the image")
    p_write.add_argument("disk")
    p_write.add_argument("tfs_path", help="destination path on the image, e.g. /bin/lspci")
    p_write.add_argument("local_file")
    p_write.add_argument("--force", action="store_true", help="overwrite if the path already exists")
    p_write.add_argument("--dry-run", action="store_true", help="preview without writing")
    p_write.set_defaults(func=cmd_write)

    p_read = sub.add_parser("read", help="read one file out of the image")
    p_read.add_argument("disk")
    p_read.add_argument("tfs_path")
    p_read.add_argument("-o", "--out", help="write to this local file instead of stdout")
    p_read.set_defaults(func=cmd_read)

    p_ls = sub.add_parser("ls", help="list a directory's direct children")
    p_ls.add_argument("disk")
    p_ls.add_argument("tfs_path", nargs="?", default="/")
    p_ls.set_defaults(func=cmd_ls)

    p_sync = sub.add_parser("sync", help="mirror a seed directory's once/ and sync/ trees into the image")
    p_sync.add_argument("disk")
    p_sync.add_argument("seed_dir", help="directory containing once/ and/or sync/ subtrees")
    p_sync.add_argument("--dest", default="/", help="TFS destination root (default: /)")
    p_sync.add_argument("--dry-run", action="store_true", help="preview without writing")
    p_sync.set_defaults(func=cmd_sync)

    p_corrupt = sub.add_parser(
        "corrupt", help="inject a known inconsistency (for testing the kernel's `fsck`)"
    )
    p_corrupt.add_argument("disk")
    p_corrupt.add_argument("--leak", type=int, default=0,
                            help="mark N free data blocks as allocated, referenced by nothing")
    p_corrupt.add_argument("--free-referenced", type=int, default=0,
                            help="clear the bitmap bit of N blocks a file actually references")
    p_corrupt.add_argument("--bad-pointer", metavar="PATH",
                            help="point PATH's first direct pointer past the end of the disk")
    p_corrupt.add_argument("--stage-journal", metavar="PATH",
                            help="leave a committed journal entry for PATH with its table slot "
                                 "unwritten (the state a crash mid-persist_record() produces)")
    p_corrupt.add_argument("--stage-journal-torn", action="store_true",
                            help="with --stage-journal: corrupt the staged bytes so replay must "
                                 "discard the entry instead of applying it")
    p_corrupt.add_argument("--dry-run", action="store_true")
    p_corrupt.set_defaults(func=cmd_corrupt)

    args = p.parse_args()
    try:
        args.func(args)
    except (ValueError, RuntimeError) as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
