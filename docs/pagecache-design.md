# A write-back page cache, and the `fsync()` that makes it safe

A staged plan, in the shape `docs/signals-design.md` and
`docs/init-design.md` used. It answers the question the storage
measurements of 2026-09-04 left open: **toy-os pays for durability on
every write, and Linux pays for it only when asked. What would it take
to do the same?**

**Status: DESIGNED, NOT BUILT.**

## Why this exists, in one measurement

`QUERY_BLKSTAT` landed on 2026-09-04 and made the cost attributable for
the first time. On the bare-metal laptop (AHCI, SATA SSD), 16 MiB:

| profile | MB/s | read | write | flush | flush share |
|---|---|---|---|---|---|
| SEQ-write | 14.68 | 252 ms | 87 ms | **393 ms** | **53%** |
| RND4K-write | 1.09 | 301 ms | 56 ms | **730 ms** | **67%** |

One flush costs **659 us** on the drive against 95 us emulated. Every
`fs_write*()` call is one TFS3 transaction ending in two of them, so at
64 KiB a syscall that is 32 flushes per MiB, and 512 per MiB at 4 KiB.

**The ceiling is already measured.** `storage.sync = lazy` turns the
barriers off entirely and gives **36.55 MB/s** sequential and **4.01**
at 4 KiB -- 2.5x and 3.7x. That is what removing flush cost is worth on
this hardware, and it is the number this project is chasing. The
difference is that `lazy` buys it by giving up crash recovery, and a
page cache buys it by BATCHING: one barrier for thousands of
operations instead of two per write.

## What exists today, measured

Checked against the tree rather than assumed, and three of these
changed the plan.

- **The journal can already group-commit.** `T3_V2_JSLOTS` is 32, and
  `create_entry()` and `fs_rename()` each stage several distinct blocks
  and two distinct inodes under one `txn_begin()`. So committing many
  files' metadata in one transaction needs no format change -- it needs
  callers that batch. The ceiling is 32 distinct blocks per transaction,
  which is a real bound a writeback pass has to split against.
- **Nothing caches file data.** `read_range_impl()` goes to the block
  device every call. The three caches that exist are narrow and
  unrelated: `rcache` (3 slots, indirect-table blocks), `pcache` (one
  block, the write-side leaf table), and `ncache` (16 directory-entry
  names).
- **There is no reclaim mechanism anywhere in the kernel.** No shrinker,
  no watermark, no eviction of anything; `pmm_alloc_frame()` returns 0
  when a zone is exhausted and that is the whole story. A page cache is
  the first thing here that would need to give memory back, so it brings
  its own eviction or it is a leak with a nice name.
- **Memory is not the constraint.** A default boot has ~1.9 GB free of
  2 GB. `ramfs` already sets a budget at mount ("half of free memory"),
  which is the precedent to copy.

### A per-syscall cost that is real in the code and NOT yet measured

**`resolve()` reads inode sectors from the disk on every read and write
syscall, with no inode cache at all.** A path of depth N costs at least
N uncached sector reads plus up to N dirent-block scans, per syscall.
That is plain in `tfs3.c`, and it is the obvious candidate for why a
sequential WRITE spends most of its block time READING (252 ms of 737 on
the laptop; 76% of block-layer time in QEMU).

**An attempt to confirm it by PATH DEPTH failed, and the failure is
worth recording alongside the answer.** Running the same 4 MiB benchmark
at `/d.tmp` and at `/aa/bb/cc/d.tmp`, each on a freshly seeded disk, the
two profiles disagreed in direction:

| | SEQ-write | its reads | RND4K-write | its reads |
|---|---|---|---|---|
| depth 1 | 10.25 MB/s | 3274 | 0.834 MB/s | 3629 |
| depth 4 | 7.00 MB/s | 4448 | 0.780 MB/s | 3486 |

Sequential got worse with depth, random got slightly better, and block
allocation layout varies between runs more than depth costs. **Depth is
not the driver, so depth cannot measure it.**

**Counting it directly settled it.** `lookup()` now brackets itself with
the block layer's read counter, so the delta is exactly the disk traffic
a resolution caused, and `QUERY_FSSTAT` reports it. On QEMU/KVM with
AHCI, 1 MiB:

| profile | block reads | caused by lookup | share |
|---|---|---|---|
| SEQ-write | 1712 | 503 | **29%** |
| SEQ-read | 1556 | 1033 | **66%** |
| RND4K-write | 2493 | 1187 | **48%** |
| RND4K-read | 1709 | 854 | **50%** |

**Path resolution is between a third and two thirds of every block read
this filesystem issues**, and the driver is not depth but COUNT: a
single 64 KiB write syscall triggers about eight resolutions, each
costing ~3.6 block reads. The hypothesis was right and the first
experiment was simply built on the wrong variable.

