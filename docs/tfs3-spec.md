# TFS3 on-disk format (v1 -- block groups + inodes)

What is actually on a TFS3 disk, byte by byte, as shipped in
`kernel/fs/tfs3.c` and mirrored by `tools/tfs3_writer.py`. This is the
sibling of `docs/tfs2-spec.md` and follows its conventions; the design
history and the reasoning behind every choice live in
`docs/tfs3-design.md` and the CHANGELOG -- this file documents the
format only.

No backward compatibility: a reader must check magic + version and
bail on anything unexpected. A version bump means reformat, per the
project's standing policy. TFS2 remains a separate, coexisting format
(`kernel/fs/tfs.c`); which one a disk carries is decided by probing
both magics -- see "Coexistence with TFS2" below.

## Conventions

- All multi-byte integers are **little-endian**.
- Structures are **hand-serialized field by field** at the offsets in
  each table -- there are no C structs on disk, so compiler
  alignment/padding never applies.
- Disk I/O is in 512-byte sectors; addressing is in 4096-byte blocks
  (8 sectors). **Every block number is VOLUME-relative**: block `b`
  starts at sector `volume_base_lba + b * 8`. Today the only volume
  is the flat disk (`base = 0`); the format is identical inside a
  partition (see the design doc's "Volumes and partitions").
- All checksums are FNV-1a-32 (offset basis `0x811C9DC5`, prime
  `0x01000193`) -- `k_fnv1a()` kernel-side, `fnv1a()` in both writer
  tools.
- Reserved bytes are written as zero and ignored on read.
- Bitmap bit `i` lives in byte `i >> 3`, mask `1 << (i & 7)` (LSB
  first).

## Disk layout

There are two versions of this layout. They differ ONLY in the size of
the journal region and therefore in where everything after it starts.

**v2** (current; what `format` writes):

| Region | Blocks | Contents |
|---|---|---|
| reserved | 0-7 | never touched (32 KiB for MBR/GPT on a flat disk) |
| superblock | 8 | first sector meaningful |
| journal header | 9 | first sector meaningful |
| journal images | 10-41 | 32 staged block images (one per slot) |
| group descriptors | 42-57 | fixed 16 blocks = 4096 slots of 16 bytes |
| block groups | 58... | group 0, group 1, ... to end of volume |

**v1** (Milestone 15; still mounted read/write, never written fresh):

| Region | Blocks | Contents |
|---|---|---|
| journal images | 10-13 | four staged block images |
| group descriptors | 14-29 | as above |
| block groups | 30... | as above |

Each version's numbers are CONSTANTS, not parameters. That is what
keeps the geometry derivable with no superblock in hand -- the point of
the fixed-size descriptor table -- because a reader whose primary
superblock is unreadable has only two candidate values of
`group0_start` to try:

```
group0_start = 58 (v2) or 30 (v1)       (constants)
blocks_per_group = 32768                (constant, one 4 KiB bitmap)
group_count = (volume_blocks - group0_start) / 32768   (floor; trailing
                                                        partial group unused)
group g = blocks [group0_start + g*32768, group0_start + (g+1)*32768)
```

`tools/tfs3_writer.py format --fs-version {1,2}` writes either layout
-- v2 by default, v1 so that the layout the kernel still mounts stays
producible and therefore testable (`tools/tfs3_v1_test.py`).

A reformat must erase the OTHER version's backup superblock sectors,
for the same reason the wipefs rule exists between filesystems: a
stale backup at a position the new layout never writes will claim the
disk with the wrong geometry the first time a primary goes bad.

## Superblock (block 8, first sector)

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | magic | `"TFS3"` |
| 4 | 1 | version | 2 (v1 images still mount) |
| 5 | 1 | flags | bit 0 reserves a per-group data-checksum table (never set by current tools; the kernel REFUSES to mount unknown flag bits rather than rot a feature it doesn't implement) |
| 6 | 2 | reserved | |
| 8 | 4 | total_blocks | volume size in blocks |
| 12 | 4 | blocks_per_group | 32768; a reader rejects anything else |
| 16 | 4 | inodes_per_group | multiple of 32 (whole table blocks); 8192 at the default 16 KiB-per-inode format ratio |
| 20 | 4 | group_count | |
| 24 | 4 | group0_start | 58 on v2, 30 on v1; validated against the version's constant |
| 28 | 4 | journal_start | v2 only: 10 |
| 32 | 4 | journal_blocks | v2 only: 32 |
| 36 | 4 | gdt_start | v2 only: 42 |
| 40 | 4 | reserved | |
| 44 | 4 | checksum | FNV-1a over bytes 0-43 |

The three v2 offsets are the version's own layout written down. A
reader VALIDATES them against the constants above and rejects a
superblock that disagrees; it never believes them on their own, since
the layout is what makes the backups findable when this sector is the
thing that has gone bad.

### Superblock backups

The superblock is write-once after format, so byte-identical copies
(same checksum) live in the LAST block, first sector, of the backup
groups:

```
backup groups: gc == 1 -> [0]; gc == 2 -> [1]; else [1, gc-1]
backup_sb(g) = block 30 + (g+1)*32768 - 1
```

The 16 blocks before each copy hold a format-time snapshot of the
group-descriptor table (free counts stale by design; `fsck`
recomputes). A reader whose primary fails should try the backups --
their positions derive from the volume size alone. The kernel mounts
from a backup loudly and repairs the primary only on `fsck repair`.

**The wipefs rule:** reformatting with a DIFFERENT filesystem must
erase the primary AND backup superblock sectors, or the stale backups
keep claiming the disk (`fs_ops.wipe()` kernel-side; the writer tools
clear the other format's magic).

## Group descriptor (16 bytes; slot g at block 14, offset g*16)

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | free_blocks | cache -- bitmaps are the authority |
| 4 | 4 | free_inodes | cache |
| 8 | 4 | reserved | |
| 12 | 4 | checksum | FNV-1a over bytes 0-11 |

A descriptor failing its checksum degrades that group's counts to
zero until `fsck` recomputes; it never blocks a mount.

## Inside a group (fixed layout, all derivable)

```
block G       block bitmap   (bit i = block G+i; metadata + backup
                              blocks pre-set at format)
block G+1     inode bitmap   (bit i = inode g*ipg + i)
[cksum table  only if flags bit 0 -- 32 blocks; never present today]
inode table   ipg * 128 bytes (256 blocks at ipg=8192)
data blocks   the rest; backup groups lose their trailing 17 blocks
```

## Inode (128 bytes, 4 per sector)

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 1 | type | 0 file, 1 dir, 2 symlink (format-reserved), 3-5 reserved |
| 1 | 1 | reserved | |
| 2 | 2 | links | name count; blocks freed at 0. Dirs: 2 + subdirs |
| 4 | 8 | size | bytes |
| 12 | 8 | created | epoch seconds, LOCAL-derived (see tz.h's tz_rtc_to_epoch) |
| 20 | 8 | modified | epoch seconds |
| 28 | 48 | direct[12] | block numbers; 0 = hole (reads as zeros) |
| 76 | 4 | single_indirect | block of 1024 u32 pointers |
| 80 | 4 | double_indirect | |
| 84 | 4 | triple_indirect | |
| 88 | 4 | checksum | FNV-1a over bytes 0-87 ++ 92-127; verified on every read |
| 92 | 4 | reserved | earmarked uid u16 + mode u16 (Milestone 17) |
| 96 | 32 | reserved | |

Inode 0 is the null inode (never allocated; a dirent with ino 0 is a
free slot). Inode 1 is the root directory. An unallocated inode's
table slot is all zeros, which fails the checksum by design -- the
inode bitmap is the single allocation authority.

Fast symlinks (type 2, format-reserved, unimplemented): target string
stored inline in bytes 28-87 when `size <= 60`, one data block
otherwise.

## Directory entries (inside a dir inode's data blocks)

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | inode | 0 = free slot |
| 4 | 2 | rec_len | bytes this entry spans; multiple of 4; >= 8 |
| 6 | 1 | name_len | 1-255 |
| 7 | name_len | name | raw bytes, not NUL-terminated |

Entries chain through each 4096-byte block (`off += rec_len`; the
last entry's rec_len reaches the block end; entries never span
blocks). Insertion splits a slot's slack; deletion folds an entry into
its predecessor's rec_len (or zeroes the inode field for a
block-leading entry). Every directory has real `.` and `..` entries;
the root's `..` points at itself. A `rec_len < 8` or one that runs
past the block marks a corrupt chain -- stop, don't loop.

## The journal (blocks 9-41 on v2, 9-13 on v1)

One fixed-size transaction of up to `journal_blocks` metadata block
images -- 32 on v2, 4 on v1. What goes through it: **dirent blocks and inode-table blocks only** -- the
structures whose torn write is namespace corruption. Bitmaps, group
descriptors, data blocks and indirect-pointer blocks are deliberately
NOT journaled: allocation state follows the set-before-use /
clear-after-persist ordering, so a crash costs a leaked block that
`fsck` reclaims, never a double allocation (TFS2's rule, inherited).

Header (block 9, first sector):

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | magic | `"JRN3"` |
| 4 | 1 | commit | nonzero = transaction pending |
| 5 | 1 | count | 1..journal_blocks |
| 6 | 2 | reserved | |
| 8 | 4 | seq | monotonic, diagnostic |
| 16 | 8*n | slots[] | per slot: target block u32, FNV-1a of the image u32 |
| 508 | 4 | checksum | FNV-1a over bytes 0-507 |

The slot table and the checksum MOVED in v2: four v1 entries at offset
12 end exactly at byte 44, which is where v1 put its checksum, so 32 of
them would have overwritten it. v1's header is unchanged (slots at 12,
checksum at 44 over bytes 0-43) and is read that way on a v1 image.

Images live in blocks 10..10+count-1 in slot order. Write discipline
(two barriers, `persist_record()`'s generalization -- see tfs.c for why
two is the minimum): stage images + committed header, FLUSH, write
targets, FLUSH, clear commit (no barrier).

A writer RESERVES its slot count before staging anything (`txn_begin()`
kernel-side, jbd2's credit discipline in miniature), so an operation
too big for the volume's journal is refused before it has changed
anything. That is how a v1 image behaves correctly rather than
half-completing: moving a directory between parents needs five blocks
(both dirent blocks, the child's `..`, both parents' link counts) and
is refused there, with every other operation unaffected.

### Journal semantics for readers

A host tool reading a TFS3 image can meet a committed header. Two
reasonable choices, same as tfs2-spec's: (1) ignore it and read the
possibly-pre-transaction state, or (2) verify each image's checksum
and, if ALL match, prefer the staged images for those blocks. Never
write to the image to "replay" -- that is the kernel's job at mount
(all-or-discard; a torn stage is discarded, a failed replay is left
committed for the next boot).

## Limits

| Quantity | Value |
|---|---|
| Max file size | 12 + 1024 + 1024^2 + 1024^3 blocks = ~4 TiB format; volume-capped in practice |
| Name | 255 bytes; path depth unlimited on disk (`PATH_MAX` 4096 is an in-memory constant; today's fs.h callers still hold 64-byte buffers) |
| Inodes | group_count * inodes_per_group; ~590k on 9 GiB at defaults |
| Dir entries | uncapped (a dir grows like a file; the kernel writer stops dirs at single-indirect scale, ~4 M entries) |

## Coexistence with TFS2

The two formats' signatures don't overlap: TFS2's superblock is
sector 0 (`"TFS2"`, version 3); TFS3's is block 8. The kernel probes
TFS3 first, then TFS2; a blank disk is formatted with the default
(TFS3 since Stage E; `tools/seed_disk.py` applies the same rule at
build time). `fsformat <name> confirm` switches live, wiping the
other format's signatures first.

## Reference implementation

`tools/tfs3_writer.py` is the normative host-side reader/writer --
`format`/`info`/`ls`/`read`/`write`/`mkdir`/`delete`/`sync`/`trim`/
`corrupt`. Unlike tfs2-spec, this file embeds no separate reference
reader: the tool exists, is exercised by the build (`seed_disk.py`)
and by `tools/check_layout.py`, and a second in-doc copy would only
drift. Its write scope is direct + single-indirect per file
(~4.03 MB), the same deliberate cap tfs2_writer has.
