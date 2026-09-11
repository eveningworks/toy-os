# TFS2 on-disk format (v3 -- block-addressed)

> **TFS2 IS NO LONGER IMPLEMENTED, AND NOTHING GUARDS AGAINST IT.** The
> kernel backend (`kernel/fs/tfs.c`), the host tool
> (`tools/tfs2_writer.py`) and the recognise-and-refuse check that briefly
> replaced them are all gone; TFS3 is the only filesystem and FAT32 is the
> next one.
>
> **A TFS2 disk booted now is treated as blank and REFORMATTED.** That is
> a deliberate choice made on the basis that no such disks exist — see
> `docs/decisions.md`. If you have one, do not boot it; read it with
> `tools/tfs2_writer.py` from a commit before the removal.
>
> This page is kept because a byte-level description of a **frozen**
> format cannot go stale, and several `docs/decisions/` entries reason
> from it — TFS3's 32 KiB front reservation exists *because* TFS2's
> superblock collided with the MBR.

**Status: the LEGACY of toy-os's two filesystems.** Since Milestone 15
(2026-08-14), TFS3 (`docs/tfs3-spec.md`) is the default for
fresh/blank images; TFS2 remains fully supported as a second backend
-- the kernel probes both magics and mounts whatever a disk actually
carries, and `fsformat tfs2 confirm` creates a fresh TFS2 on purpose.
This spec is unchanged by any of that: the format itself did not move
a byte.

This is a byte-exact specification of TFS2 for writing an independent
(read-only, ideally) tool on a
host machine that can open a `disk.img`/`toy-os.iso`-adjacent raw disk
image and browse its contents without booting toy-os at all.

**This describes on-disk version 3.** The block-addressed,
indirect-pointer layout (12 direct block pointers + single/double/
triple indirect, classic Unix-inode shape) is unchanged from version 2;
what v3 changes is `FS_MAX_FILES`, 32 -> 256, which moves the free-block
bitmap and everything after it. Every byte-level record/journal/bitmap
structure below is identical between v2 and v3 -- only the region
offsets differ, and only because that one constant did.

Older versions are **gone**: v1 (the original journaled/timestamped
format, with each file's data inlined in its table record, capped at
`FS_DATA_MAX` = 2048 bytes) and v2 (same layout as here, 32 slots).
toy-os detects any version mismatch and treats the disk as unclaimed;
since the multi-backend VFS landed, an unclaimed-but-readable disk is
formatted with the DEFAULT filesystem (TFS3, not TFS2 -- see
`kernel/fs/vfs.c`'s probe loop), and an unreadable superblock is
refused outright. A reader built against this document
cannot open a v1 or v2 image either, and shouldn't try to guess.

This document describes the format only -- not why it's shaped this
way. For that, see `kernel/fs/tfs.c`'s top comment (the reference
implementation) and `docs/decisions.md`'s entries on journaling/
timestamps/indirect-block design choices.

**No backward compatibility** is provided or intended between format
versions. A TFS2-aware reader should check the superblock magic+version
(below) and refuse/bail on anything else rather than guess.

## Conventions

