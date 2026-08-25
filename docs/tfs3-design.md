# TFS3 design (block groups + inodes)

**Status: IMPLEMENTED (2026-08-14) -- see `docs/tfs3-spec.md` for the
byte-exact format as shipped.** This file remains the record of the
design decisions and their reasoning; where the implementation
deliberately revised the design mid-build, the section carries a note
(the journal's scope is the significant one). The build was staged
A-E, one commit each -- see the git history
starting at "The VFS selects filesystems by probe now".

Decisions settled deliberately rather than defaulted (each has its own
section below): journal scope (a small fixed multi-block transaction,
not inode-only journaling), timestamps (epoch seconds, not
`struct rtc_time`), symlinks (format support is first-class NOW,
implementation deferred -- an explicit user requirement, symlinks are
wanted eventually and the format must not make them an afterthought).

## Why TFS3 (vs. evolving TFS2 in place)

TFS2 has two hard limits baked into its format:

- A fixed 256-slot file/directory table (`FS_MAX_FILES`,
  `kernel/include/api/fs.h`) -- every file *and* every directory
  shares one global cap.
- Each record stores its own full path inline (`path[64]`,
  `FS_PATH_MAX`) -- an individual name and a whole path are capped by
  the same 64-byte field, and a "directory" is just a record with
  `type == DIR`, not a container of anything.

Both come from one root choice: a file's identity (its path) and its
metadata live in the same fixed-size global table entry. TFS3 breaks
that coupling -- inodes hold metadata only, directory-entry data
blocks hold names -- which removes both limits at once.

Per the project's standing policy (`tfs.c`'s top comment,
`docs/decisions.md`), TFS3 is a new, **incompatible** on-disk format:
no migration from TFS2, an old disk is detected as foreign at mount,
same as every previous format bump.

## Conventions

Same rules as `docs/tfs2-spec.md`'s Conventions section:

- All multi-byte integers are **little-endian**.
- Structures are **hand-serialized field by field** at the offsets
  given in each table below -- there are no C structs on disk, so C
  alignment/padding rules never apply. (The struct sketches in the
  original design discussion are replaced by offset tables for exactly
  this reason.)
- Disk I/O is in 512-byte sectors; filesystem addressing is in
  4096-byte blocks (`FS_BLOCK_SIZE`, 8 sectors). A "block number" is
  global within the volume and VOLUME-relative: block `b` starts at
  sector `volume_base_lba + b * 8` (see "Volumes and partitions").
  Block 0 contains the volume's first sector; on today's flat disk
  `volume_base_lba` is 0.
- Bytes marked reserved are written as zero and ignored on read.

## Disk layout