## The stages

Each one ships on its own and is worth having if the next never lands.

### Stage 0 -- MEASURE the resolution cost, then decide  [MEASURED]

**Done: `QUERY_FSSTAT` and `/bin/diskbench`'s `lookup` line.** The
numbers are in the table above -- 29-66% of all block reads -- so the
change is justified rather than assumed.

The change itself is small: `struct open_file` holds
`char name[64]` and a position, so give it the resolved inode as well
and a read or write on an open fd skips resolution entirely. That needs
an `fs_ops` entry taking a resolved handle, or a VFS inode cache keyed
by path and invalidated on rename and delete -- the second being smaller
and bounded by the same correctness question stage 2 must answer anyway.

**Note what is NOT a target here.** `sys_do_write_file()` resolves twice
on an `O_APPEND` write, once for `fs_size()` and once for the write, and
its comment says why: re-reading the size is what makes two appenders
interleave whole writes instead of overwriting each other. That is a
semantic, not an oversight.

**Buys:** unknown until measured, which is the point of the stage.
**Does not buy:** anything about flushes.

### Stage 1 -- one cache, above the VFS, read-only first

A page cache keyed by (mount, inode, block index), consulted by
`fs_read_range()`. Read-only means it can be wrong about nothing: a hit
returns what the disk had, and a write invalidates.

**THE HOLE TO DESIGN AROUND, and it is not obvious:** `fs_write()` and
`fs_write_range()` are two independent VFS entry points reaching two
independent `fs_ops` slots, and `fs_write_range_begin/_step` is a third.
A cache hooked into `read_range`/`write_range` alone is silently
bypassed by `fs_write()`'s five real callers -- shell history, the
shell's `write` command, `O_TRUNC` on open, every `/etc` config write,
and the timezone database. Stale reads, silently. Every mutating VFS
entry point invalidates, or the cache is a correctness bug.

**Buys:** repeat reads, and the read half of read-modify-write.

### Stage 2 -- write-back, behind `storage.sync`

`write()` copies into the cache, marks it dirty and returns. Writeback
happens on a dirty threshold, on an interval, at `sync`, and at unmount
-- and a writeback pass commits ONE transaction for as many files as
fit in the journal's 32 slots.

**Gated behind the setting from the start.** `storage.sync` gains a
third mode, and `strict` stays the default, so nothing changes for
anyone until they ask. That is what makes this landable incrementally
rather than as a single switch-flip nobody can bisect.

**Buys:** the flush cost -- the 53% and 67% above.

### Stage 3 -- `fsync()` and `fdatasync()`

Now they mean something: flush this file's dirty pages and commit.
Per-file granularity is possible ONLY because the cache is keyed by
file; the sector cache below could never have offered it, since it is
keyed by LBA and cannot know which file owns a sector.

Three edits, as every syscall is: a number in `syscall_abi.h`, a handler
plus its prototype, a row in `syscall_table.c`. Highest number in use is
95.

**A DECISION TO MAKE HERE, not to inherit:** does `fsync()` issue a
device flush, or only push to the drive? Linux's does flush; macOS's
does NOT, which is why `F_FULLFSYNC` exists and why every database on
macOS has a workaround. Follow Linux.

### Stage 4 -- the writeback interval as a setting

`storage.writeback_interval`, the visible half of ext4's `commit=5` and
Linux's `dirty_expire_centisecs`. Only meaningful once stage 2 exists,
which is why it is last rather than bundled with the mode.

## The honest case against

- **It weakens a promise that is currently simple.** "Every write is on
  the disk when it returns" is one sentence a person can hold. What
  replaces it -- durable at `fsync`, at `sync`, at an interval, at a
  threshold, at unmount -- is five, and every one is a chance to be
  wrong. The setting is what keeps the old sentence available.
- **The kernel has no reclaim, and this is the first caller that needs
  it.** Eviction under memory pressure is the part most likely to be
  got wrong, and its failure mode is the machine dying rather than a
  file being slow.
- **A cache is a second copy, and stale data is silent.** Every bug in
  this project's history that took longest to find was silent: a `sync`
  that flushed nothing, a truncation mid-word, a cache invalidated only
  by its owner's writes. This is a large new surface of exactly that
  kind.
- **Stage 0 may be most of the win.** It is now measured at 29-66% of
  all block reads, and reads are 34% of a sequential write's block time
  on hardware. If caching resolution removes most of that, and
  `storage.sync` already offers the flush trade, the remaining case for
  stages 1-4 is smaller than it looked when this document was started --
  which is an argument for building stage 0 and re-measuring before
  committing to the rest.