- All multi-byte integers are **little-endian**.
- All sector I/O is in fixed **512-byte sectors** (standard ATA/IDE
  sector size -- `ATA_SECTOR_SIZE` in `kernel/include/api/ata.h`). File
  *data*, however, is addressed in **4096-byte blocks**
  (`FS_BLOCK_SIZE`, 8 sectors each) -- see "Block addressing" below.
  LBA numbers are absolute sector indices from the start of the disk
  image (LBA 0 = the image's first 512 bytes); block numbers are
  separate, absolute 4096-byte-block indices, also from the start of
  the disk image.
- Fixed-size string fields are C-style: bytes up to (and including) the
  first `\0`, or the field's full width if there's no terminator; only
  bytes before the first `\0` are meaningful. This spec doesn't rely on
  trailing bytes past a string's terminator being any particular
  value -- treat them as unspecified padding, not as data.
- `FS_MAX_FILES` (256) and `FS_PATH_MAX` (64) are compile-time constants
  in this kernel (`kernel/include/api/fs.h`), not something an on-disk
  header records anywhere. (Since TFS3 landed, `FS_MAX_FILES` is
  TFS2's table size only; `FS_PATH_MAX` remains the caller-side path
  buffer limit shared by both backends.) A reader has to know them ahead of time
  (they're listed here) rather than discover them from the image
  itself. A future toy-os build that changes either would also bump
  the superblock version (see below), which is the signal a reader
  should treat as "this exact spec no longer applies."
- The *total disk size* this backend assumes (`FS_DISK_TOTAL_BYTES`,
  currently 9 GiB) is likewise a compile-time constant, not stored
  on-disk -- it determines the free-block bitmap's size and therefore
  where the data region starts (see "Disk layout" below). A reader
  built against a different total size will compute the wrong
  bitmap/data offsets; this spec's numbers below assume the current
  9 GiB constant.

## Disk layout

| Region | LBA (sectors) | Size | Contents |
|---|---|---|---|
| Superblock | 0 | 1 sector (512 B) | magic + version |
| Journal header | 1 | 1 sector (512 B) | pending-mutation metadata |
| Journal data | 2 | 1 sector (512 B) | one record's worth of staged metadata |
| Table | 3 – 258 | 256 sectors (131,072 B) | `FS_MAX_FILES` (256) fixed-size records, 1 sector each |
| Free-block bitmap | 259 – 834 | 576 sectors (294,912 B) | one bit per 4096-byte block of the whole disk |
| (padding to block boundary) | 835 – 839 | 5 sectors | unused, rounds the data region up to a block-aligned LBA |
| File data | 840 onward | rest of the disk | 4096-byte blocks, block-number addressed |

These are the concrete numbers for the current constants
(`FS_MAX_FILES` = 256, `FS_DISK_TOTAL_BYTES` = 9 GiB); see "Deriving
these offsets yourself" below if any of those constants change. The v2
numbers, for comparison, were table 3–34, bitmap 35–610, data from LBA
616 -- the same structures, 224 sectors earlier.

`FS_DISK_TOTAL_BYTES` is a compile-time *maximum*, not a claim about
the image: since the capacity-detection change, toy-os clamps its
usable block count to what the drive actually reports (IDENTIFY words
60-61) at mount. A reader should size the bitmap region from the
constant above (it's a fixed on-disk region either way) but shouldn't
assume the image is 9 GiB.

Unlike v1, there is **no fixed "everything past here is unused, safe
to ignore" boundary** -- the entire disk (all the way out to
`FS_DISK_TOTAL_BYTES`) is potentially file data. `disk.img` is created
as a *sparse* 9 GiB file (`truncate -s 9G`, see the Makefile's
`DISK_IMG` rule) specifically so an unwritten disk costs no real space
on the host -- most of it reads as zeros and occupies no blocks on the
host filesystem until toy-os actually writes there.

### Deriving these offsets yourself

If `FS_MAX_FILES` or `FS_DISK_TOTAL_BYTES` ever change, recompute from
`kernel/fs/tfs.c`'s own macros rather than trusting the table
above verbatim:

```
FS_RECORD_SECTORS   = 1                          (see "Table records" below -- 148 raw bytes, rounds up to 1 sector)
FS_TABLE_START_LBA  = 3                          (superblock + journal header + journal data)
FS_BITMAP_START_LBA = FS_TABLE_START_LBA + FS_MAX_FILES * FS_RECORD_SECTORS
FS_DISK_TOTAL_BLOCKS= FS_DISK_TOTAL_BYTES / 4096
FS_BITMAP_BYTES     = (FS_DISK_TOTAL_BLOCKS + 7) / 8
FS_BITMAP_SECTORS   = ceil(FS_BITMAP_BYTES / 512)
FS_DATA_START_BLOCK = ceil((FS_BITMAP_START_LBA + FS_BITMAP_SECTORS) / 8)   (rounds up to a whole 4096-byte block)
FS_DATA_START_LBA   = FS_DATA_START_BLOCK * 8
```

`record_lba(i) = FS_TABLE_START_LBA + i * FS_RECORD_SECTORS`.

## Superblock (LBA 0)

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 4 bytes | magic | ASCII `"TFS2"` (`0x54 0x46 0x53 0x32`) |
| 4 | 1 byte | version | `0x03` |
| 5–511 | — | (unused) | zero-filled by this kernel, but a reader shouldn't assume that |

A reader should treat any image whose first 5 bytes don't match
exactly (`"TFS2"` + version `0x03`) as **not a v3 TFS2 image** --
toy-os itself reformats on any mismatch (including a v1 image) rather
than trying to read a foreign/old-version layout, and a host-side
reader should refuse the same way rather than guess at a different
layout.

## Journal header (LBA 1)

Write-ahead log metadata for the single mutation (if any) that was
most recently in flight. See "Journal semantics for readers" below for
what a *browsing* tool should actually do with this region -- short
version: **the table (below) is the authoritative, current state; the
journal only matters to toy-os's own crash recovery on its next real
boot.**

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 4 bytes | magic | ASCII `"JRN1"` (`0x4A 0x52 0x4E 0x31`) -- absent/different if the journal region has never been initialized |
| 4 | 1 byte | commit | `0x00` = empty/no pending entry, `0x01` = a pending entry is described below |
| 5 | 4 bytes (uint32 LE) | target slot | which table slot (0–255) this entry is for -- only meaningful if `commit == 1` |
| 9 | 4 bytes (uint32 LE) | checksum | FNV-1a-32 (see below) of the journal data area's `FS_RECORD_BYTES` (512) bytes -- only meaningful if `commit == 1` |
| 13–511 | — | (unused) | — |

**FNV-1a-32**: `hash = 0x811C9DC5; for each byte b: hash ^= b; hash *=
0x01000193` (32-bit unsigned, wraps on overflow). Not cryptographic --
just enough to detect a torn/partial write with overwhelming
probability, matching `kernel/fs/tfs.c`'s `fnv1a()` (a thin wrapper
over the shared `k_fnv1a()` in `kernel/include/api/string.h` since
TFS3 became the hash's second caller).

**What the journal protects, and what it doesn't**: exactly as
before, `persist_record()` protects one table-slot *record* (path,
type, size, timestamps, and the direct/indirect block **pointers**)
from a torn write. What changed with the block-addressed rework: a
file's actual *data* no longer lives inside the record at all, so the
journal no longer covers it. Block and indirect-block writes go
straight to disk, un-journaled -- a crash mid-write to a large file
can leave a data or indirect block partially written, or a pointer
committed before its target block's content was durable. The
top-level record itself (and therefore a file's size and top block
pointers) still can't end up torn -- just possibly pointing at a block
whose content wasn't the last thing written to it. This is a known,
accepted gap (see `kernel/fs/tfs.c`'s top comment and
`docs/decisions.md`), not something a v1-era reader's assumptions
about journal coverage should be carried over for.

### Journal semantics for readers

toy-os's own write path (`persist_record()` in `tfs.c`) is a 4-step
write-ahead sequence: stage the new record bytes in the journal data
area -> set `commit = 1` (the durability point) -> apply the same
bytes to the real table slot -> clear the journal (`commit = 0`). On
its own next boot, toy-os checks this header before trusting the
table: if `commit == 1` and the journal data's checksum still matches,
it replays those exact bytes into the target slot (safe either way,
since the table write may or may not have already finished before a
crash); if the checksum doesn't match, the journal write itself was
torn, so the table slot was never touched for that mutation and needs
no recovery.

A read-only browsing tool almost certainly does **not** want to
reimplement this recovery logic -- doing so risks writing to the
image (recovery means applying bytes to the table) for a purpose
(browsing) that shouldn't need to. Two reasonable choices, in order of
preference:

1. **Ignore the journal entirely.** Read the table as-is. In the
   overwhelmingly common case (the image came from a clean shutdown),
   `commit` will already be `0` and the table is already fully up to
   date. This is almost certainly the right default for a browsing
   tool.
2. **Surface it as information, without acting on it.** If `commit ==
   1`, tell the user "there's an unapplied pending change for slot N,
   from an unclean shutdown -- boot toy-os once to let it finish
   recovering, then re-open this image" rather than silently guessing
   which of the two possible states (table already updated / table
   still old) is true.

Do not write to the image from a browsing tool to "help" -- that's
squarely toy-os's own job, and a tool that gets the replay logic even
slightly wrong risks corrupting a slot that was otherwise fine.

## Table records (LBA 3 onward)

Each of the 256 slots is `FS_RECORD_SECTORS` = 1 sector = 512 bytes
(only 148 bytes are meaningful; the rest is padding). This is much
smaller than v1's 2560-byte record, because file data is no longer
stored inline -- a record now holds metadata plus block **pointers**,
not the data itself.

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 64 bytes (`FS_PATH_MAX`) | path | C string, absolute (`"/docs/notes.txt"`), NUL-terminated. Meaningless if `used == 0`. |
| 64 | 1 byte | type | `0x00` = file, `0x01` = directory |
| 65 | 1 byte | used | `0x00` = slot is free/deleted, ignore every other field; `0x01` (or, per the C source, any nonzero byte) = slot is in use |
| 66 | 8 bytes (uint64 LE) | size | File content length in bytes. Only meaningful for `type == file`; always 0 for directories. No longer capped at 2048 -- this is the whole point of the block-addressed rework (multi-gigabyte files). |
| 74 | 7 bytes | created | `rtc_time` -- see encoding below |
| 81 | 7 bytes | modified | `rtc_time` -- see encoding below |
| 88 | 48 bytes (12 × uint32 LE) | direct | 12 direct block pointers (`FS_N_DIRECT`). `0` = no block allocated for this position (sparse/not-yet-written, or past EOF). |
| 136 | 4 bytes (uint32 LE) | single_indirect | Block number of a single-indirect block (see below), or `0` if none allocated yet. |
| 140 | 4 bytes (uint32 LE) | double_indirect | Block number of a double-indirect block, or `0`. |
| 144 | 4 bytes (uint32 LE) | triple_indirect | Block number of a triple-indirect block, or `0`. |
| 148–511 | — | (unused) | Padding to the 1-sector/512-byte boundary. |

A slot with `used == 0` should be skipped entirely by a reader -- its
`path`/`type`/`size`/timestamps/pointers are all stale leftovers from
whatever last occupied that slot (deleted files/directories reuse
slots), not a real entry.

### `rtc_time` encoding (7 bytes)

Unchanged from v1. Matches `struct rtc_time` in
`kernel/include/api/timer.h` field-for-field:

| Offset (within the 7 bytes) | Size | Field | Range |
|---|---|---|---|
| 0 | 1 byte | hour | 0–23 |
| 1 | 1 byte | minute | 0–59 |
| 2 | 1 byte | second | 0–59 |
| 3 | 1 byte | day | 1–31 |
| 4 | 1 byte | month | 1–12 |
| 5 | 2 bytes (uint16 LE) | year | full year, e.g. `2026` |

**This is broken-down civil time, not a Unix epoch integer** -- and it
is UTC on anything written since 2026-09-11, when the timezone left the
kernel (`docs/decisions.md`). Records written before that carry the
LOCAL time of whatever city was selected when they were written, with
nothing to say which; the two are indistinguishable in the format.
There's
no timezone/UTC-offset field stored anywhere in the record or the
superblock -- if a browsing tool wants to show these fields
meaningfully alongside a real-world reference, it should just display
the six numbers as-is (a wall-clock date/time) rather than assume any
particular UTC offset.

## Block addressing and the indirect-pointer scheme

File data lives in `FS_BLOCK_SIZE` = 4096-byte blocks, addressed by
absolute block number (block N starts at LBA `N * 8`, since a block is
exactly 8 sectors -- chosen to match `ATA_MAX_SECTORS_PER_XFER`
exactly, one ATA command per block). **Block 0 is reserved** as the
"no block" null-pointer sentinel -- it's never handed out by the
allocator and never holds real data; a `0` pointer anywhere in a
record or an indirect block means "not allocated" (reads as all-zero
bytes), not "block number 0 literally."

Every block from block 0 up to (but not including) `FS_DATA_START_BLOCK`
is permanently marked allocated in the free-block bitmap at format
time, whether or not it's block 0 -- this reserves the LBA range that
actually holds the superblock/journal/table/bitmap regions so the
allocator can never hand one of those blocks out as if it were free
file data.

A file's data blocks are found by *block index* (0-based, "the Nth
4096-byte chunk of this file's content") using the classic Unix-inode
scheme:

- **Indices 0–11** (`FS_N_DIRECT` = 12): read straight from the
  record's own `direct[]` array -- `direct[index]` is the block
  number.
- **Indices 12–1035** (12 + `FS_PTRS_PER_BLOCK`, where
  `FS_PTRS_PER_BLOCK` = 4096 / 4 = 1024 uint32 block numbers per
  index block): read the `single_indirect` block (itself a
  4096-byte block holding 1024 packed uint32 block numbers), then
  index into it at position `(index - 12)`.
- **Indices 1036–1,049,611** (next 1024×1024 = 1,048,576 indices):
  read the `double_indirect` block, index into it to find a
  *single*-indirect block, then index into that to find the leaf
  data block. Two levels of indirection.
- **Beyond that**: the `triple_indirect` block, three levels deep.
  Capacity here is ~1024³ blocks (~4 TB) -- far past this disk's
  real 9 GiB size, so in practice triple-indirect blocks are rarely
  if ever populated on a real toy-os image.

A file's total size in blocks is `ceil(size / 4096)`; the last block
is only partially meaningful (only the first `size % 4096` bytes of
it, or all 4096 if size is an exact multiple). A reader reconstructing
file content should stop at `size` bytes regardless of how many blocks
the pointer chain reaches.

**Reading a byte range**: `fs_read_range(path, offset, length, buf)`
(the kernel's own API for this, in `fs.h`) walks exactly this same
pointer chain, one block at a time, copying only the requested slice
out of each block touched -- a host-side reader doing a partial read
should do the same rather than materializing the whole file. `about`/
`fs_read()` (whole-file convenience wrappers) just call the range API
with `offset = 0, length = size`.

## Free-block bitmap

One bit per 4096-byte block of the *entire* disk (including the
reserved metadata region below `FS_DATA_START_BLOCK` -- see above),
starting at `FS_BITMAP_START_LBA`. Bit `b`'s byte is at
`bitmap[b / 8]`, bit position `b % 8` (LSB-first within the byte); `1`
= allocated, `0` = free. A reader only needs this to answer "how much
free space is left," not to browse existing files (every live file's
blocks are already reachable through its record's pointers) -- it's
not required reading for a basic directory-listing tool.

## Path / directory semantics

Unchanged from v1. There is no separate on-disk directory structure.
Every slot -- file or directory -- is just an entry with a full
absolute path string. Parent/child relationships are derived purely
from path-string prefix matching at read time: `/docs/notes.txt`'s
parent is `/docs`, and `/docs`'s children are every used entry whose
path starts with `/docs/` and has no further `/` after that prefix.
The root directory `/` has **no entry of its own** in the table --
it's implicit and always "exists."

## Reference reader (Python, read-only)

```python
#!/usr/bin/env python3
"""Read-only TFS2 v3 (block-addressed) disk image reader -- reference
implementation for docs/tfs2-spec.md. Prints every used entry as a
path + metadata line, and can dump a file's content by walking its
direct/indirect block pointers.

Usage: python3 tfs2_reader.py disk.img [path/to/dump]
"""
import struct
import sys
from datetime import datetime

SECTOR = 512
BLOCK = 4096
BLOCK_SECTORS = BLOCK // SECTOR          # 8
PTRS_PER_BLOCK = BLOCK // 4              # 1024 uint32 block numbers per indirect block

FS_PATH_MAX = 64
FS_MAX_FILES = 256
FS_N_DIRECT = 12

RECORD_SECTORS = 1
RECORD_BYTES = RECORD_SECTORS * SECTOR
TABLE_START_LBA = 3                       # superblock(1) + journal header(1) + journal data(1)
BITMAP_START_LBA = TABLE_START_LBA + FS_MAX_FILES * RECORD_SECTORS  # 259

REC_OFF_TYPE = FS_PATH_MAX                       # 64
REC_OFF_USED = FS_PATH_MAX + 1                   # 65
REC_OFF_SIZE = FS_PATH_MAX + 2                   # 66, 8 bytes (uint64 LE)
REC_OFF_CREATED = REC_OFF_SIZE + 8               # 74
REC_OFF_MODIFIED = REC_OFF_CREATED + 7           # 81
REC_OFF_DIRECT = REC_OFF_MODIFIED + 7            # 88, 12 * uint32
REC_OFF_SINGLE = REC_OFF_DIRECT + FS_N_DIRECT * 4  # 136
REC_OFF_DOUBLE = REC_OFF_SINGLE + 4              # 140
REC_OFF_TRIPLE = REC_OFF_DOUBLE + 4              # 144


def read_sector(f, lba):
    f.seek(lba * SECTOR)
    return f.read(SECTOR)


def read_block(f, block):
    if block == 0:
        return bytes(BLOCK)  # null pointer -- reads as all-zero
    f.seek(block * BLOCK_SECTORS * SECTOR)
    data = f.read(BLOCK)
    return data if len(data) == BLOCK else data + bytes(BLOCK - len(data))


def check_superblock(f):
    sb = read_sector(f, 0)
    if sb[0:4] != b"TFS2" or sb[4] != 0x03:
        raise ValueError("not a TFS2 v3 image (bad magic/version) -- "
                          "a v1 or v2 image fails this check too, on purpose")


def check_journal(f):
    hdr = read_sector(f, 1)
    if hdr[0:4] != b"JRN1":
        return  # never initialized
    commit = hdr[4]
    if commit:
        slot = struct.unpack_from("<I", hdr, 5)[0]
        print(f"NOTE: pending unapplied journal entry for slot {slot} "
              f"(unclean shutdown) -- boot toy-os once to let it finish "
              f"recovering before trusting this image fully.",
              file=sys.stderr)


def parse_rtc(buf, off):
    hour, minute, second, day, month = buf[off:off + 5]
    year = struct.unpack_from("<H", buf, off + 5)[0]
    try:
        return datetime(year, month, day, hour, minute, second)
    except ValueError:
        return None  # zeroed/garbage slot -- caller already checks `used`


def parse_record(buf):
    path = buf[0:FS_PATH_MAX].split(b"\x00", 1)[0].decode("utf-8", "replace")
    entry_type = buf[REC_OFF_TYPE]
    used = buf[REC_OFF_USED]
    size = struct.unpack_from("<Q", buf, REC_OFF_SIZE)[0]
    created = parse_rtc(buf, REC_OFF_CREATED)
    modified = parse_rtc(buf, REC_OFF_MODIFIED)
    direct = list(struct.unpack_from("<12I", buf, REC_OFF_DIRECT))
    single, double, triple = struct.unpack_from("<3I", buf, REC_OFF_SINGLE)
    return {
        "path": path,
        "is_dir": entry_type == 1,
        "used": bool(used),
        "size": size,
        "created": created,
        "modified": modified,
        "direct": direct,
        "single_indirect": single,
        "double_indirect": double,
        "triple_indirect": triple,
    }


def block_for_index(f, rec, index):
    """Mirrors tfs.c's block_for_index()/walk_indirect(), read-only
    (never allocates -- a missing pointer just means a hole/EOF)."""
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
            ptrs = struct.unpack_from(f"<{PTRS_PER_BLOCK}I", read_block(f, cur))
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


def read_file_data(f, rec):
    """Reconstructs a file's full content by walking its block chain."""
    size = rec["size"]
    out = bytearray()
    n_blocks = (size + BLOCK - 1) // BLOCK
    for i in range(n_blocks):
        blk = block_for_index(f, rec, i)
        out.extend(read_block(f, blk))
    return bytes(out[:size])


def walk_entries(f):
    for i in range(FS_MAX_FILES):
        lba = TABLE_START_LBA + i * RECORD_SECTORS
        f.seek(lba * SECTOR)
        buf = f.read(RECORD_BYTES)
        rec = parse_record(buf)
        if rec["used"]:
            yield rec


def main():
    if len(sys.argv) not in (2, 3):
        print(f"usage: {sys.argv[0]} disk.img [path/to/dump]", file=sys.stderr)
        sys.exit(1)
    with open(sys.argv[1], "rb") as f:
        check_superblock(f)
        check_journal(f)
        entries = {rec["path"]: rec for rec in walk_entries(f)}
        if len(sys.argv) == 3:
            target = sys.argv[2]
            if target not in entries or entries[target]["is_dir"]:
                print(f"no such file: {target}", file=sys.stderr)
                sys.exit(1)
            sys.stdout.buffer.write(read_file_data(f, entries[target]))
            return
        for rec in sorted(entries.values(), key=lambda r: r["path"]):
            kind = "DIR " if rec["is_dir"] else "FILE"
            size = "" if rec["is_dir"] else f"  {rec['size']:>10} B"
            print(f"{kind}  {rec['path']:<40}{size}  "
                  f"created={rec['created']}  modified={rec['modified']}")


if __name__ == "__main__":
    main()
```

Save this as e.g. `tfs2_reader.py` and run it against a `disk.img`
copied off toy-os's disk image -- it needs no toy-os build tooling,
just Python 3's standard library. Run with no third argument to list
every entry; pass a path as a second argument to dump that file's
content to stdout.

## Writing to a TFS2 image from a host tool

The tool exists: `tools/tfs2_writer.py` (TFS2 images only -- the TFS3
equivalent is `tools/tfs3_writer.py`, and `tools/seed_disk.py` picks
between them by magic at build time). Hand-rolling a second writer is
not covered by this spec in detail, and not recommended as a first
step -- the journal's write-ahead sequence (see above) has to be
followed exactly (stage -> commit -> apply -> clear) for the *record*
to be crash-safe, and getting it wrong risks corrupting the image in a
way that's hard to distinguish from a real toy-os bug when it's next
booted. Writing file *data* correctly additionally means allocating
blocks (updating the bitmap) and threading indirect-block pointers
through the record -- meaningfully more bookkeeping than v1's single
inline blob was. A browsing tool that also wants to *edit* files
should strongly consider driving toy-os itself (e.g. via the same
QMP/serial automation `tools/qmp_test.py` uses for testing) rather
than reimplementing the write path independently, at least until
there's a concrete need that's worth the risk.