All positions below are **volume-relative** (see "Volumes and
partitions"): block `b` occupies the 8 sectors starting at
`volume_base_lba + b * 8`. On today's flat disk `volume_base_lba` is
0, so volume block == absolute block.

```
blocks 0-7    reserved, never touched by TFS3 (32 KiB)
              -- on a flat disk this is room for an MBR (LBA 0) and a
              primary GPT (header LBA 1, entries through LBA 33);
              inside a partition it is harmless slack
block 8       superblock            (first sector meaningful)
block 9       journal header        (first sector meaningful)
block 10-13   journal block images  (4 blocks, one per transaction slot)
block 14-29   group descriptor table (16 blocks, fixed -- see below)
block 30      block group 0, then group 1, ... to end of volume
```

**Why the 32 KiB reservation:** TFS2 puts its superblock at LBA 0 and
its journal header at LBA 1, which collides with both an MBR and the
GPT header -- a real cost already paid once (`docs/decisions.md`, "GPT
header verification: a host-compiled unit test, not a live boot").
A new format gets out of the way for free. TFS3 never reads or writes
volume blocks 0-7; `tools/mkpart_test.py`-style partition tables and a
flat-disk filesystem coexist with no byte-range carve-outs.

**Why the descriptor table is a fixed 16 blocks** rather than sized to
`group_count`: 16 blocks hold 4096 descriptors, which at 128 MiB per
group covers a 512 GiB volume -- four times the LBA28 ceiling this
kernel can address at all, so the size never binds. What the constant
buys is that **every structural position in the format is derivable
with no superblock in hand**: group 0 starts at block 30, period, and
the superblock-backup locations (see "Superblock backups") fall out of
the volume size alone. A descriptor table sized to fit would save at
most 15 blocks (60 KiB) and cost exactly that recoverability. Unused
descriptor slots are zeroed.

So the whole layout derives, in the spirit of tfs2-spec's "deriving
these offsets yourself":

```
group0_start = 30                          (constant)
group_count  = (total_blocks - 30) / blocks_per_group   (floor;
               a trailing partial group is unused)
group g      = blocks [30 + g * blocks_per_group,
                       30 + (g+1) * blocks_per_group)
```

Group size and inode density are computed at **format time** from the
disk's real capacity (ext2-style bytes-per-inode ratio) -- more disk
means more inodes automatically. This is what removes the fixed
256-file cap without a recompile.

## Superblock (block 8, first sector)

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | magic | `"TFS3"` |
| 4 | 1 | version | 1; incompatible bump = foreign disk, reformat |
| 5 | 1 | flags | bit 0 = `TFS3_FEATURE_BLOCK_CKSUM` (see Checksums) |
| 6 | 2 | reserved | |
| 8 | 4 | total_blocks | VOLUME size in blocks, clamped to the real device/partition extent at format time (TFS2's `clamp_total_blocks_to_disk()` lesson) |
| 12 | 4 | blocks_per_group | 32768 (one 4 KiB bitmap's worth) |
| 16 | 4 | inodes_per_group | from the bytes-per-inode ratio; <= 32768 |
| 20 | 4 | group_count | |
| 24 | 4 | group0_start | always 30; stored as a cross-check, a mismatch fails validation (see layout) |
| 28 | 16 | reserved | future additive fields, zeroed today |
| 44 | 4 | checksum | FNV-1a-32 over bytes 0-43 with this field zeroed |

The superblock is checksummed because the most expensive corruption
lesson this project has ("an unreadable superblock is not a foreign
disk", `docs/decisions.md`) is about exactly this structure: validate
before trusting, refuse rather than guess.

## Superblock backups (ext-style)

The superblock is effectively **write-once**: after format, nothing
in normal operation modifies it (free counts live in the group
descriptors, the feature flags are format-time). That makes backups
nearly free -- they never need resyncing.

Each backup region is the **trailing 17 blocks of its group**: a
16-block snapshot of the group descriptor table, then one superblock
copy in the group's final block. Regions live in **group 1 and the
last group** (`group_count - 1`); with `group_count == 1` a single
region sits at the end of group 0, and with `group_count == 2` the
two coincide. Their blocks are marked allocated in the owning group's
block bitmap at format time, so the allocator never has to know they
exist.

Backup locations are derivable from the volume size alone -- no
superblock needed, which is the entire point (`group0_start` is the
constant 30, `blocks_per_group` is the constant 32768):

```
backup_sb(g)  = 30 + (g + 1) * 32768 - 1      (the group's last block)
backup_gdt(g) = backup_sb(g) - 16 .. backup_sb(g) - 1
```

Rules, in the spirit of the refuse-don't-guess policy:

- **Mount fallback:** a primary that fails its read or checksum makes
  mount try the backups (group 1 first, then last group). Mounting
  from a backup is loud (klog) and does NOT rewrite the primary --
  an automatic write to the one block that just failed is how a
  transient read error becomes permanent damage.
- **Repair is explicit:** `fsck repair` rewrites the primary (and any
  bad backup) from a copy that validates.
- **Backup GDT free-counts are stale by design** (they are format-time
  snapshots, like ext2's backup GDTs between resizes). They are for
  recovering the *shape* of the volume; `fsck` recomputes the counts
  from the bitmaps afterwards, which it already must know how to do.
- A backup superblock copy is byte-identical to the primary
  (including the checksum field), so validation is one shared code
  path.
- **The wipefs rule** (learned live, not designed in advance):
  reformatting a disk with a DIFFERENT filesystem must erase this
  filesystem's primary AND backup superblocks first, or the stale
  backups keep claiming the disk at probe time -- formatting a TFS3
  disk as TFS2 overwrote the primary (it sits inside TFS2's
  record-table region) but not the far-away backups, and the probe
  mounted the corpse. Each backend owns a `wipe()` op
  (`fs_ops.h`) that erases exactly its own signatures;
  `fs_format_backend()` wipes every other backend before formatting,
  and the host tools apply the same rule (`tfs3_writer.py format`
  clears a `TFS2` magic at LBA 0 -- and only that, an MBR/GPT there
  is left alone).

## Group descriptor (16 bytes each, table at blocks 14-29)

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | free_blocks | cached count -- see below |
| 4 | 4 | free_inodes | cached count |
| 8 | 4 | reserved | |
| 12 | 4 | checksum | FNV-1a-32 over bytes 0-11 |

Deliberately **slimmer than ext2's**: the original draft stored
`block_bitmap_start`/`inode_bitmap_start`/`inode_table_start` per
group, but every group has the same fixed internal layout (below), so
those are all derivable by formula -- storing them would be state that
can only ever agree with the formula or be corrupt. Only the free
counts are genuinely state, and they are **caches**: the bitmaps are
the authority, and `fsck` recomputes and repairs a descriptor that
disagrees rather than believing it.

### Inside each group (fixed layout, all derivable)

```
group g spans blocks  G = group0_start + g * blocks_per_group
                      .. G + blocks_per_group - 1
block bitmap    block G           (32768 bits = blocks_per_group)
inode bitmap    block G + 1       (bit i = inode g*inodes_per_group + i)
checksum table  blocks G + 2 ..   (only if TFS3_FEATURE_BLOCK_CKSUM;
                                   ceil(blocks_per_group * 4 / 4096) blocks)
inode table     next blocks       (inodes_per_group * 128 bytes)
data blocks     the rest of the group
```

Bitmap bits for a group's own metadata blocks (bitmaps, tables) are
set at format time, so allocation never has to special-case them.

## Inode (128 bytes)

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 1 | type | see Inode types |
| 1 | 1 | reserved | |
| 2 | 2 | links | hardlink count; 0 in a bitmap-allocated inode is fsck-repairable damage |
| 4 | 8 | size | bytes |
| 12 | 8 | created | seconds since the Unix epoch -- see Timestamps |
| 20 | 8 | modified | seconds since the Unix epoch |
| 28 | 48 | direct[12] | global block numbers, u32 each; 0 = hole/unassigned |
| 76 | 4 | single_indirect | block of 1024 u32 pointers |
| 80 | 4 | double_indirect | |
| 84 | 4 | triple_indirect | |
| 88 | 4 | checksum | FNV-1a-32 over bytes 0-87 and 92-127 (i.e. the whole inode except this field); verified on every inode read |
| 92 | 4 | reserved (owner/mode) | earmarked for Milestone 17: uid u16 + mode u16 |
| 96 | 32 | reserved | future additive fields |

**Why 128 bytes, not the draft's 90:** four inodes per 512-byte
sector, exactly -- no inode ever straddles a sector, so a torn sector
write can't tear an inode in half and updating one inode is a
single-sector read-modify-write. The padding is also where Milestone
17's owner/mode and any future fields live without a format bump
(ext2 made the same call for the same reasons).

**No `used` byte** (the draft had one): the group's inode bitmap is
the single authority on whether an inode is allocated. Two copies of
the same fact can disagree, and then something must arbitrate; with
one copy the failure modes collapse to "bitmap set but inode garbage"
(caught by the inode checksum, fsck clears the bit or rebuilds from
the dirent that points there) and "bitmap clear but dirent points
here" (fsck removes the dirent -- same prefer-a-leak-to-a-double-
allocation instinct as TFS2's metadata ordering, `docs/decisions.md`).

**Inode numbering:** inode 0 is reserved as the null inode (a dirent
with `inode == 0` is a free slot) and is never allocated; inode 1 is
the root directory, created at format time. Group membership by
division: `group = n / inodes_per_group`, `index = n %
inodes_per_group`.

### Inode types

| Value | Type | Initial implementation |
|---|---|---|
| 0 | file | yes |
| 1 | directory | yes |
| 2 | symlink | format only -- see Symlinks |
| 3 | chardev | reserved enum value only |
| 4 | blockdev | reserved enum value only |
| 5 | fifo | reserved enum value only |

## Timestamps

`created`/`modified` are **uint64 seconds since the Unix epoch**, not
TFS2's 7-byte serialized `struct rtc_time`. A new incompatible format
is the free moment to make this switch; deferring it again would bake
the "local wall-clock, no offset, means only what it looks like"
ambiguity (`docs/decisions.md`, file-timestamps entry) into a third
format. Cost, accepted knowingly: the kernel has no civil<->epoch
conversion anywhere today, so the implementation needs one small
helper (RTC's broken-down time -> epoch at write, the reverse for
display). Timezone policy is unchanged -- the RTC is read as local
time, so the stored epoch inherits whatever `timezone` was set; this
does not try to solve UTC handling, it only makes timestamps
arithmetic-comparable and ports the format to what Milestone 40-era
code will expect.

## Directory entries (data inside a directory inode's blocks)

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | inode | 0 = free/deleted slot |
| 4 | 2 | rec_len | total bytes this entry occupies; multiple of 4 |
| 6 | 1 | name_len | 1-255 |
| 7 | name_len | name | raw bytes, NOT NUL-terminated |

Entries are packed first-fit within a block and never span blocks;
`rec_len` includes the slack after the name (rounded up to 4, plus any
absorbed free space), so deletion is "fold this entry's bytes into the
previous entry's rec_len" and creation reuses slack -- the ext2
scheme. A directory's capacity is however many blocks it grows to,
like any file.

Lookup of `/docs/notes.txt`: read root's (inode 1) data blocks, scan
dirents for `"docs"`, follow its inode number, scan that inode's
blocks for `"notes.txt"`.

**`.` and `..` are real entries**, written when a directory is
created, pointing at the directory itself and its parent. Link-count
rules that fall out (and that `fsck` checks):

- A new file has `links = 1`; each additional hardlink increments it;
  blocks are freed only when it reaches 0 (`unlink` semantics).
- A new directory has `links = 2` (its own dirent + its `.`); its
  parent gains 1 (the child's `..`). So a directory's count is
  2 + number of subdirectories.
- The empty-directory test (for the unchanged no-recursive-delete
  policy) is "no entries other than `.` and `..`".
- Hardlinks to directories are refused -- `.`/`..` are the only
  directory entries with `links > 1` semantics, same as every real
  Unix filesystem, because anything else makes the tree a graph.

## Symlinks (format now, implementation deferred)

Format support is first-class by explicit decision -- symlinks are
wanted eventually, and the format must not need a bump to add them.

Fast-symlink storage: when the target path fits in **60 bytes**, it is
stored inline in the block-pointer area (offsets 28-87: `direct[12]` +
the three indirect pointers), no data block allocated. `type == 2`
tells a reader those bytes are the raw target string (length =
`size`), not block pointers. A longer target (up to the in-memory
`PATH_MAX` 4096) falls back to one data block, addressed normally.

Deferred to implementation time, deliberately (the initial TFS3
implementation writes and reads no symlinks; it must merely not choke
on `type == 2`):

- **Resolution lives inside the backend.** `kernel/lib/kpath.c` is
  purely lexical ("nothing here touches the filesystem",
  `kpath.h`) and `vfs.c` does no path work at all -- path handling is
  backend-internal by contract (`fs_ops.h`'s normalization note). So
  symlink-following is a resolve loop inside tfs3.c's lookup, not a
  kpath or VFS feature.
- That loop caps total symlink hops per lookup (~8, the classic
  `ELOOP` guard) so `ln -s a b; ln -s b a` terminates. Note this is a
  *traversal* cap, unrelated to `KPATH_MAX_DEPTH`'s lexical nesting
  cap.

## Block addressing

Block numbers are global u32s, counted straight through the VOLUME --
locality is an allocation policy, not something the pointer format
encodes (same as TFS2 and ext2). The indirect scheme is TFS2's,
unchanged: 12 direct + single + double + triple indirect, 1024
pointers per 4 KiB block. Group membership by subtract-then-divide:

```
group  = (b - group0_start) / blocks_per_group
offset = (b - group0_start) % blocks_per_group
```

(the subtraction is the one difference from the original draft, which
divided raw block numbers -- correct only if groups started at block
0, which they don't).

## Volumes and partitions (BUILT)

Every block number on disk is relative to a **volume**: a contiguous
sector extent `{ base_lba, sector_count }`. This was designed
partition-proof from day one, by an explicit user requirement (toy-os
should eventually boot from a TFS3 partition), and **it now does** --
see `docs/decisions.md` and `tools/partition_test.py`.

The prediction below about "when partition mounting arrives" turned out
to be half right, and the half it got wrong is the interesting one: the
change *was* confined to the VFS's probe loop, but the extent is **not**
handed to the backend. It went into a `block_device` wrapper
(`kernel/drivers/block/block_part.c`) one layer further down, so TFS3's
volume is still `{0, blk_sector_count()}` — and that now *means* the
partition, because the device it is handed IS the partition. Linux
(`bd_start_sect`) and Windows (`partmgr`) both put it there.

The seam earned its keep anyway, and this is the point worth keeping:
it is what made partition mounting a change to the block layer and
**not** to `tfs3.c`, which was not touched at all.

The original reasoning, which still holds:

- Nothing on disk ever stores an absolute LBA. `total_blocks` is the
  volume's size. An image is bit-identical whether it lives at LBA 0
  or inside a partition -- which also means host tools can build a
  filesystem image and `dd` it into a partition unchanged.
- The implementation does all I/O through the volume view handed to
  `probe()`/`init()` -- one `volume_read/write(vol, block, ...)` seam,
  never raw absolute ATA calls. On the flat disk this is a zero-cost
  base of 0.
- When partition mounting arrives, the change is confined to the
  VFS's probe loop: iterate `kernel/drivers/partition.c`'s
  already-parsed MBR/GPT entries, offer each extent to each backend,
  plus the flat-disk extent as the fallback. No format change, no
  backend change. *(Built as `try_partitions()` in `kernel/fs/vfs.c`.
  "No format change, no backend change" held exactly — `tfs3.c` has no
  edit in it. What is offered per entry is a partition `block_device`
  rather than an extent; see above.)*
- The 32 KiB front reserve is volume-relative and kept in all cases:
  essential on a flat disk (MBR/GPT live there), harmless slack
  inside a partition.

## Allocation policy (not format) and performance

None of this is encoded on disk; it is how the implementation should
allocate, recorded here so it isn't re-litigated:

- **Locality:** prefer the parent directory's group for new inodes
  and a file's own group for new blocks; spill to the next group with
  free space only when full. This is the whole reason groups exist.
- **Try-adjacent-first on append:** when extending a file, try
  `last_block + 1` before scanning. Sequential writes then produce
  contiguous runs, which is what makes the existing contiguous-run
  ATA batching (the TFS2 coalescing work, 18 -> 25 MB/s) actually
  fire instead of being defeated by scattered allocation.
- **Per-group rotor:** remember where the last allocation in each
  group landed and scan from there, not from bit 0 (ext2's trick) --
  keeps allocation O(small) as a group fills.
- **A small in-RAM name-lookup cache** (directory inode + name ->
  inode number, a handful of entries, invalidated on any mutation of
  that directory): path walks are TFS3's new hot loop, every `/a/b/c`
  lookup is otherwise a dirent scan per component. This is the cheap
  cousin of roadmap Milestone 3's on-disk directory index.
- **No private block cache.** Milestone 3's block/buffer cache is the
  single biggest read-path win and it belongs below the filesystem;
  TFS3 should keep its block reads behind one seam so it can sit on
  top of that cache when it exists, not grow its own.

Deliberately not done: hashed/htree directories, extents, an on-disk
dcache -- real complexity for workloads this OS doesn't have.

## Checksums

**Per-inode, per-superblock, per-group-descriptor: always on**, all
FNV-1a-32 (offset basis 0x811C9DC5, prime 0x01000193 -- the exact
function TFS2's journal already uses, `fnv1a()` in `tfs.c`, mirrored
host-side in `tools/tfs2_writer.py`). One hash implementation, kernel
and host. Right strength for the threat model: torn writes and bit
rot, not adversaries -- the same non-cryptographic call real
filesystems make for metadata.

**Per-data-block: optional format-time feature bit**
(`TFS3_FEATURE_BLOCK_CKSUM`), reserving a per-group checksum table
(4 bytes per block, index = offset-within-group). Off means the region
isn't allocated at all. Two things recorded honestly for whoever
builds it:

- **The algorithm is deferred to Milestone 16**, which currently
  names CRC32c. Note the kernel's only CRC today is plain CRC-32
  (0xEDB88320), `static` inside `partition.c`'s GPT parser -- so
  CRC32c is new code either way, and M16 should make the
  FNV-vs-CRC32c call once, for data blocks, when it arrives. This
  document does not pre-decide it; the feature bit and table layout
  are algorithm-agnostic (32-bit slot).
- **Every data-block write becomes two writes** (data + its table
  entry, possibly distant). Contiguous runs batch their table updates
  into one write, but the cost is real and must be measured (`stress`)
  when the feature is first implemented, per the M16 checklist.

Inherited known gap, unchanged from TFS2's honesty about data: data
blocks (and their checksum entries) are not journaled, so a crash can
leave a stale checksum on valid data. `fsck`'s repair for a mismatch
is therefore recompute-and-rewrite, never assume-data-loss.

## Journal: one fixed-size multi-block transaction

**Implementation note (Stage C revision):** the shipped journal is
NARROWER than the sketch below -- transactions carry dirent blocks and
inode-table blocks only. Bitmaps are write-through and unjournaled
under the set-before-use / clear-after-persist ordering (crash =
leak, fsck reclaims -- TFS2's rule), which keeps every operation at
<= 3 of the 4 slots and halves the journaled bytes for the same crash
guarantees. Directory growth runs as its own empty-block-first
transaction. `docs/tfs3-spec.md` documents what shipped; the sketch
below is kept as the design-time record.

The decision the original draft left open, now made: **generalize
TFS2's single-slot journal to a single-transaction intent log of up to
4 metadata blocks** -- not inode-only journaling. Inode-only would be
*weaker* than TFS2, not equivalent: TFS2's one record write WAS the
whole mutation, so its journal covered the entire consistency story,
while a TFS3 create touches a dirent block + an inode-table block +
up to two bitmap blocks. Four slots covers the worst mutation
(create/mkdir/unlink/rename each touch <= 4 metadata blocks); data
blocks remain unjournaled, as in TFS2.

Journal header (block 9, first sector):

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | magic | `"JRN3"` |
| 4 | 1 | commit | nonzero = transaction pending |
| 5 | 1 | count | 1-4 blocks in this transaction |
| 6 | 2 | reserved | |
| 8 | 4 | seq | monotonic transaction counter (diagnostic) |
| 12 | 32 | slots[4] | per slot: target block number u32 + FNV-1a-32 of the block image u32 |
| 44 | 4 | checksum | FNV-1a-32 over bytes 0-43 with this field zeroed |

Block images live in blocks 10-13, slot order.

Write discipline, same two-barrier shape as TFS2's
`persist_record()` (`tfs.c` documents why two barriers are the
minimum the recovery protocol depends on):

1. Write the block images and the committed header (no barrier
   between them).
2. **Flush.** From here the transaction survives a crash.
3. Write the target blocks in place.
4. **Flush.**
5. Clear the commit flag (no barrier; a stale committed header just
   replays idempotently on the next boot).

The window where the commit flag could reach disk before the images
(between steps 1 and 2 nothing orders them) is closed the same way
TFS2 closes it: replay verifies every slot's image checksum and
discards the whole transaction on any mismatch -- a torn stage is
indistinguishable from no stage, which is correct, because nothing in
step 3 had begun. Replay applies all-or-nothing; if the replay's own
writes fail, the header is left committed for the next boot (TFS2's
`replay_journal()` behavior, kept).

## Reserved space

Superblock (16 bytes), group descriptor (4), inode (1 + 4 + 32) --
for future *additive* fields only. This does not buy cross-version
compatibility (explicitly a non-goal under the reformat-only policy);
it buys the chance that a small future field lands in reserved bytes
instead of forcing a version bump.

## Limits and capacity

Per-component name <= 255 bytes; total path depth unlimited on disk
(no full path is ever stored); `PATH_MAX = 4096` is an in-memory
constant for buffer-sizing only.

Concrete numbers, so they aren't rederived (4 KiB blocks, u32
pointers, 128-byte inodes; "today" = the 9 GiB image; LBA28 caps any
disk at 128 GiB until roadmap M3's LBA48):

| Quantity | Value |
|---|---|
| Max file size, format | 12 + 1024 + 1024^2 + 1024^3 blocks = ~4 TiB |
| Max file size, practical | disk-capped: 9 GiB today, 128 GiB at LBA28 |
| ...direct only | 48 KiB |
| ...+ single indirect | ~4.05 MiB |
| ...+ double indirect | ~4.00 GiB |
| Block group | 32768 blocks = 128 MiB; 72 groups on 9 GiB |
| Inodes (= files + dirs) | format-time ratio; at the 16 KiB/inode default: 8192/group, **~590,000 on 9 GiB** (vs. 256 total today), scaling with disk |
| Per-group metadata overhead | 258 blocks (~0.8%); +32 blocks with the block-checksum feature; +17 in the two backup-region groups |
| Entries per directory | uncapped; ~250-340/block, ~3-4K in direct blocks, ~350K with one indirect |
| Dirs | no separate cap -- same inode pool as files |

## Deferred: device nodes and mount points

Unchanged from the original draft:

- **Device nodes** (types 3-5): enum values reserved (free), storage
  would reuse the fast-symlink inline area (major/minor instead of a
  target string). Not built -- toy-os has no `/dev` concept; drivers
  are reached through `kapi.h`, and the standing bar is a second real
  caller.
- **Mount points are not an inode concept.** What makes a directory a
  mount point is VFS runtime state, not anything on disk; that
  bookkeeping belongs in `vfs.c` (roadmap Milestone 25) and needs
  zero format support here.

## Explicitly out of scope / unchanged from TFS2

- No recursive delete (deleting a non-empty directory fails).
- No locking against racing mutations (single-threaded kernel).
- RAM-only fallback mode when no disk is found.
- Write-through durability: every mutation hits disk before its call
  returns, journal aside. Data journaling/COW/snapshots are roadmap
  Milestone 29, on top of this format or a successor.

## What still needs building, beyond the format

- `kernel/fs/tfs3.c` implementing the full `fs_ops` surface -- 19
  required function pointers plus `name` (`fs_ops.h`; all
  non-NULLable, including the steppable read/write entry points).
  `vfs.c`'s single `g_fs` is assigned once in `fs_init()`; backend
  selection there by superblock magic probe is pre-authorized by
  `fs_ops.h`'s own comment. Path normalization is backend-internal
  per the same header.
- The civil<->epoch time helper (see Timestamps).
- `fsck` for the new format: bitmap-vs-checksum arbitration rules
  (see Inode), link-count verification, free-count cache recompute,
  checksum-mismatch = recompute-and-rewrite for data blocks.
- A host-side `tools/tfs3_writer.py` (built as its own tool, not a
  tfs2_writer.py mode -- that fork resolved at Stage B)
  mirroring the format, including the superblock magic+version
  refusal behavior tfs2_writer already has, `trim` awareness of
  the new bitmap locations, and writing the backup regions at format
  time.
- Partition mounting (later, deliberately): the VFS probe loop
  iterating `partition.c`'s MBR/GPT entries and handing each volume
  extent to the backends -- see "Volumes and partitions". Nothing in
  the format waits on it.
- KTESTs following `kernel/fs/fs_test.c`'s patterns: `FRESH()`-style
  state setup, `fs_is_persistent()` skips, `fault_inject.h` brackets
  for the error paths -- plus new ones this format makes possible
  (journal replay via `--stage-journal`-equivalent tooling, hardlink
  counts, `.`/`..` integrity).
- `docs/filesystem-layout.md` + `tools/check_layout.py` updates once
  the record/path budget stops being the binding constraint.
- The `rename()` op (roadmap M15 item) -- fits the 4-slot journal:
  two dirent blocks + inode + nothing else.
