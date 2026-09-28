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

One flush costs **659 us** on the drive against 95 us emulated -- **and
that figure is the LIGHT-LOAD one.** Measured again over 8 MiB of
sustained writing the same drive takes **2.8-3.3 ms** per flush, four to
five times more, as its SLC cache fills and garbage collection starts
competing. So the cost of a barrier here is not a constant to look up;
it grows with exactly the workload that issues the most of them. Every
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

### A per-syscall cost that is real in the code -- MEASURED AND FIXED (2026-09-13)

**`resolve()` read inode sectors from the disk on every read and write
syscall.** A path of depth N cost at least N uncached sector reads plus
up to N dirent-block scans, per syscall. That was the candidate for why
a sequential WRITE spent most of its block time READING, and it was
right.

The cause was narrower than "no inode cache": TFS3 HAD a full-path cache
(`lcache`), it just lived inside `lookup()`, which only the READ path
called -- `tfs3_write_range()` called `resolve()` directly. The
signature was that sequential READ was completely insensitive to path
depth while random 4 KiB WRITE lost 34% of throughput on a
three-component path. Moving the cache into `resolve()` (322f5201)
removed it; path depth now costs nothing on any profile.

See `docs/decisions/storage.md` for why a VFS inode cache was NOT built
on top of that: what it adds is one sector read per operation.

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
a resolution caused, and `QUERY_FSSTAT` reports it. Measured on both,
because the two disagree about the mix:

| profile | QEMU/KVM AHCI, 1 MiB | bare metal, 4 MiB |
|---|---|---|
| SEQ-write | 503 of 1712 reads (**29%**) | 1188 of 3063 (**39%**) |
| SEQ-read | 1033 of 1556 (**66%**) | 331 of 622 (**53%**) |
| RND4K-write | 1187 of 2493 (**48%**) | 232 of 1181 (**20%**) |
| RND4K-read | 854 of 1709 (**50%**) | 522 of 876 (**60%**) |

**Path resolution is between a fifth and two thirds of every block read
this filesystem issues**, and the driver is not depth but COUNT: a
64 KiB write syscall triggers several resolutions, each costing ~3-4
block reads. The hypothesis was right and the first experiment was built
on the wrong variable.

The two columns are not directly comparable -- different sizes, and the
laptop's own layout -- so read them as "large on both" rather than as a
delta. The one number worth taking from the hardware column on its own
is that a sequential WRITE spends 75 ms of its 197 ms of read time
simply finding the file again.

## The stages

Each one ships on its own and is worth having if the next never lands.

### Stage 0 -- cache the resolved path  [BUILT]

**Measured first (`QUERY_FSSTAT`, `/bin/diskbench`'s `lookup` line),
then built.** tfs3 caches whole path -> inode in a 16-entry per-mount
`lcache`, checked at the top of `lookup()`.

A/B on a freshly seeded disk each way, QEMU/KVM + AHCI, 2 MiB:

| profile | without | with |
|---|---|---|
| SEQ-write | 9.40 MB/s | **16.29** |
| SEQ-read | 15.93 | **173.84** |
| RND4K-write | 0.852 | 0.889 |
| RND4K-read | 1.53 | **6.63** |

**The reliable number is per-lookup block reads, which fell from ~3.9 to
1.2-1.8 across all four profiles** (1.0 is the floor -- the inode read
itself, which is never cached). Throughput multiples vary run to run,
SEQ-read especially, so read those as "large" rather than as exact.

**RND4K-write barely moved, and that is the expected answer**: it is
flush-dominated (67% of its block time on hardware), so removing reads
cannot help it. Only stage 2 can.

**On the bare-metal laptop the mechanism reaches its floor.** Same
benchmark before and after, 4 MiB, per-lookup block reads:

| profile | before | after |
|---|---|---|
| SEQ-write | 3.95 | 1.73 |
| SEQ-read | 3.01 | **1.00** |
| RND4K-write | 3.05 | **1.00** |
| RND4K-read | 3.00 | **1.00** |

1.00 is the floor -- the inode read that is deliberately never cached --
so on three of four profiles there is nothing left to remove.

**The hardware THROUGHPUT numbers are not a controlled comparison and
should not be quoted as one.** SEQ-write went 12.39 -> 14.48 MB/s, but
SEQ-read read 116.17 -> 56.05 and RND4K-read 6.24 -> 5.07 -- both within
the range this machine has shown across runs all day, and neither
measured with the A/B discipline the QEMU table above used (that would
mean reflashing the old kernel). The per-lookup column is the claim; the
throughput column is an observation with a caveat attached.

**Design, in three decisions.** The cache holds path -> inode NUMBER
only, never the inode's contents: those change on every write, so
caching them would need invalidating on the hot path instead of the rare
one. It is flushed from `ncache_flush()`, which create, delete, link,
rename and unmount already call -- a second entry point would have to be
added to all five, and the one that got missed would hand back another
file's inode. And a FAILED resolution is deliberately not cached,
because the thing that would make it wrong -- a create at that path --
is common.

**The test needed a decoy, and that is the part worth copying.** The
obvious check -- write a file, read it, delete it, recreate it, read it
-- PASSES with the invalidation disabled, because the recreated file is
handed the inode the deleted one just freed, so the stale entry is
accidentally correct. Creating a second file in between takes that inode
and forces the recreate onto a different one, which is the case a stale
entry gets wrong. Without the decoy the test proves nothing.

If more is wanted here later, the bigger version is: `struct open_file` holds
`char name[64]` and a position, so give it the resolved inode as well
and a read or write on an open fd skips resolution entirely. That needs
an `fs_ops` entry taking a resolved handle, or a VFS inode cache keyed
by path and invalidated on rename and delete -- the second being smaller
and bounded by the same correctness question stage 2 must answer anyway.

**Note what is NOT a target here.** `file_fd_write()` resolves twice
on an `O_APPEND` write, once for `fs_size()` and once for the write, and
its comment says why: re-reading the size is what makes two appenders
interleave whole writes instead of overwriting each other. That is a
semantic, not an oversight.

**Buys:** unknown until measured, which is the point of the stage.
**Does not buy:** anything about flushes.

### Re-measured after stage 0, and it REORDERS what follows

With path resolution cached, the same benchmark on the laptop (8 MiB)
attributes the block layer's time like this:

| profile | flush | read | write | lookup |
|---|---|---|---|---|
| SEQ-write | **56%** | 31% | 12% | **2.7%** |
| RND4K-write | **69%** | 25% | 5% | **1.0%** |

Lookup is finished as a cost -- it was ~38% of SEQ-write's read time
before stage 0 and is under 3% of all block time now. **Flush dominates
outright.**

**And the flush is buying almost nothing.** `do_write_inner()` writes
its data blocks DIRECTLY (`vol_write_sectors`), then writes the
allocation bitmaps (`flush_alloc_state()`, set-before-use), and only
then opens a transaction -- `txn_begin(1)`, one credit, staging the
INODE BLOCK and nothing else. So 56-69% of block time is two device
barriers per syscall, spent making a single 4 KiB inode block durable,
while the file's actual data reached the disk before the transaction
started.

**That can be batched WITHOUT a page cache**, which the original staging
here did not consider and which changes the order of the rest.

### Stage 1a -- batch the metadata commit  [BUILT]

Keep the journal transaction open across writes instead of committing
per call, and commit when the journal's 32 slots would overflow, when a
timer elapses, at `fsync`/`sync`/unmount, or when an operation needs the
journal for something else (create, delete, rename force it first).

**What a crash costs, stated exactly:** the data blocks and the
allocation bitmaps are already on disk, so what is lost is the INODE
UPDATE -- a just-extended file comes back at its old size and the blocks
past it are unreferenced. That is a LEAK, not corruption, and `fsck`
already reclaims leaks; "prefer a leak to a double-allocation" is the
ordering this filesystem was built on. It is much weaker than what
`storage.sync = lazy` risks, which is a journal that cannot be replayed.

**THE CONSTRAINT THAT DECIDES THE DESIGN:** `g_txn_img` is file-scope,
not per mount, and its comment says why -- "a transaction begins and
commits inside one op, so no second mount can be between them". Holding
one open across syscalls breaks that premise. Either the staging becomes
per-mount (+128 KiB a mount, against a mount that costs ~7 KiB) or the
VFS commits the open transaction whenever a DIFFERENT mount becomes
active. The second is cheap and is the one to build. (Stage 2 of
`docs/fslock-design.md` later made the staging per mount after all,
2026-09-24, because one lock per mount needed it.)

Gated behind `storage.sync` like everything else here, default `strict`
when written; built as `batched`, which became the default on 2026-09-04
(`kernel/lib/storage_config.c`).

**Buys:** most of the 56-69%, with no cache, no eviction and no reclaim
mechanism. **Does not buy:** repeat reads, which is what the page cache
below is actually for.

**Built. On the BARE-METAL LAPTOP, 4 MiB, both modes back to back:**

| profile | strict | batched | flushes |
|---|---|---|---|
| SEQ-write | 12.91 MB/s | **80.02** | 212 -> 84 |
| RND4K-write | 1.077 | **9.56** | 340 -> 84 |

**6.2x and 8.9x.** The same A/B in QEMU gives 11.95 -> 13.43 and
1.381 -> 2.636, because an emulated flush costs 95 us against the
drive's 659 -- the gap between the two columns IS the thing this stage
was built to remove, and it only exists on real hardware.

**AND IT BEATS `lazy`, which is the surprise.** `lazy` measured 36.55
and 4.01 on the same machine; `batched` is more than twice that on both
profiles while being far safer. The reason is that `lazy` skips the
BARRIERS but still performs every commit's WRITES -- journal data, the
header, the target, the header again, four block writes per syscall.
Batching removes the commits themselves, so it removes the writes too.
That makes `lazy` hard to justify for anything but measuring what a
barrier costs.

`fsck` reports 0 leaked blocks afterwards -- verified in QEMU, where
`fsck` is reachable; it is a ring-0 shell builtin, so a telnet session
to the laptop cannot run it. The flush count does not fall
further than that because the DESKTOP is writing throughout, and any
operation that opens its own transaction commits the deferred one on its
way past -- on a quiet machine the reduction is much larger.

**TWO BUGS IN THE FIRST VERSION, both found by measuring rather than by
reading, and both worth the space.**

The commit was forced on every operation and batching saved nothing.
`tfs3_state_activate()` committed whenever the incoming state differed
from the transaction's owner -- and `vfs.c`'s `FS_OP` DEACTIVATES after
every backend call, activating `NULL`. So "a different mount" was true
every single time. Nothing can reach the journal while no state is
current, so the fix is to ignore a NULL activation.

And a batched write was invisible to readers. The newest inode image
lives in the journal staging buffer while the disk still holds the
previous one, so `fs_size()` returned the size the file had BEFORE
writes that had already returned success, and `diskbench`'s read pass
died with a short read. The staged image is the current truth, so reads
consult it -- **at `vol_read_sectors()`, not at `read_block()`**, because
`read_inode()` reads ONE SECTOR rather than a block and a block-level
overlay missed the only read that mattered, without changing the
symptom.

**THE TEST THAT PASSED WITH THAT BUG IS THE LESSON.** It wrote eight
blocks in `strict` first and then re-wrote the SAME offsets in
`batched`, so the file's size never changed and a stale inode was
indistinguishable from a fresh one. The test that catches it EXTENDS the
file and checks the size after every step -- and a single extend-then-
read is racy, because anything else opening a transaction commits this
one on its way past, so it takes a dozen steps for one to land in a
window where nothing intervened.

### Where the time goes with 0, 1a, 3 and 4 all built

Bare-metal laptop, 8 MiB, sequential write, both modes:

| | strict | batched |
|---|---|---|
| throughput | 3.97 MB/s | **26.77** |
| read | 240 ms | 339 ms |
| write | 61 ms | 54 ms |
| **flush** | **1780 ms (85%)** | **301 ms (43%)** |
| lookup | 22 ms | 11 ms |

**Flush is STILL the largest term even in `batched`.** Not because the
count is high -- 92 against 632 -- but because each one now costs
~3.3 ms under sustained writing. Reads are 16% and lookup is under 1%.

**What further reduction would take, and it is not a page cache.** The
92 remaining commits are forced by OTHER operations: anything calling
`txn_begin()` commits the deferred transaction on its way past, and the
desktop writes constantly. Holding more than one transaction open at a
time would need per-transaction journal staging rather than the single
file-scope `g_txn_img`, which is a real change to the journal rather
than a cache above it.

So the honest state of stages 1 and 2 is: **the workload that motivated
them is not read-bound any more, and the remaining cost is in a place a
page cache does not sit.** They stay designed and unbuilt, and the
reason is a measurement rather than a preference.

### Stage 1 -- one cache, above the VFS, read-only first

**Demoted by the re-measure.** Reads are 25-31% of block time now and
the ones that remain are bitmap, group-descriptor and inode-table
traffic plus read-modify-write, not REPEATS -- and a cache pays for
repeats. A linear benchmark has few, so this stage would barely move
these numbers. It is worth building for real workloads that re-read, and
it is no longer the next thing.

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
anyone until they ask. (Since 2026-09-04 the default is `batched`.) That is what makes this landable incrementally
rather than as a single switch-flip nobody can bisect.

**Buys:** the flush cost -- the 53% and 67% above.

### Stage 3 -- `fsync()` and `fdatasync()`  [BUILT, scoped to the VOLUME]

`SYS_FSYNC` (96) takes an fd, commits the backend holding that file's
path and flushes the device under it. `fsync()` and `fdatasync()` in
`<unistd.h>` both call it.

**SCOPED TO THE VOLUME, NOT THE FILE, and that is the honest name for
what it does.** This document assumed fsync would arrive after the page
cache and inherit per-file granularity from it. It arrived before,
because stage 1a made it necessary -- `batched` gives a program no way
to force durability at a moment of its choosing, and `sync` is
whole-system. Nothing is held per file: the deferred transaction may
carry several files' inode blocks, and a device flush is a whole-drive
operation regardless. So it is narrower than `sync` (other mounts are
untouched) and wider than POSIX describes, and the ABI comment says so.

**`fdatasync()` is the same call, deliberately.** It may skip metadata
not needed to retrieve the data -- and what a deferred write holds back
IS the inode, so the metadata it is allowed to skip is exactly what has
to land for the bytes to be findable. There is no cheaper subset to
offer, and pretending otherwise would be a second name for one
behaviour.

Per-file granularity still needs the page cache below.

**IT ISSUES A DEVICE FLUSH, following Linux rather than macOS.**
macOS's `fsync()` pushes to the drive and does NOT flush its cache,
which is why `F_FULLFSYNC` exists and why every database on macOS has a
workaround for it. A call that returns without the data being on the
platter is a trap dressed as an optimisation.

### Stage 4 -- the writeback interval as a setting  [BUILT, with stage 1a]

`storage.writeback_interval` (seconds, 1-30, default 1), the visible
half of ext4's `commit=5` and Linux's `dirty_expire_centisecs`. It
landed WITH stage 1a rather than after it, because that stage created
the thing it bounds.

**It is not a nicety, it closes a hole.** `batched` as first written
committed only when another transaction opened, a second mount became
active, `sync` ran, or the volume unmounted -- so a machine that wrote a
file and was then left alone could hold that inode update
INDEFINITELY. The idle path (`fs_ops.idle` -> `tfs3_idle()`, called from
`scheduler_idle()` beside `atac_idle()`) bounds how long; the journal's
slot ceiling already bounded how much. Both halves, the same shape the
sector cache has had all along.

The setting reports itself UNAVAILABLE outside `batched`, with the
reason, rather than offering a spinbox that does nothing: in `strict` a
write commits before it returns, and in `lazy` nothing defers either.

**AND THE TEST FOR IT PASSED WITH THE IDLE PATH DISABLED**, which is the
second time on this feature that the obvious assertion was satisfied by
somebody else's work. "A commit happened after waiting" is true whether
or not the idle path exists, because anything that opens a transaction
commits the deferred one on its way past and the desktop is always
writing. It counts the idle path's OWN commits now
(`tfs3_idle_commits()`), which is the only way to tell the two apart.

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


## Re-measured 2026-09-13, after the path cache and the bigger transactions

Bare-metal laptop (Samsung MZNLN128 on AHCI), 64 MiB,
`/var/tmp/diskbench.tmp` -- a three-component path, which is the case
that used to be penalised. `storage.sync` as marked.

| profile | before, `lazy` | after, `lazy` | after, `batched` |
|---|---|---|---|
| SEQ write | 34.8 | 104.4 | 140.9 MB/s |
| SEQ read | 89.7 | 133.9 | ~151 MB/s |
| RND4K write | 3.13 | 6.63 | ~16.5 MB/s |
| RND4K read | 9.37 | 10.93 | ~13.1 MB/s |

**THIS MACHINE THROTTLES, AND THE SPREAD IS LARGER THAN SOME OF THE
EFFECTS BEING MEASURED.** The figures above are from runs taken shortly
after a boot. The same build after roughly a gigabyte of back-to-back
benchmarking measured 94.8 / 104.1 / 11.3 / 9.3 -- a third down across
the board, with no code change. It is a fanless Core M. Compare runs
taken at the same point in a session, never a fresh run against a hot
one, and take at least three.

**Where the time goes now** (`batched`, cool, 64 MiB sequential write,
256 syscalls): 623 block-layer write commands moving 133,360 sectors
against 131,072 of data -- 1.018x amplification, down from 1.33x -- and
~960 read commands moving only ~2,450 sectors. Block-layer time is
roughly half the wall clock; the rest is the two copies and the syscall
itself.

**What this reorders.** The three remaining candidates, by measured
headroom rather than by how interesting they are:

1. **The copies and the per-syscall buffer.** The block layer moved
   319 MB/s during a sequential read the caller saw as 133. Every file
   read/write `kmalloc`s a bounce buffer, and the AHCI driver `memcpy`s
   through a second one.
2. **The pointer tables**, which still flush per write even in
   `batched` (the bitmap and group descriptors no longer do).
3. **NCQ and read-ahead**, which both still wait on the asynchronous
   `block_device` split and buy least at QD1 sequential.
