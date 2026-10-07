# Decisions: Filesystem and storage

TFS2 and TFS3 on-disk layout, the block layer, the journal, and what the VFS does and refuses to do.

Part of **[docs/decisions.md](../decisions.md)**, which indexes every
decision in this project and is GENERATED from these files -- run
`tools/gen_decisions_index.py` after adding an entry here, or
`tools/check_docs.py` will fail.

Write the reasoning HERE, in full: an entry that cannot be understood
without opening something else is not finished.

---

## `/tmp` is a ramfs mount and `/var/tmp` is the disk, and the split is not cosmetic

`/tmp` was an ordinary directory on the TFS3 root: persistent, uncapped,
never emptied. It is a mount point now, with a ramfs over it put there
at boot by a one-shot service (`data/etc/services.d/tmpfs`).

**What real systems do.** Linux has mounted `/tmp` as tmpfs by default
for years (systemd's `tmp.mount`), keeps `/var/tmp` on the disk for
scratch that must survive, and puts runtime state in `/run` — three
directories because they are three different promises. Windows has none
of this: `%TEMP%` is a plain NTFS directory and the pagefile backs
anonymous memory only. macOS is the same shape as Windows. toy-os
follows Linux, because Linux is the only one of the three that has the
problem — a machine where scratch I/O is worth not paying for.

**Why it is a SERVICE and not `fs_init()`.** Mount policy is userland's
on Linux (fstab, `tmp.mount`) and there is no reason for it to be the
kernel's here: the descriptor is a file, turning it off is deleting
that file, and the kernel learns nothing new. The cost is that `/tmp` is
a plain disk directory for the first fraction of a second of boot, which
nothing observes.

**The size has three sources and the DEFAULT IS THE SUBTLE ONE.**
`mount -o size=` wins, then `storage.ramfs_size`, then half of free
memory — tmpfs's own default. That last step is not a mere fallback: a
diskless boot mounts its ROOT as ramfs from `fs_init()`, before `/etc`
is readable and before `storage_config_init()` has run, so the compiled
default is what that root sees. **Any non-zero default silently shrinks
a diskless root to it**, which is why the setting's default is 0 and why
that is written down in three places.

**The option is a typed field, not an options string.**
`abi/mount_abi.h` argued against a `data` string on the grounds that no
backend had per-filesystem options; ramfs is the first that does, and
the answer is a `size_mib` in the struct rather than a parser. A number
needs no parser and cannot be mistyped into something that silently
means nothing. It fit in what was already a `reserved` word, so the ABI
did not change size — which is exactly what that word was for.

**What this broke, and the general lesson.** Six KTESTs failed
immediately, all of them using `/tmp` as *disk-backed* scratch: the
block-stat counters stopped moving, the `storage.sync` tests could not
tell `strict` from `lazy`, and `cwd_test` got ramfs's error code for a
refused hardlink instead of TFS3's. They were relying on a property
`/tmp` should never have carried. Three more would have been worse
because they would NOT have failed: `diskbench`, Disk Benchmark and the
shell's `stress` measure the disk by writing a real file, and against a
ramfs they would have reported an enormous, entirely meaningless number
with nothing about it looking wrong. `remote.py`'s sync checksums are
the fourth — that file exists precisely to still be there next run.

So: **when a directory's guarantees change, the things that break
loudly are the lucky ones.** The dangerous callers are the ones whose
assumption was about performance or durability rather than correctness,
because they keep working and start lying. Grepping for the path is what
finds them; nothing else does.

**THE LOCATIONS ARE SETTINGS, AND NOTHING SPELLS THEM OUT.** Thirty-odd
files named the two directories literally, which made moving either a
grep and made picking the wrong one a silent bug. They are
`storage.tmpdir` and `storage.vartmpdir` now, read through one API
(`api/tmppath.h`) whose join half is shared source compiled twice --
the same seam `geom.c` and `klineedit.c` use, with only "where does the
directory come from" differing per ring. That is what makes it
impossible for a KTEST and a ring-3 program to disagree.

**Why a BUILDER rather than an accessor.** `tmppath()` does the join
and REFUSES if the result will not fit, rather than returning a
directory each caller concatenates for itself. Every caller-side path
buffer here is 64 bytes, and making the prefix configurable is exactly
what turns truncation from impossible into likely -- `/tmp/x` fits where
a hand-set `/mnt/scratch/deep/x` may not. One place answers that
question instead of thirty.

**What it cost, which is the part to know before doing this again.** A
path built at runtime cannot be concatenated with a string literal, so
every `PATH "/child"` had to become an argument (`PROBE_UNDER("/child")`,
`SUB_AT("/made")`) and every `"created " PATH` message had to become
plain text. That conversion, not the API, was the bulk of the work.

**And it exposed a latent bug the literal had been masking.**
`setting_test.c` registers one descriptor from two tests, and only one
of them set `.file`; as a compile-time constant that was always right,
and as a runtime assignment it left the other NULL. The registry's
duplicate check compares `a == b` first, so two NULLs read as "the same
file" and it refused a registration that should have succeeded. A
constant folded into the binary hides the question of who initialises
it.

**`%T` and `%V` in a service descriptor** are systemd's specifiers for
these same two categories, expanded by init before the spawn. A
descriptor naming a configurable directory literally would stop agreeing
with the setting the moment anyone changed it -- and the `tmpfs` service
is the case in point, since it is what mounts the volatile one.

**`tmpfile()` and `mkstemp()` were considered and deferred.** They are
the POSIX answer to "I want scratch and do not care where", they belong
in tolibc on its completeness bar, and they do not solve this problem:
`mkstemp` takes a template, so the caller still writes a path. They are
a roadmap item rather than part of this change.

**And init's own channel had to move.** `init.c` put its control file
and status in `/tmp` with a comment saying it wanted `/run` and that
`/tmp` was the only such directory. That stopped being tenable the
moment `/tmp` became a mount point one of init's own services mounts:
init's control channel cannot live under a filesystem init is
responsible for putting there. `/run` exists now.

## `SYS_WRITE_MAX` is a throughput constant, because every write is a journal commit

It was 1024, described in its own comment as "an artefact of the bounce
buffer the syscall copies through, not a promise to the caller". That is
true and it buried the more important fact: **the cap decides how many
cache flushes a megabyte of ring-3 writing costs.**

Each `fs_write*()` call is one complete TFS3 transaction, and
`txn_commit()` ends with TWO `blkdev_flush()` barriers -- real flushes
reaching the device. `do_write()` commits ONCE for the whole range it is
handed, however large. So the arithmetic is:

| writer | bytes per call | transactions per MiB | **flushes per MiB** |
|---|---|---|---|
| `stress` (ring 0, `fs_write_range`) | 1 MiB | 1 | **2** |
| ring 3, cap at 1 KiB | 1 KiB | 1024 | **2048** |
| ring 3, cap at 64 KiB | 64 KiB | 16 | **32** |

That is the whole reason a ring-3 write measured ~30x slower than the
same bytes written from the kernel shell, and it was invisible because
nothing compared the two until `/bin/diskbench` existed. Measured on
KVM + virtio, 16 MiB: sequential write **3.4 -> 114.3 MB/s**, sequential
read 12.2 -> 76.2, and RND4K -- whose 4 KiB operation was FOUR syscalls
and is now one -- 867 -> 3938 IOPS. The whole run went from 7 s to 1 s.

**Two things this is not.** It is not the architectural answer: Linux
and Windows are fast because they do not flush on write at all -- a
`write(2)` lands in the page cache and returns, writeback happens on a
timer (`dirty_expire_centisecs`, 30 s), and the journal commits every
~5 s (`commit=5`) with one barrier for thousands of operations.
toy-os makes a STRONGER promise -- every write durable when it returns
-- and pays roughly a thousandfold for it. Closing that gap is a
write-back cache, not a bigger constant. And it is not free: a 64 KiB
buffer wants sixteen CONTIGUOUS frames, so `fd_bounce_alloc()` asks for
what the call actually needs and halves down to a 1 KiB floor rather
than failing, which is a short transfer and every caller already handles
one.

**THE TRAP IT SPRANG, and it is the reason to write this down.** Raising
the constant nearly deadlocked pipes. `pipe_write()` is all-or-nothing:
a write that does not fit takes none of the bytes and PARKS the writer
to retry the whole thing. That is safe only while one write can always
eventually fit -- which held for free while 1024 < `PIPE_BUF_SIZE`
(4096), with nothing enforcing it and a header comment stating it as a
happy consequence. At 64 KiB a writer would park on a request the pipe
could never satisfy. `pipe_fd_write()` clamps explicitly now.
The general shape: **a constant three files away was load-bearing for
an invariant nothing checked**, and the comment that noticed the
relationship described it as a coincidence rather than a requirement.


## A filesystem talks to a BLOCK DEVICE, and persistence is the device's answer

TFS3 called `ata_*` directly, which was fine while a disk was the only
thing a filesystem could live on. A Live CD mounts an image the
bootloader handed over as a GRUB module, with no ATA controller
involved, so the choice was a block-device abstraction or an
`if (live) ... else ...` at every call site -- and the second shape rots
predictably: one path gets tested and the other is found broken later.

`struct block_device` (`kernel/include/kernel/block.h`) is the same
registry pattern `display_driver` and `fs_ops` already use here. Five
required operations, two optional ones behind capability bits, one
active device. TFS3 moved in 15 call-site substitutions because it
already funnelled everything through two functions for partition
support; **TFS2 deliberately kept its 24 direct `ata_*` calls**, since a
live image is always TFS3 and rewiring a legacy backend to serve a
feature it will never carry is cost with no return.

**Capabilities are declared and refused at registration**, per the
display_driver rule: a device claiming `BLK_CAP_FLUSH` with no `flush()`
is rejected, and so is a `flush()` with no bit. A device that needs a
flush and silently never gets one turns the journal's two barriers into
no-ops, which is a corruption bug that surfaces long after the mistake.
On a RAM device both optional operations are genuinely absent and the
block layer turns them into no-ops -- correct, not a degradation, since
nothing there can be lost independently of everything else.

**The part worth remembering: persistence belongs to the DEVICE.** The
VFS computes `g_persistent = fs->init() && blk_persistent()`, because a
backend cannot tell -- TFS3 mounts a RAM image exactly as it mounts a
disk, and asking it would have reported a live session as persistent.
`df`, `fsck` and the About window all repeat that answer to the user, so
the single most misleading thing this feature could have done was let a
live volume claim your files were safe. It says
`tfs3 (RAM-only -- won't survive reboot)` instead.

## The filesystem is not re-entrant, so the VFS holds a preemption guard

`kernel/fs/tfs3.c` walked directories, inodes and file data through
module-level scratch buffers (`g_blk`, `g_ptr_blk`; per mount since
stage 2 of `docs/fslock-design.md`). That is fine for a
filesystem only one thing uses at a time, and this kernel is not that:
the kernel context is a scheduler participant and a ring-3 process is
preemptible inside a syscall, so the WM reading a file and an app
reading a file interleave at any instruction. The app's read then
overwrites the block the WM is parsing.

It does not look like a filesystem bug from outside, which is why it
survived so long. The WM reported files that plainly exist as missing or
unreadable -- the desktop loading "5 of 6" cursor shapes on roughly one
boot in three under KVM, silently, because a shape that fails to load
falls back to the built-in one. No error was logged anywhere, and 24 MB
through `stress` verified byte-for-byte under the same contention, which
ruled the disk out.

So `vfs.c` holds `scheduler_preempt_disable()` across every backend call
(`FS_OP()`/`FS_OP_VOID()`). It is at the VFS because that is the one
place every caller passes through; per-backend guards would have to be
repeated and eventually forgotten. The primitive itself is general
rather than filesystem-specific -- the hazard is "shared state plus
preemption", and the filesystem is only where this project met it first.

Three things worth knowing. It does NOT make a `fs_list()` callback safe
to call `fs_*` from: that is direct recursion, and a depth counter
cannot tell it from the safe case. It is NOT the same as `fs_read()`'s
nested-read refusal, which protects one buffer during one call while
this protects every backend's internal state for the whole call. And an
unbalanced `disable()` hangs the machine, since nothing would ever
rotate again -- which is why `scheduler_preempt_enable()` clamps at zero
rather than letting the count go negative and silently disarming the
NEXT legitimate section.

## `fs_read_into()` reads into the CALLER's buffer; `fs_read()`'s pointer is not preemption-safe

`fs_read()` hands back a pointer into one shared staging buffer, valid
"until the next `fs_read()`/`fs_write()`". That contract is unstatable
in a preemptible kernel: the *caller* can honour it perfectly and still
lose the buffer, because a ring-3 process can make a file-reading
syscall while a kernel-side parse is still walking it.
`userland/wm/cursor_theme.c` did exactly that, with a comment reasoning that
its parse happens before anything else touches the filesystem -- true of
the function, and not of the machine.

`fs_read_into(path, buf, cap)` is the fix: `fs_size()` plus
`fs_read_range()` into memory the caller owns, so there is no shared
buffer anywhere in the path for a concurrent reader to invalidate. It
refuses a file larger than the buffer rather than truncating, because a
half-read config file parses as a valid config file with keys silently
missing.

`fs_read()` stayed at first, for the one-shot callers, and "prefer"
converged nothing: two weeks later the kernel had seven live callers of
it and two of `fs_read_into()`, the scheduler's ELF loader among the
seven -- which parses an image in a context a ring-3 file read can
preempt, the exact exposure. **So `fs_read()` was deleted (2026-09-03)**
along with the nested-read refusal that guarded it: the small readers
got a static buffer sized for their file, the ELF loaders `kmalloc` at
`fs_size()` and free after `elf_load()`, and `stat` asks `fs_size()`
instead of reading the whole file to print a number. The backends'
`read` op and the staging buffer each of the three kept for it went the
same day: `read_range()` into caller memory is the only read there is.
The preemption guard above and `fs_read_into()` are complementary, not
alternatives: the guard protects the backend DURING a call, this removes
the shared buffer AFTER it returns.

## The read path caches pointer tables per LEVEL and drops them on any write

`read_range_impl()` called `block_for_index()` once per 4 KiB block, and
that function re-read the indirect table from the device every time. The
write path had solved the same problem years earlier with `pcache` and
run coalescing (the 18 -> 25 MB/s commit); the read half was simply
never done, and nothing measured it because a MB/s figure does not say
how many commands produced it.

Measured on QEMU/KVM with AHCI, 16 MiB: a sequential read issued 13189
block-device calls moving 93689 sectors to deliver 32768 sectors of
data -- 3.2 commands per block and 2.9x sector amplification, because a
file past 4 MiB is double-indirect and every block re-read both tables
above it. Fixing it took the same run coalescing the write path has plus
a table cache: 1385 calls, 34565 sectors, and 6.87 -> 62.7 MB/s.

**Per LEVEL, not one entry.** A walk touches top, then mid, then leaf.
A single slot evicts the level above on every step, so three reads per
block become three misses per block -- which is what the uncached code
already did. Three entries, numbered from the LEAF so a single- and a
triple-indirect walk agree about which slot a leaf occupies; numbering
from the top puts the leaf at a different level per depth and evicts it
on every step.

**Per mount, and it was wrong the first time.** It began file-scope,
"per call" on the reasoning that one global critical section kept a
second mount from being between a load and its use -- but nothing
dropped it between CALLS, only on writes, so a read on a second TFS3
mount could be served the first's table at the same block number.
`tfs3_test.c` reproduces it against the old code; stage 2 of
`docs/fslock-design.md` moved it into the mount (12 KiB each).

**Invalidated by ANY write, in `vol_write_sectors()`.** This is the
whole safety argument and it is deliberately blunt: an entry may only
hold what is on the device, and anything cleverer needs to know which
blocks are pointer tables. Being wrong once means a read served from a
stale table returns another file's data. It sits at `vol_write_sectors`
rather than `write_block` because a coalesced data run goes straight to
the device, and a block that was a pointer table before it was freed and
reused as data would otherwise still be cached under its old number.

**What this did NOT fix**, stated because the measurement says so:
random 4 KiB reads gained almost nothing (3.41 -> 3.67 MB/s) -- they
cannot coalesce and each seek lands in a different leaf -- and the
sequential WRITE path was unchanged, because its metadata reads go
through `map_get_or_alloc_tables()` and the allocator, not through
`block_for_index()`.

**The positive control is the part worth keeping.** Disabling the
invalidation left all 640 KTESTs green -- the same shape as the truncate
tests that stayed green with the indirect boundary handling disabled,
and for the same reason: nothing wrote a file big enough to load a table
and then re-read it. The test that closes it grows a file across the
direct-pointer boundary, reads it back (which is what ARMS the bug),
appends, and requires the appended bytes -- a stale table answers with a
zero pointer, which reads as a hole, so the data comes back blank.


## tfs3 caches whole path -> inode, and the invalidation rides the name cache

Every `fs_*(path, ...)` call resolved from the root: one uncached inode
read per component plus a directory scan. Measured at a fifth to two
thirds of every block read the filesystem issued -- and the driver is
COUNT, not depth, since one 64 KiB write syscall triggers several
resolutions. `lcache` (16 entries per mount, 64-byte keys) answers the
whole question.

Per-lookup block reads fell from ~3.9 to 1.2-1.8, floor 1.0. On a clean
A/B: sequential write 9.40 -> 16.29 MB/s, sequential read 15.93 -> 173.84,
random 4 KiB read 1.53 -> 6.63. Random 4 KiB WRITE did not move, which
is the expected answer -- it is flush-dominated, and no amount of read
saving touches that.

**The number only, never the inode's contents.** Size, mtime and block
pointers change on every write, so caching the struct would mean
invalidating on the hot path; the path -> number mapping changes only
when the namespace does.

**Flushed from `ncache_flush()` rather than through its own entry
point.** Create, delete, link, rename and unmount already call it, and
those are exactly the operations that can change which inode a path
names. A separate function would have to be added to all five, and the
one that got missed would return another file's inode -- silently, with
the right byte count.

**A failed resolution is not cached.** The thing that makes a negative
entry wrong is a create at that path, which is common; one stale "no
such file" is worse than every miss it saves.

**THE TEST NEEDED A DECOY, and this is the transferable part.** The
obvious check -- write, read, delete, recreate, read -- PASSES with the
invalidation disabled: the recreated file is handed the inode the
deleted one just freed, so the stale entry is accidentally correct. It
took creating a second file in between, to claim that inode, before the
recreate landed on a different one and the test could fail. The general
form is this project's oldest testing rule, arriving from a new
direction: ask what a broken version would still pass, and here the
answer was "all of it", because the allocator was quietly repairing the
bug.


## `sync` flushes every mount through the block layer, not the ATA cache

`sys_sync()` opened with `if (!ata_cache_active()) { return 0; }` and a
comment arguing that with no write-back cache "every write has already
reached [the disk]". That is true of the DRIVE and false of the
PLATTER: the bytes were in the drive's own volatile cache, and only a
device flush moves them. So on AHCI and virtio-blk -- every modern
machine, and the bare-metal laptop -- `sync` asked the disk for nothing
and returned success.

`fs_sync()` (vfs.c) does both stages now: write back a driver's software
cache where one exists, then `blkdev_flush()` **every mounted volume**,
whatever backend is on it. Filesystem- and driver-agnostic, because the
block layer already knows how to flush whatever is under a mount and the
filesystem never needed to come into it.

**It became a `fs_*` function rather than staying in the syscall**
because it has three callers coming: `sync`, a KTEST that can only tell
a flush from a no-op by counting device flushes, and `fsync`.

**Deduped by device, and two partitions of one disk still cost two
flushes.** A partition forwards flush to its parent (block_part.c) and
nothing above can see that it did. Correct, merely not minimal -- and a
flush is ~0.7 ms on real hardware rather than free, so this is worth
knowing before mounting many partitions of one disk.

**Zero sectors is not "did nothing", and `/bin/sync` had to stop saying
it was.** The count is stage 1 only, so a machine with no software cache
legitimately reports zero while the flush was the entire point. "nothing
was pending" reported a working sync as a no-op on exactly the machines
where it had just started working.

**THE REASON IT SURVIVED IS THE TEST MATRIX, and that is the
transferable part.** `ktest_run.py` defaults to ATA and CI runs ATA and
virtio-blk; **nothing automated boots AHCI**, which is what real
hardware uses. Restoring the old early return makes the new KTEST pass
on ATA and fail on AHCI -- a suite that only runs the backend WITH the
cache cannot see a bug in the ones without it. On the roadmap now.


## `storage.sync = batched` defers the COMMIT, and the data is already on disk

After the resolved-path cache, flush was 56-69% of block time on real
hardware -- and it was buying almost nothing. `do_write_inner()` writes
its data blocks directly, then the allocation bitmaps, and only then
opens a transaction: `txn_begin(1)`, one credit, staging the INODE BLOCK
alone. Two device barriers per syscall, to make one 4 KiB block durable,
after the file's data had already reached the disk.

`batched` keeps that transaction open across writes. On the bare-metal
laptop, 4 MiB, both modes back to back: sequential write **12.91 ->
80.02 MB/s** and random 4 KiB write **1.077 -> 9.56**, with flushes
212 -> 84 and 340 -> 84. The same A/B in QEMU gives 11.95 -> 13.43 and
1.381 -> 2.636 -- an emulated flush costs 95 us against the drive's 659,
and that gap is exactly what this stage removes.

**It also beats `lazy` by more than 2x** (36.55 and 4.01 on the same
machine), which was not expected. `lazy` skips the BARRIERS but still
performs every commit's WRITES -- journal data, header, target, header
again. Batching removes the commits, so it removes those too. `lazy` is
now hard to justify for anything except measuring what a barrier costs.

`fsck` clean afterwards, verified in QEMU; it is a ring-0 shell builtin,
so a telnet session to the laptop cannot run it.

**What a crash costs, exactly.** The data and the bitmaps are on disk;
what is lost is the inode update, so a just-extended file returns at its
old size with the blocks past it unreferenced. That is a LEAK, which
`fsck` reclaims, and it is the ordering this filesystem already chose
("prefer a leak to a double-allocation"). Far weaker than `lazy`, which
risks a journal that cannot be replayed at all.

**The commit is forced in `txn_begin()`, not at the call sites.**
`txn_begin()` zeroes `txn_count`, so any operation opening its own
transaction while one was deferred would silently discard every inode
staged in it -- writes reported as succeeded, gone. Forcing it in the
one function every transaction passes through covers create, delete,
rename and truncate without editing them, and a new operation cannot
forget.

**A NULL activation is not a mount switch, and getting that wrong made
the whole feature a no-op.** The deferred transaction belongs to one
mount because `g_txn_img` is file-scope, so activating a DIFFERENT mount
must commit it first. The first version tested `owner != st` -- and
`FS_OP` deactivates after every backend call by activating NULL, so that
was true every single time and every write still committed. Nothing can
reach the journal while no state is current, so a NULL activation is
ignored. (Moot since stage 2 of `docs/fslock-design.md`: the journal is
per mount, and nothing activates anything.)

**Readers consult the staged image, at `vol_read_sectors()`.** The
newest inode lives in the staging buffer while the disk holds the
previous one, so without an overlay `fs_size()` reports the size from
before writes that already returned success -- `diskbench`'s read pass
died with a short read. It has to sit at `vol_read_sectors()` rather
than `read_block()`, because `read_inode()` reads ONE SECTOR and a
block-level overlay missed exactly the read that mattered while leaving
the symptom unchanged.

**AND THE TEST THAT PASSED WITH THAT BUG IS THE PART TO REMEMBER.** It
wrote eight blocks in `strict`, then re-wrote the SAME offsets in
`batched` -- so the file's size never changed and a stale inode was
indistinguishable from a current one. Catching it needs a test that
EXTENDS the file and checks the size after every step, and even then a
single step is racy, because anything else opening a transaction commits
this one on its way past. A dozen steps is what makes one land in a
window where nothing intervened.


## `storage.sync` is a setting because the barriers' cost cannot be measured where they are cheap

TFS3 commits one journal transaction per `fs_write*()` call and ends it
with two real device flushes. At 64 KiB a syscall that is 32 flushes per
MiB written, and 512 per MiB at 4 KiB. Linux and Windows do not flush on
write at all -- a `write(2)` lands in the page cache and returns,
writeback runs on a timer, and the journal commits every few seconds
with one barrier for thousands of operations. toy-os makes the stronger
promise and pays for it.

**How much it pays was the part nobody knew, and it is now measured on
both.** On QEMU/KVM with AHCI, 16 MiB sequential write: 622 flushes
costing 59 ms out of 1288 ms of block-layer time -- **4.6%**, i.e.
almost nothing. On the bare-metal laptop (AHCI, SATA SSD) the same
benchmark says:

| profile | flush time | share of block I/O | `strict` | `lazy` |
|---|---|---|---|---|
| SEQ-write | 393 ms (596 calls) | **53%** | 14.7 MB/s | **36.6** |
| RND4K-write | 730 ms (1108 calls) | **67%** | 1.09 MB/s | **4.01** |

**One flush costs 659 us on the drive against 95 us emulated** -- 7x,
and **2.8-3.3 ms once the drive has been writing for a while**, so the
ratio understates it for exactly the workload that issues the most
barriers --
and it is a fixed cost per transaction rather than per byte, which is
why the 4 KiB profile suffers most. That single ratio is the whole
reason this is a setting and not a constant somebody could have tuned
from a QEMU run: the term that dominates on the hardware is the one the
emulator very nearly hides.

(Read throughput moved 87.1 -> 63.5 MB/s across those two runs, which is
run-to-run variance and not an effect: reads issue 84 flushes to the
writes' 596, and the read call counts were within 3%. Quoted here so
the number is not mistaken for a regression later.)

**`strict` and `lazy` are ext4's `barrier` and `nobarrier`**, and the
warning is the same. Both barriers are load-bearing: the first orders
the journal against the targets so a crash mid-target-write can be
replayed, the second orders the targets against clearing the commit flag
so a crash cannot leave the journal saying "nothing to do" over work
that never landed. `lazy` therefore risks a filesystem replay cannot
repair, not merely the loss of recent writes. It is meant for a device
whose cache is battery-backed, or for a measurement.

**One function, `txn_barrier()`, not a flag tested at each call.** Both
sites go through it, so neither can be given the exemption separately --
there is no coherent middle position where one barrier is skipped, and
a reader of `txn_commit()` should not have to work that out.

**It returns 1 when it skips.** A caller must not read "no barrier was
issued" as "the barrier failed", because failure ABANDONS the
transaction -- doing that on every write would be a filesystem that
refuses to write at all.

**Default `batched` since 2026-09-04, and an unparseable value stays
STRICT.** The default moved on the measurement above; the fallback did
not, because a value nobody can parse is not a request for less
durability. Every other
`/etc` reader here tolerates a bad value by keeping its default; this
one is the same rule pointed the safe way, because the value trades
correctness for speed. `storage_sync_strict()` also answers 1 before
`/etc` has been read at all, which matters because mount and journal
replay both run before `INIT_CONFIG`.

**The test asserts the flag reaches the JOURNAL, not that a string
parses.** The whole setting is one branch in `txn_commit()`, and a flag
nothing reads is exactly what a parse test cannot see -- so it counts
real device flushes across identical work in each mode, through
`QUERY_BLKSTAT`. It compares the two modes rather than checking an
absolute count, because the desktop is running and flushing on its own.
It also reads the bytes back: a mode that quietly lost them would
satisfy a flush comparison perfectly.


## The write walk caches middle pointer tables too, and its comment had claimed it already did

`map_get_or_alloc_tables()` walked the middle levels with a read and a
write-back INSIDE the per-block loop, so every data block written past
4 MiB paid a table round trip. The comment above it said "rare compared
to leaf patches -- one RMW per 1024 (or 1024^2) data blocks", which
described the intended design rather than the code: `pcache` gave the
LEAF that property and the levels above it never got one.

Measured A/B on a freshly seeded disk, KVM + AHCI, 16 MiB sequential
write: 7.08 -> 9.31 MB/s, read sectors 53223 -> 26990, read calls
13598 -> 9761. Write calls (2427 -> 2426) and flushes (622 -> 622) are
unchanged, which is the internal control -- the change is confined to
the read side of the write path, as intended.

**Folded into `pcache_flush()`/`pcache_drop()` rather than given its
own pair.** Both caches have exactly the same lifetime -- one
filesystem operation -- and there are nine call sites that drop the
leaf cache, including the steppable-write path. A second pair would
have to be added to every one of them, and the one that got missed
would lose a middle table's pointer silently.

**A fresh table is dirty the moment it is loaded**, which preserves what
the old walk did with an unconditional write whose comment said it
"can't happen": a middle table allocated but never written into must
still reach the disk, or the level below it is unreachable.

**The reason this could be rewritten with the suite green is the
coverage gap the roadmap already named.** Nothing wrote a file past the
single-indirect table, so the entire double- and triple-indirect walk
was untested. That entry assumed reaching them meant a file of tens of
megabytes and a host-side tool; writing SPARSELY costs the pointer chain
and one data block instead, which is how TFS2's removed selftest reached
4.6 GB, so both are ordinary KTESTs now. The positive control matters as
much as the tests: disabling the dirty marking reddens both, and the
FIRST version of the triple test passed anyway -- its chain was
allocated entirely fresh, and a fresh table is written whatever else is
broken. It needed a second range under an EXISTING middle table before
it could fail.


## The block layer counts TIME per operation, not just calls

`QUERY_BLKSTAT` records calls, sectors, nanoseconds and failures for
read, write, flush and trim, timed around the driver call in
`block.c`'s four `io_*` helpers -- the one place `blk_*`, `blk_disk_*`
and `blkdev_*` all funnel through, so a caller can be counted neither
twice nor not at all. `/bin/diskbench` snapshots it around each profile
and reports the delta.

It exists because three unrelated things make a disk slow here and
throughput tells them apart from none of them: commands too small,
commands too many, and cache flushes. A flush moves no sectors and can
still dominate -- and it is nearly free on emulated hardware while a
real SSD must push DRAM to NAND, which is precisely how a number
measured in QEMU gets believed about a laptop. The first run with it
attached showed a sequential WRITE spending 79% of its time in READS,
which no throughput figure would ever have said.

**Always on, like `net_device`'s counters.** Two clocksource reads per
operation is nothing beside a disk command, and a counter that must be
enabled first is a counter nobody has when they need it.

**A failed call is still timed.** A command that timed out is the most
expensive one the layer issues; dropping it would make a disk look
faster the worse it was behaving.

**Deltas, not a reset.** Reset exists for tests, but ring 3 subtracts
two snapshots: a reset would be a WRITE to a class that is a fact, and
two readers of it must not be able to blank each other's baseline.

**The trap it exposed, which is about the CLOCK rather than the disk.**
In every default QEMU configuration toy-os runs on the PIT, because a
guest is not offered an invariant TSC unless the CPU model says
`+invtsc` -- QEMU masks it since it blocks migration. The PIT cannot
resolve one driver call, so every duration reads as zero and the
profiler confidently reports that nothing costs anything. `diskbench`
prints the observed `clock-granularity-ns` so that case names itself,
and the KTEST for the counters SKIPS its timing assertion rather than
failing on it, saying which clocksource it needed. Use
`vm.py --kvm --cpu host,+invtsc` for a real measurement.


## The disk cache is under the ATA DRIVER, not the block layer -- and its flush can fail

The block layer is the tidier home for a cache and it is the wrong one.
TFS2 makes seven direct `ata_*` calls and `partition.c` three more, so a
cache in the block layer would sit beside two bypass paths -- and a
bypass past a WRITE-BACK cache is a correctness hole in both directions:
the bypassing reader sees a stale sector, and a later write-back
overwrites what the bypassing writer put there. Both are silent.
`ata_read_sectors()`/`ata_write_sectors()` are the one place every
caller in this kernel funnels through, so caching there means no bypass
path can exist to get wrong. The cost is that it lives in a driver
rather than a layer, and a RAM-backed live image gets no cache -- which
is right anyway, since its "I/O" is already a memcpy.

**The flush had to learn to fail.** A write-back cache means a write
that returned success may be refused LATER, at the flush -- and
`blk_flush()`, `ata_flush_now()` and `struct block_device`'s flush op
were all `void`. TFS3's journal is only safe because its two barriers
mean "everything before this is on the platter"; a barrier that cannot
fail cannot say otherwise. All three return a status now, and
`txn_commit()` checks it: barrier 1 failing ABANDONS the transaction
rather than overwriting targets, because a half-written target with no
durable journal behind it is unrecoverable while abandoning costs
nothing that was not already lost. A failed write-back keeps its line
DIRTY rather than dropping it, so the data is still there to retry.

Sizing and shape, briefly: 4-way set-associative, 512 sectors, per-
SECTOR lines (TFS3 issues 1- and 8-sector transfers and a partition
offset can unalign an 8-sector one, so a larger line would need
alignment reasoning per-sector lines do not have). Transfers over 8
sectors bypass the cache and reconcile overlapping lines first, so bulk
file data cannot evict the metadata the cache exists to hold. Flushes
happen at the journal's barriers, at a dirty-line threshold, on an idle
timer via `scheduler_idle()`, at shutdown, and on `sync`.

**Fault injection sits at the public entry AND on the write-back path.**
With a cache in front, "the drive refused this write" no longer
necessarily happens during the caller's `write()` at all, so a test
could otherwise arm a failure, dirty a line, flush, and watch the flush
report success.

## TFS3's last block group may be partial, like ext2/3/4's

TFS3 divided a volume into whole 128 MiB block groups by floor
division, so the smallest filesystem it could make was 128 MiB and every
volume threw away the remainder. That was fine while the only volume was
a 9 GiB disk image, and became the single biggest cost of the Live CD:
the smallest live image was 129 MiB, which made the ISO 162 MB and put
7 seconds of GRUB module-reading in front of every boot.

ext2/3/4 have always allowed the LAST group to be short. The
32768-blocks-per-group figure is not a size choice, it is the number of
blocks one block of bitmap can describe; nothing about it requires the
volume to divide evenly. So the group count is a ceiling now, and
`group_span(g)` -- "how many blocks group g actually has" -- is the one
place that answers it. **`T3_BPG` still means the STRIDE between
groups**, and every block<->group calculation still uses it; only the
sites that meant "the size of this group" changed. The live image is
24 MiB and the ISO 57 MB.

The trick that kept the change small: at format time, blocks past the
end of the volume are marked USED in the last group's bitmap. The
allocator, the free-block search and the bitmap arithmetic then need no
knowledge of partial groups at all -- they simply never find those
blocks free.

**It also recovered space on the disk that was never a live image.** A
9 GiB `disk.img` now formats to 9362512 KB rather than 9232704 KB:
~127 MB that had existed and been unaddressable the whole time.

Two of the three sites that needed `group_span()` were found by failure
rather than by reading the code, which is the part worth carrying:
`df` reported a 16 MiB volume as 127 MB (free-space accounting still
assumed full groups), and the BACKUP SUPERBLOCK write used the nominal
group end, which lands past the volume -- so `fsformat` on a real disk
failed with nothing printed anywhere. That one was found by putting
`klog_printf(__LINE__)` on every `return 0` in the format path, which
took one run after three wrong guesses. A third followed from the
second: `group_span()` reads `g_sb`, and `format()` had not published
the new geometry yet, so during a format every span was computed from
the PREVIOUS volume's numbers.

**The positive control is the interesting part, because it did not
fire.** Re-introducing the `df` bug leaves the new geometry KTEST green:
on the 9 GB dev disk one group over-reported is 1.4% of the total, and
no honest bound is that tight. What catches it is `live_boot_test.py`'s
size check against the ~24 MB live volume -- the only small volume
anything here mounts. The KTEST's comment now records what it cannot
catch. Generalising: when a control fires nothing, ask whether the
test's DATA can express the bug at all before suspecting the harness.

## Filesystem is one active backend, not mount points

**SUPERSEDED on 2026-08-25 -- see "Why the VFS grew a mount table, and
why it stayed small" below.** `/boot` is a FAT32 partition holding the
kernel image the machine booted from, and no amount of
single-backend dispatch makes it readable; `kernel/fs/mount.c` holds a
prefix-keyed mount table now. What this entry got RIGHT is worth
keeping, because it is why the change was cheap: it predicted that
`struct fs_ops` would not need to change shape, only to be looked up
differently, and not one operation's signature moved. What it did not
predict is that a backend's VOLUME would have to stop being
`blk_active()`.

The original entry follows.

`kernel/fs/vfs.c` dispatches every `fs_*` call to a single active
`struct fs_ops` backend. **Updated at Milestone 15:** there are two
backends now (`tfs3_ops` and `tfs_ops`), and selection is a
boot-time superblock probe rather than a compile-time constant --
see the entry below on TFS2 staying as a second filesystem. What has
NOT changed is this entry's actual decision: exactly one backend is
ACTIVE at a time, and adding a filesystem means registering it in
vfs.c's priority list with `probe()`/`wipe()`/`format()`/`init()` +
a caps bitmask -- not routing different path prefixes to different
backends simultaneously. Mount points remain meaningfully more code
(cross-mount path resolution, boundary conflicts) for a capability
nothing needs yet. See `fs_ops.h`'s top comment and
the commit for build 304
for the original reasoning, including what it would take to add mount
points later if that ever changes.

## `/etc` and `/tmp` are created by the MOUNT, not by `kernel_main()`

`ensure_layout()` in `kernel/fs/vfs.c` makes both, and it is called
from `fs_init()` and from `fs_format_backend()`. They used to be two
`fs_mkdir()` calls on the line after `fs_init()` in `kernel_main()`,
which is correct exactly once per boot and wrong the moment anything
else mounts a filesystem -- `fsformat` reformats and remounts a live
disk and never goes near that line, so it left a volume with neither
directory until the next reboot.

The general shape is worth keeping: **if a step belongs to "having a
filesystem" rather than to "booting", it belongs beside the mount.**
What makes it cheap is that `fs_mkdir()` is a no-op on an existing
directory, so the rule can be "after every mount" with no conditions
to get wrong. See the commit that added it.

## A setting reports whether it PERSISTED, separately from whether it applied

the timezone saver, `font_config_save()`, `cursor_config_save()` and
`keyboard_config_save()` return `enum setting_result`
(`kernel/include/api/etc_config.h`): `SETTING_INVALID`,
`SETTING_SAVED`, `SETTING_UNSAVED`. Three values rather than a bool
because a caller has three different things to say -- and the four
shell commands do say them, through one shared `print_save_result()`.

This replaced three `void` returns and one that answered a different
question (the timezone saver returned 1 for a valid city whether or not
the write landed). The symptom was `timezone Helsinki` printing
`Timezone set to helsinki.` on a filesystem with no `/etc` and writing
nothing -- a lie the user only discovers after a reboot. Note the
writer was never at fault: `etc_config_set()` correctly returned 0 to
callers that did not look, which is the reusable lesson. **A function
that can fail and whose caller returns `void` is a silent failure
waiting for a reason to happen.**

`SETTING_UNSAVED` is deliberately non-zero so the existing
`if (!save(...))` idiom still reads as "did it apply?" -- adding
a distinction should not force every caller to care about it.

## A setting's identity is (namespace, name), and the namespace is its FILE

`config get theme` used to have one possible answer, because a setting's
identity was a bare global name and `setting_register()` refused a
second `theme` outright -- silently, first-wins. That was fine while the
registry was one compiled-in table of five kernel settings. It stops
being fine the moment two programs own configuration: the loser has no
way to know it lost, and the winner depends on boot order.

**The namespace is the registered NAME OF THE FILE the setting persists
to** (`api/config_file.h`), so `font_size` in `/etc/toyos.conf` is
`system.font_size`. That choice is what made this cheap: the namespace
is DERIVED, not declared, so **not one `struct setting` had to change**
and the /etc files are untouched. It also means a ring-3 program that
registers its own config file with a `/etc/config.d` descriptor gets a
namespace for free, which is the property the eventual settings daemon
needs.

Every real system namespaces settings this way -- sysctl puts it in the
path (`net.ipv4.ip_forward`), GSettings uses a schema id plus a key,
macOS `defaults` takes a domain and **requires** it for a write. A flat
global name with silent first-wins was the outlier, not the baseline.

**The lookup rule: qualified is always exact; bare works when exactly
one setting has that name, and is REFUSED when several do.** Never
resolved by order -- that would make the answer depend on boot sequence,
and the caller would never learn it had been guessed at.
`setting_matches()` returns the count, so "no such setting" and "say
which one" stay different answers; they need different words in a UI.

Two consequences worth stating:

  * **A write refuses ambiguity where a read merely reports it.**
    Reading the wrong setting shows a wrong answer; writing the wrong
    one changes something the user did not mean to change and persists
    it. `config set`, `config unset` and `config where` all resolve
    first. This is the same asymmetry that makes the domain mandatory
    for a `defaults write` and optional for nothing.
  * **`config` prints qualified names everywhere** -- `list`, `diff`,
    the ambiguity report. A tool that printed the bare key would be
    offering a name that is not necessarily usable.

The registration rule follows from the identity: a duplicate is refused
when the PAIR collides, i.e. the same name in the same file. That was
always a genuine collision. The same name in two different files is now
two settings.

**What is NOT in this**, and is a separate item on the roadmap: moving
the registry out of the kernel. Several settings are kernel state whose
`apply` mutates live subsystems (timezone, font size, keymap, cursor)
and cannot leave ring 0; the rest belong to programs, and a ring-3
settings daemon needs supervision, discovery and an IPC that toy-os does
not have yet. Qualified names were built first deliberately, because
(namespace, name) is transport-agnostic -- it is the same identity
whichever side of that boundary the registry ends up on.

## No recursive delete

`fs_delete()` refuses to delete a non-empty directory outright, rather
than deleting its contents. Deliberate, not a missing feature --
avoids a whole class of "oops, deleted more than I meant to" mistakes
in a filesystem with no trash/undo. See `kernel/include/api/fs.h` and
`tfs.c`'s top comment ("Honest limitations, not solved here").

## Persistent filesystem is write-through with a single-slot journal

Every mutating call (`touch`/`write`/`mkdir`/`delete`) still writes its
one record to disk immediately (write-through, not batched or lazily
flushed) -- but as of build 480 ("TFS2"), that one record write goes
through a write-ahead log first rather than straight to its final
table slot, closing the "crash mid-write corrupts one record" window
the original ("TFS1") write-through design explicitly accepted. A
single journal slot is enough -- not a general multi-record
transaction log -- because every mutating call here only ever changes
ONE table slot; a real filesystem juggling multi-record transactions
(renaming across directories, say) would need more than this. Chosen
over the two alternatives it was weighed against (a shadow/double-
buffer per record -- simpler logic but doubles every record's on-disk
size; a minimal commit-flag-only journal -- smaller journal but a torn
write loses the newest change instead of recovering it) because it
gives genuine crash recovery, not just torn-write detection, for a
journal region that only costs one extra record's worth of disk space
total (not per-record). See `tfs.c`'s top comment ("Journaling") for
the exact 4-step write-ahead sequence and replay logic, `docs/
tfs2-spec.md` for the on-disk journal format, and the commit for build 480 for the full writeup. **This is TFS2's rule.** The
"would need more than this" prediction came true at Milestone 15:
TFS3's mutations touch several metadata blocks, and it ships the
4-slot transaction journal this entry anticipated -- see the entry
below on TFS3's journal scope.

## File timestamps: broken-down local time on disk (TFS2), epoch seconds at the API since M15

`fs_stat()`'s `created`/`modified` fields (build 480) are `struct
rtc_time` -- the same hour/minute/second/day/month/year struct
`SYS_GETTIME` and the shell's `time` already return -- not a Unix
epoch integer. This kernel has never needed a civil-date<->epoch
conversion for anything else (no code anywhere computes "days since
1970" or similar), so storing the same struct everything else already
uses avoided adding one just for this feature; a host-side tool
reading a TFS2 image converts to epoch seconds itself if it wants
that instead (`docs/tfs2-spec.md`'s reference reader shows the
equivalent conversion via Python's `datetime`). **Updated at
Milestone 15:** the "never needed a conversion" premise expired --
`tz_rtc_to_epoch()`/`tz_epoch_to_rtc()` exist now (tz.c), the
`fs_stat()` API reports epoch seconds on every backend, and TFS3
stores epochs natively; TFS2's 7-byte on-disk civil fields are
unchanged and converted at stat time. The no-zone-recorded caveat
below still applies to both formats -- these are LOCAL-derived
epochs. The tradeoff: no
UTC-offset field is stored alongside a timestamp, so a value only
means what it looks like -- local wall-clock time at whatever
timezone was selected (`timezone` shell command) at the moment it was
written -- not an unambiguous point in time comparable across
different timezone selections. Acceptable for a toy OS's own files;
would need revisiting (probably by finally adding an epoch conversion
helper) if timestamps ever needed to be meaningfully compared against
a real-world reference. See `fs.h`'s `fs_stat()` doc comment,
`tfs.c`'s top comment, and the commit for build 480.

## TFS2 v2's block pointers go direct + single + double + triple indirect, not just direct + single

TFS2's original 2048-byte inline-file format was replaced with a
classic Unix-inode-style scheme (12 direct block pointers + single/
double/triple indirect) specifically to reach multi-gigabyte files
without keeping the whole file in RAM. Direct + single indirect alone
tops out at `12 + 1024` blocks (~4MB at the 4096-byte block size) --
nowhere close to the 8GB target. Direct + single + double gets to
roughly `12 + 1024 + 1024*1024` blocks (~4GB) -- still short. Triple
indirect (`1024^3` more blocks reachable through one pointer) is what
actually clears 8GB with headroom, which is why all three tiers exist
rather than stopping at double indirect the way a smaller target could
have. This is also why `tfs_selftest()` deliberately targets a write
at a ~4.6GB offset -- past double indirect's ceiling -- as the boot-time
proof that the triple-indirect chain is really being built and walked,
not just declared. See the commit that added it (the TFS2
multi-GB bullet) for the full format writeup.

## `fs_read_range()`/`fs_write_range()` were added alongside `fs_read()`/`fs_write()`, not as a replacement

`fs_read()`'s contract has always been "return a pointer to the whole
file, loaded into RAM in one call" -- fine for small text files, but
architecturally incapable of handling a file larger than available RAM
(256MB in the normal QEMU config) no matter how large the on-disk
format gets, since the call itself has nowhere to put an 8GB result.
Rather than redesign every existing caller (Notepad, the shell,
editor.c -- all of which only ever touch small files and are simplest
written against "give me the whole thing") around a chunked API they
don't need, `fs_read_range(path, offset, buf, len)`/`fs_write_range()`/
`fs_size()` were added as a second, parallel API for callers that
genuinely need bounded-memory access to a large file. `fs_read()`/
`fs_write()` keep their exact old behavior and signatures. See
the commit that added it (the TFS2 multi-GB bullet).

## `SYS_READ` read the whole file on every call, which made streaming quadratic

`SYS_READ`'s handler (`kernel/proc/syscall.c`) used to call `fs_read()`
-- which loads an ENTIRE file into a `kmalloc()`'d buffer -- and then
copy out just the `len` bytes sitting at the fd's current offset. Every
call. So the cost of streaming a file was (file size) x (number of
reads), and `SYS_WRITE_MAX` caps a read at 1KB.

Nothing noticed for a long time because nothing in ring 3 had ever
opened a file bigger than a few hundred bytes; at that size the whole
file *is* one read. `/bin/lspci` reading the 1.6MB `pci.ids` was the
first real caller, and it turned into roughly 1,615 calls x 1.6MB =
**~2.6GB of disk reads, taking 35 seconds** for what should be a
sub-second command. Switching the handler to `fs_read_range()` -- which
exists precisely for this, and whose own doc comment describes "a caller
streaming a whole file just calls this in a loop with an increasing
offset" -- took the same command to **0.9 seconds including boot**.

Two things worth carrying from it. **A wrong complexity class can sit
undisturbed for as long as the inputs stay small**, and it fails by
being slow rather than by being wrong, so no test catches it -- this one
was found by a feature that happened to need a bigger file, not by
review. And the correct API already existed and was already documented
for exactly this use; the bug was a call site that predated it and was
never revisited. When a range-based API gets added next to a
whole-object one (see the entry above on why both exist), the existing
callers are the thing to check.

See `syscall.c`'s `SYS_READ` branch and the git history.

## pci.ids is bundled in `data/`, not downloaded or read from the build host

`/bin/lspci` resolves `8086:7010` into "Intel Corporation 82371SB PIIX3
IDE" by reading `/usr/share/hwdata/pci.ids` -- the same file, at the
same path, that a real Linux distribution's `lspci` reads. The copy is
committed at `data/pci.ids` (1.6MB) and staged onto the disk image by
the Makefile's `seed` target.

Bundling was chosen over the two alternatives. **Reading the build
host's `/usr/share/hwdata/pci.ids`** costs nothing in the repo but makes
the build depend on host layout -- absent on macOS and minimal
containers -- and makes two machines produce different images.
**Downloading from pci-ids.ucw.cz at build time** is always current, but
puts a network fetch in the build, which breaks offline builds and the
sandboxed environments CLAUDE.md documents, and adds a supply-chain
input. A committed copy is reproducible, offline, identical everywhere,
and refreshing it is a deliberate commit rather than a silent change.

**It does not live in `seed/`.** `seed/sync/` looks like the obvious
home -- it's the tree that gets mirrored onto the disk image -- but it's
a build *staging* area: `make clean` does `rm -rf seed/sync`, and
`.gitignore` excludes it, because the Makefile repopulates it with built
ELFs every build. A file placed there works perfectly on the machine
that created it and silently doesn't exist for anyone who clones. (This
was caught exactly that way: the file survived local testing, then
vanished during a `make verify`, while the copy already written to
`disk.img` kept the feature working.) Hand-authored content belongs in a
tracked directory that the `seed` target copies in.

Licensing: upstream offers the database under GPL-2.0-or-later **or**
3-clause BSD. toy-os takes the BSD option, which is compatible with the
MIT repo; `LICENSE` carries the full text in a "Third-party data"
section, following the same pattern as the baked JetBrains Mono glyph
data (see the entry on that).

## TFS2's write batching: `write_range_impl()`'s data path in bulk, `persist_record()`'s journal down to two barriers

`ata_flush_begin()`/`ata_flush_end()` (`ata.h`) let a caller defer the
synchronous `CMD_CACHE_FLUSH` that used to follow every single ATA
write, batching it into one flush at the end of a run of many writes
instead -- `stress 100` measured ~1.4MB/s before this existed (one
flush per 4KB filesystem block written, plus a second one per
newly-allocated block's bitmap-sector update, so a 100MB write was
tens of thousands of tiny synchronous round trips). `write_range_impl()`
(`kernel/fs/tfs.c`) wraps its whole per-file-write loop in one
batch, and `persist_bitmap_bit()` defers the bitmap sector write
itself (not just its flush) to the batch's end, coalescing what would
otherwise be one redundant sector write per allocated block into one
write per distinct dirty sector.

`persist_record()` -- the write-ahead-journal-protected path that
persists a file's metadata -- can't use that same treatment, because
its four writes (journal data, commit header, real table slot, header
clear) do have ordering requirements: `ata_flush_begin()`/`end()`
suppresses ALL flushes in the region, which is exactly what a WAL can't
tolerate. For a long time it therefore flushed after every one of the
four, on the reasoning that a journal needs each write durable before
the next.

Two of those four barriers turn out to carry no weight, and the
reasoning is worth keeping because it's the general shape of the
question "does this write need a barrier?":

- **After journal data: not needed.** A torn write there fails the
  FNV-1a checksum stored in the commit header, so replay discards the
  entry. "The operation didn't happen" is a legitimate crash outcome.
- **After the commit header: REQUIRED.** Once the table slot is being
  overwritten, the journal entry is the only surviving copy of a record
  that can be torn.
- **After the table slot: REQUIRED.** Retiring the journal entry before
  the real slot is durable leaves a torn slot with nothing to replay.
- **After the header clear: not needed.** Losing it costs one redundant
  replay next boot, which rewrites the same bytes to the same slot.

So the rule isn't "a WAL flushes every write" -- it's "a barrier is
required where losing write N-1 would make write N unrecoverable." Two
of four qualify, which halved the cost of every metadata operation (a
256-record disk format went 0.73s -> 0.34s).

The two barriers use `ata_flush_now()` (ata.h), not `ata_flush_end()`,
and that distinction is load-bearing: `end()` only flushes once its own
depth reaches 0, so a journal sequence running inside an OUTER batch
would silently get no barrier at all. `tfs_check()`'s repair pass calls
`persist_record()` inside exactly such a batch -- which meant, between
the `fsck` commit and this one, the journal briefly had every one of its
flushes suppressed there.

`write_range_impl()`'s data blocks still have no ordering requirement at
all -- a half-written data block after a crash is just incomplete file
content (`fs_write_range()` documents partial-write behavior), not a
corrupted recovery structure -- so that path stays one flush per batch.
See the commit that added it for the numbers and for how
replay/discard were verified without an actual power loss.

## `fs_ops`'s new steppable-write function pointers are required, not optional/NULLable

The async-I/O roadmap item's Phase 2 (`fs_write_range_begin()`/
`fs_write_range_step()`, see `docs/roadmap.md`) added two new function
pointers to `struct fs_ops` (`kernel/include/kernel/fs_ops.h`) rather than a
separate, optional side-interface a backend could leave unset. Every
other entry in that struct is unconditionally required -- `vfs.c`'s
dispatch wrappers call straight through (`g_fs->touch(...)`, etc.) with
no NULL check on the function pointer itself, only on the *arguments*
(see `fs_write_range_step()`'s handle guard, added the same session).
Making the two new ones optional would have meant either a NULL check
on every dispatch call (real overhead on the hot path for something
every backend implements) or a silent fallback to
non-stepped behavior a caller couldn't easily detect it got.
**Updated at Milestone 15:** `fs_ops` DOES carry a capability
bitmask and one optional op now (`link()`, gated by
FS_CAP_HARDLINKS, with the honesty check refusing a backend whose
bit and pointer disagree) -- but the required-not-optional call made
here still stands for the range/steppable ops: both backends (tfs2
and tfs3) implement them, and there's still no scenario where a
backend legitimately can't. If a future backend
genuinely can't support incremental writes (say, one backed by a
remote API with no partial-write primitive), that's the point to
revisit this, not before.

## `tools/tfs2_writer.py`: content-hash sync, not mtime comparison; direct+single-indirect write scope, not full indirect support

Two scope calls made building the deferred host-side TFS2 writer (see
the entry above): how `sync`'s "only rewrite if changed" policy
detects a change, and how big a file the tool is willing to write at
all. Both calls carried unchanged into `tools/tfs3_writer.py` when
TFS3 landed -- same content-hash sync, same ~4.03 MB
direct+single-indirect cap.

**Content hash, not mtime.** TFS2's `created`/`modified` fields are
toy-os's own RTC-sourced local wall-clock time (see `fs.h`'s
`fs_stat()` comment) -- there's no epoch, and no defined relationship
to the *host* machine's clock a comparison could lean on without
assuming a particular skew. Comparing "is the local file newer" against
that would be guessing. Hashing the on-disk content and comparing it to
the local file's content sidesteps the clock question entirely and is
just as correct for the actual goal ("did this file's bytes change") --
this is why `sync`'s `sync/` subtree policy reads the existing file
back and SHA-256-compares it rather than checking timestamps.

**Write scope is direct + single-indirect blocks only (~4.03 MB/file),
not the full direct+single+double+triple scheme `docs/tfs2-spec.md`
documents for reading.** The tool refuses cleanly (clear error, no
silent truncation) rather than write a partial file past that size.
Everything this tool exists for -- ELF binaries, config/text seed
files -- fits comfortably under that ceiling; double/triple-indirect
allocation is real extra code (the same recursive block-tree shape
`tfs.c`'s own `alloc_block()`-adjacent logic would need) that has no
current caller. If a future seed file genuinely needs to be larger,
extend `write_file()`'s allocation loop rather than raising the limit
silently -- the read path (`block_for_index()`) already walks all four
levels, so only the write side needs the extra work.

## `/bin` binaries: boot-time bootstrap-install now, a host-side TFS2 writer tool later

Getting a compiled ELF's bytes onto `disk.img` has no in-guest-compiler
option -- something outside the OS has to place them there. Two ways
were on the table (see the previous entry's roadmap plan): a host-side
tool that writes directly into TFS2's on-disk format (informed by
`docs/tfs2-spec.md`'s existing read-only reference parser, which would
need a write-side counterpart built from scratch), or copying a GRUB
module's bytes into `/bin` once, at boot, via code the kernel already
has (`fs_write_range()`/`fs_touch()`, both already exercised by other
callers). The user chose bootstrap-install for `lspci` now, with the
host-side tool explicitly deferred to a later session (see
`docs/roadmap.md`'s new backlog entry) rather than skipped -- the
bootstrap path needs zero new tooling and was demonstrably enough to
prove the whole `/bin`-loading pipeline end to end, while the
host-side tool only pays for itself once a *second* binary needs
installing without a kernel rebuild, which isn't true yet. The
tradeoff this defers, worth remembering when that second binary shows
up: `install_bin_binaries()` (`kernel/core/kernel.c`) is a small table
of `{module_index, bin_path}` pairs specifically so adding one more
GRUB-module-installed binary is a one-line addition, not a redesign --
but it's still "add a GRUB module + a table row + rebuild the kernel"
per binary, not "drop a file onto the disk image," which is exactly
what the host-side tool is for.

## `/bin/lspci` moved from boot-time bootstrap-install to build-time seeding, once the writer tool existed

The previous entry deferred the host-side TFS2 writer tool and kept
`install_bin_binaries()`'s boot-time bootstrap-install
(`kernel/core/kernel.c`, copying a GRUB module's bytes into `/bin` the
first time toy-os boots against a disk) as the interim way to get
`lspci` onto disk. Once the writer tool existed and grew a `format`
subcommand (see the entry above -- needed because `write`/`sync`
previously required an already-formatted image, which a brand new
`disk.img` isn't until toy-os boots and formats it once), that
interim mechanism became fully redundant: the writer tools'
`sync`
can format-and-seed a completely untouched `disk.img` in one call, at
BUILD time, with no boot cycle needed at all. (Since Milestone 15 the
Makefile's entry point is `tools/seed_disk.py`, which probes the
image's magic and delegates to `tfs2_writer.py` or `tfs3_writer.py`;
a blank image gets the default format, TFS3.)

`install_bin_binaries()`/`BIN_BOOTSTRAP` and the `lspci.elf` GRUB
module were removed outright rather than kept as a fallback -- two
mechanisms solving the same problem is exactly the kind of debt this
project avoids once the better one exists (see `CLAUDE.md`'s file-split
guidance for the same instinct applied elsewhere: don't keep unused
machinery "just in case"). If a future binary genuinely needs
boot-time-only install for some reason GRUB-module bootstrap-install
would fit better than build-time seeding, that's a fresh design
question when it actually comes up, not a reason to have kept the old
table around empty -- the git history (this entry, and the
the commits it points at) has everything needed to bring
the pattern back if so.

The Makefile's new `seed` target runs on every `make iso` (not just
when `disk.img` is first created) -- deliberately `.PHONY` so it always
re-runs, relying on `sync`'s own content-hash compare (not `make`'s
mtime-based staleness check) to make repeat calls cheap. This matters
because `disk.img` is explicitly NOT rebuilt by `make clean` (see its
own comment in the Makefile -- it's local persistent dev state, not a
build output) -- if `seed` only ran once, a rebuilt `lspci.elf` with
real code changes would silently never reach an existing `disk.img`
again.

## Real disk-hosted ELF binaries: an old plan re-verified before building, not built from the doc as written

`docs/roadmap.md` already had a plan for this (split into (A) a real
syscall-based ELF program, (B) loading it from `/bin`), written in an
earlier session as a pure planning pass. Before actually building it,
that plan got re-checked against the current codebase rather than
implemented as written -- worth recording why, since the two
differences found are exactly the kind of "the codebase moved out from
under an old doc" trap a future session could hit again elsewhere:

- The plan's stated hard blocker for (B) was TFS2 capping a file at
  `FS_DATA_MAX` = 2048 bytes, with a whole discussion of multi-slot
  chaining to fix it. By the time this was re-checked, `tfs.c` no
  longer referenced `FS_DATA_MAX` at all -- TFS2 v2's block-addressed
  on-disk rework (the multi-GB file support entry, the git history) had already solved this as a side effect, for
  unrelated reasons, in a different session that had no idea an old
  ELF-binaries plan was depending on that limit staying in place. Three
  comments (`kernel/lib/etc_config.c`, and the since-deleted
  `apps/editor.c`/`.h`) still cited the old 2048-byte ceiling as real
  months later -- corrected in the same change that shipped this (see
  the git history).
- The plan assumed `elf_load()`'s ELF blob would need copying out of
  TFS2's live in-RAM table into a scratch buffer before executing,
  since that memory "isn't stable the way a GRUB module's reserved
  region is." Checking `elf_load()` (`kernel/proc/elf.c`) and
  `heap_core.c`'s own top comment together showed this wasn't needed:
  `elf_load()` just casts its `elf_phys_addr` argument straight to a
  pointer with zero translation, which only works because GRUB modules
  sit in identity-mapped low physical memory -- and `kmalloc()` is
  *also* carved out of that same identity-mapped low-4GiB range (see
  `paging.c`'s top comment, referenced from `heap_core.c`), so `fs_read()`'s
  returned buffer address already works there directly. One real
  constraint this does leave, not present in the GRUB-module path:
  nothing may call `fs_read()` again until the loaded process finishes,
  since the backend reuses one static buffer across calls (`fs.h`'s
  `fs_read()` doc comment already says this; `elf_run.c`'s own comment
  restates it as a caller-facing constraint).

Net effect: (A) and (B) shipped together in one change instead of two,
since (B) turned out to be much smaller than the plan estimated. What
the plan got right and is still true: getting a binary's bytes onto
`disk.img` at all needs *something* outside the OS, since there's no
in-guest compiler -- see the next entry for which of the plan's two
options (`bootstrap-install` vs. a host-side writer tool) was picked,
and why. See the commit that added it for the full
implementation (`SYS_PCI_COUNT`/`SYS_PCI_INFO`, `userland/bin/lspci.c`,
`elf_run.c`, `install_bin_binaries()`).

## Every ELF64 test binary moved to `/bin`, not just `lspci` -- and why two didn't fold in cleanly

`lspci` was the first ELF64 binary moved off a GRUB module onto
build-time-seeded `/bin` (see this file's `seed`-target entry above).
The remaining dozen-ish test binaries followed the same path in one
pass (the commit that added it has the full list) rather
than staying GRUB modules indefinitely, once it was clear the seeding
mechanism generalized cleanly -- there was no longer a reason for
`lspci` to be the only one.

Almost all of them folded into the existing generic loader
(`kernel/proc/elf_run.c`'s `elf_run_from_fs()`) with zero new code,
which is the whole point of that function existing: one loader, N
binaries, no per-binary kernel harness. Two didn't:

- **`schedtest`** (`counter_a`/`counter_b`) needs two processes running
  *concurrently* under the real preemptive scheduler -- a one-shot
  `run <name>` inherently can't do that, no matter how generic the
  loader gets. This got real new code: `scheduler.c`'s
  `spawn_from_fs(const char *path)`, replacing `spawn_from_module()`
  outright (its only caller was `scheduler_demo_run()`) -- same
  no-copy-needed `fs_read()` reasoning `elf_run_from_fs()` already
  used, just wired into the scheduler's spawn path instead of the
  one-shot run path.
- **`elftest`/`hello.elf`** tested toy-os's raw manual-`iretq` ring-3
  entry specifically, a different (and older) code path than
  `process_run_ring3()`'s recoverable one. Folding it into `run hello`
  means that specific raw-entry test coverage is gone -- a real
  tradeoff, made deliberately (user's call, weighing one narrow bit of
  coverage against one less special case) rather than accidentally.
  `ring3test` remains as the one place the raw-`iretq` path is still
  exercised at all (see this file's entry above). What this migration
  did *not* notice: `elftest` had also been mapping a page at
  `USERLAND_MARKER_ADDR` for `hello.elf` to write to, and the generic
  path doesn't -- see [hello.c stopped faulting on purpose and started
  faulting by
  accident](#helloc-stopped-faulting-on-purpose-and-started-faulting-by-accident).

`ring3test` itself was never a candidate to fold in -- it uses no ELF
file whatsoever, there's nothing to seed.

Along the way, `elf_run_from_fs()` gained an unconditional
`syscall_reset_heap()` call it didn't have before -- found by reading
`echo_test.c`, which called this itself ahead of its old
dedicated-command loader. Migrating `echo_test` onto the generic path
without this would have silently broken its `sbrk()`-based heap the
first time anyone actually exercised it, not at compile time. Making
it unconditional (rather than a per-binary opt-in flag) costs nothing
for a binary that never calls `sbrk()` -- it's bookkeeping, not an
allocation -- so there was no reason to keep it special-cased.

`apps/terminal.c`'s GUI Terminal window still blocks `run` wholesale,
not per-target. Several of the newly-independent `/bin` binaries
(`gui_test`, `echo_test`) have the same hazards inside a
GUI window the old dedicated commands were blocked for (drawing
straight to the physical framebuffer, blocking forever without
yielding back to the window manager) -- a per-target allowlist was
prototyped, but the QMP test written to verify it was invalid: it
relied on `tools/gui_flow.py`'s `open_app("Terminal")`, which (due to
a separate, pre-existing bug -- see below) was actually opening
Calculator. Rather than ship GUI-safety-relevant logic that couldn't
be verified, the simpler wholesale block was kept. Every `/bin` binary
can still be run from the physical shell regardless.

That `gui_flow.py` bug was real and unrelated to this migration:
`ITEM_H` (the assumed Start-menu row height) was `32`, stale against
the kernel's actual `gfx_char_h() + 6` (`24` at the default font
size) -- so every `open_app()` call was clicking roughly one row below
where it meant to. Found and fixed once it was blocking this
migration's own testing; see the commit that added it.

## GPT header verification: a host-compiled unit test, not a live boot -- TFS2's own journal collides with LBA 1

`kernel/drivers/partition.c`'s GPT support (Milestone 3, the commit that added it) couldn't be verified the same way its MBR half was
(a real `disk.img` patched with synthetic data, booted, `parttable` run
from the shell over QMP) -- a real, unavoidable architectural conflict,
not a testing inconvenience:

- The GPT header's LBA is fixed by spec at LBA 1.
- `kernel/fs/tfs.c`'s `FS_JOURNAL_HEADER_LBA` is *also* LBA 1
  (`FS_SUPERBLOCK_LBA + 1`).
- `tfs_init()` calls `tfs_selftest()` **unconditionally** after either
  mounting or formatting (`if (g_disk_backed) tfs_selftest();`, no
  bypass/flag), and `tfs_selftest()` creates and writes a real file --
  which, via `persist_record()`, always ends with `write_journal_header(0,
  0, 0)`, overwriting LBA 1 with a real `"JRN1"` journal header.
- This runs synchronously during `kernel_main()`, before the shell
  prompt is ever reachable -- there is no window, pre-boot or live-patch
  mid-boot, where a custom GPT header at LBA 1 survives long enough for
  a shell command to read it. Confirmed two ways during development: a
  disk patched with a valid GPT header before boot came back showing a
  fresh `"JRN1"` journal header at LBA 1 after boot (self-test's write
  landed exactly where the GPT header had been); and patching the file
  live from the host while QEMU sat idle at the shell prompt was
  *also* unreliable -- QEMU's own write-back caching raced the external
  patch and won, restoring the stale in-memory `"JRN1"` copy moments
  later; a plain host-side read immediately confirmed the external
  write was never actually left standing.
- (The MBR half doesn't have this problem: its partition-table region
  is bytes 446-511 of LBA 0, which TFS2 never touches -- `write_superblock()`
  only ever writes bytes 0-4. `tools/mkpart_test.py --mbr` reads the
  existing LBA 0 sector and only patches that region, preserving TFS2's
  magic so `tfs_init()` mounts normally instead of reformatting.)

Verified instead with a host-compiled unit test
(`/tmp/.../parttest/harness.c` during development, not committed --
see below) that `#include`s the real, unmodified
`kernel/drivers/partition.c`, with a tiny stub `ata_read_sector()`
reading from a plain file instead of real hardware. Run against a
synthetic image `tools/mkpart_test.py --gpt` wrote (a scratch file, not
`disk.img`), it correctly validated the header's CRC32, decoded both
partitions' type/unique GUIDs, LBA ranges, and UTF-16LE names exactly
matching what was written. This is real execution-level proof of the
parsing algorithm (CRC32, field offsets, GUID mixed-endian decoding) --
compiled from the actual shipped source, not a second reimplementation
-- just not exercised through `ata.c`'s real hardware I/O path the way
the MBR case was. `tools/mkpart_test.py` itself is committed (useful
for any future partition-table work); the throwaway `harness.c`/stub
`ata.h` were scratch-only and not worth keeping as-is -- recreate the
same shape (stub `ata_read_sector()`, `#include` the real `.c` file
being tested) if this pattern is ever needed again for another
on-disk-format parser.

## An unreadable superblock is not a foreign disk -- refuse to format, don't guess

`tfs_init()` distinguishes "the superblock read failed" from "the
superblock read fine and isn't ours", and only the second one formats.
The first degrades to RAM-only for that boot and leaves the disk
untouched. This looks like defensive over-engineering until you notice
the failure it replaced was silent total data loss: the two cases used
to share one `if`, and `ata_read_sector()` genuinely does give up after
three exhausted DMA attempts, which this project has observed happening
on real hardware for transient reasons (host filesystem stalls, not a
sick drive -- see `ata.c`'s `dma_transfer_with_retry()` comment).

The asymmetry is the point: formatting a disk that was actually fine is
unrecoverable, while refusing to format a disk that really is blank
costs one boot and a clear `dmesg` line telling you to check it. When
the two error paths have wildly different costs, the cheap-to-recover
one is the correct default. A blank/foreign disk still auto-formats,
because that path is only reached on a *successful* read.

See the commit that added it for the full writeup,
including the related "disk too small to hold the metadata region" case
and the one case still not detectable (a 0-length image, which QEMU
answers with zeros rather than an error).

## Metadata ordering: persist the record first, free the blocks second -- prefer a leak to a double-allocation

`tfs_delete()` and `tfs_write()`'s truncate path both detach a file's
block pointers, write the record that now references nothing, and only
then return the blocks to the free bitmap (`detach_blocks()`/
`reattach_blocks()` in `tfs.c`). The reverse order is the obvious one
and was what the code did first, but it opens a window where the bitmap
says a block is free while an on-disk record still points at it -- the
next allocation hands that block to a different file and two files
silently share it.

Inverting the order makes the worst case the *opposite* failure: blocks
marked allocated that nothing references. That's a space leak, it's
detectable by walking every record's pointers, and it costs disk space
rather than data. There's no fsck-style pass to reclaim them yet (see
`docs/roadmap.md`'s Milestone 3) -- the ordering is chosen so that when
something does go wrong, the recoverable failure is the one that
happens.

## `fsck` reclaims leaks and marks stragglers, but never resolves a double-allocation

`fs_check()`/`tfs_check()` (`tfs.c`) repairs exactly three things and
deliberately refuses a fourth:

| Finding | Repaired? | Why |
|---|---|---|
| Leaked block (allocated, unreferenced) | yes -- freed | Costs only space; the free bitmap is provably wrong and the record tree is the authority. |
| Referenced but marked free | yes -- marked allocated | The dangerous direction: leaving it lets the allocator hand the block to a second file. |
| Out-of-range pointer | yes -- zeroed | It can't name real data; zeroing turns it into a hole that reads as zeros. |
| Block claimed by two records | **no** -- reported only | Both records are internally plausible. Choosing which keeps the block silently destroys the other file's data, and no amount of on-disk information says which one is right. |

That last row is the whole design stance: a repair tool that guesses
turns a recoverable disk into a confidently-wrong one. It reports the
count and tells you to delete one of the affected files.

The scratch "referenced" bitmap is a static 288KB array (`g_fsck_seen`),
not `kmalloc()`'d, because a 288KB allocation needs 72 contiguous frames
from pmm and failing to get them would mean "can't check the disk"
precisely when something is already wrong. Same reasoning `g_bitmap`
itself uses one bullet up, with a repair-tool-specific edge.

This exists because the truncate/delete ordering deliberately prefers a
leak to a double-allocation (see the entry above) -- that trade is only
correct if something can reclaim the leak afterwards.

Testing it needed fault injection: the inconsistencies it repairs are
ones the kernel goes out of its way not to produce, so
`tools/tfs2_writer.py corrupt` manufactures them host-side
(`--leak N`, `--free-referenced N`, `--bad-pointer PATH`). Doing that
turned up a live demonstration of why the referenced-but-free repair
matters: with three referenced blocks marked free, the very next boot's
shell-history append allocated one of them to `/etc/history`, which
already belonged to `/bin/counter_a` -- a real double-allocation,
created by the corruption in seconds. See the commit that added it.

## Thin provisioning: the image is sparse at birth, and TRIM is what keeps it that way

`disk.img` is created with `truncate -s 9G`, so it costs nothing up
front. But sparseness is only ever LOST: a block written once stays
allocated on the host forever, even after toy-os deletes the file that
owned it. The bitmap bit clears, the host is never told, and the file
only grows.

Measured on the development image before any of this existed: **8.1 GiB
actually allocated against 581 blocks (2.3 MiB) that TFS2 considered in
use** -- 99.97% of it the leftovers of past `stress` runs. `fsck`
reported the filesystem completely clean, because it was: nothing had
leaked *inside* the filesystem, the space simply never went back to the
host. That is the whole problem in one sentence, and it is why "the
image is sparse" was true and useless at the same time.

Both halves of the fix exist, deliberately:

- **`tools/tfs2_writer.py trim`** reads the allocation bitmap and
  punches holes (`FALLOC_FL_PUNCH_HOLE`) through every run of free
  blocks. It reclaims images that are already in that state, and covers
  the host-side seeding path, which never goes through the kernel at
  all. Non-destructive: only blocks the filesystem already considers
  free are touched.
- **`ata_trim()`**, issued from `free_block()` (`kernel/fs/tfs.c`) as
  blocks are freed, with `discard=unmap` on every QEMU `-drive` line.
  QEMU turns the guest's TRIM into a hole punch, so an `rm` inside
  toy-os gives the space back with no host tool involved.

The result is measurable: `stress 150` writes 150 MB, verifies it,
deletes it, and the image is unchanged at 2.3 MiB. Before, that run cost
150 MB of host disk permanently.

The kernel deliberately ignores `ata_trim()`'s result. TRIM is an
optimisation -- the block is free either way, and a drive that refuses
it (or doesn't support it, which `ata_trim_supported()` answers from
IDENTIFY word 169) must not turn a successful delete into a failed one.

## TFS2 stays in the kernel as a second filesystem -- the VFS probes by superblock magic

Milestone 15 (TFS3) did not replace TFS2: both backends are compiled
in, `vfs.c`'s `fs_init()` walks them in priority order (tfs3 first)
asking each one's side-effect-free `probe()`, and the first valid
superblock wins -- so an existing TFS2 disk keeps mounting untouched
while fresh/blank disks get the default (TFS3). Kept deliberately, at
the user's request, to make filesystem switching a testable, living
path: `fsformat <tfs2|tfs3> confirm` reformats and remounts live
(wiping the OTHER format's signatures first -- the wipefs rule, see
`fs_ops.h`'s `wipe()` contract for the mounted-a-corpse story), and
`tools/fs_switch_test.py` proves the whole cycle including reboot
persistence. Capabilities differences are declared, not discovered:
`fs_ops.caps` mirrors `display_driver`'s honesty rule (bit and
optional op are one fact stated twice, refused when they disagree),
`fs_stat()` is one canonical shape (epoch times + an ino that TFS2
synthesizes from its table slot, Linux's FAT trick), and the ring-3
ABI never changed (epochs convert back to `rtc_time` at the syscall
boundary). see the Stage A/B commits.

## TFS3's journal covers dirent + inode blocks; bitmaps stay leak-safe write-through

The design doc sketched journaling "dirent + inode + bitmaps"; the
shipped journal (Stage C) deliberately narrowed to dirent blocks and
inode-table blocks only -- the structures whose torn write is
namespace corruption. Allocation bitmaps and group descriptors are
write-through and unjournaled under set-before-use /
clear-after-persist ordering, so a crash costs a leaked block that
`fsck` reclaims and never a double allocation -- the exact rule TFS2
established ("prefer a leak to a double-allocation") applied to the
new format. Every operation fits <= 3 of the journal's 4 slots, and
directory growth runs as its own empty-block-first transaction
(inserting the child's name into the grow block would have made the
name visible one transaction before the child's inode existed). See
`docs/tfs3-spec.md`'s journal section and the Stage C commits
entry.

## TFS3 v2 grew the journal by moving the layout, not by making it a log like ext4's

Four slots turned out to be a design constraint on OPERATIONS, not a
tuning number: a rename that moves a directory between parents touches
five metadata blocks (both dirent blocks, the child's `..`, both
parents' link counts), so it could not be expressed at all. The
journal sits between the superblock and the group descriptors, and
everything before group 0 was spoken for, so making room meant moving
`group0_start` -- i.e. a format version.

**Why not ext4's journal.** jbd2 makes the journal a regular inode
(inode 8, ~128 MB by default) holding a circular log: a descriptor
block naming each following image's real target, the images, then a
commit block. Three separable ideas live in that, and only one was
worth taking now:

- **Credits, taken.** `jbd2_journal_start(journal, nblocks)` reserves
  the worst case up front and refuses an operation that cannot fit
  before it has changed anything. TFS3 used to discover "full" halfway
  through, when `txn_stage()` returned 0 and each caller unwound by
  hand. `txn_begin(credits)` is that discipline in miniature, and it
  is what lets a v1 image behave CORRECTLY rather than half-completing:
  the one operation it cannot hold is refused with a message, and
  everything else is unaffected.
- **A large circular log, deferred.** Its real payoff is batching many
  operations into one commit, which would cut the two `ata_flush_now()`
  barriers TFS3 pays per metadata operation. That is a throughput
  project with its own crash-recovery surface (sequence numbers, log
  wrap, checkpointing), not a side effect of needing five slots.
- **Revoke blocks, not needed.** They exist because a freed metadata
  block can be reused as file data, where replay would clobber it.
  TFS3 journals only dirent and inode-table blocks, and frees blocks
  unjournaled under the leak-safe rule, so the hazard never arises.

**Both versions stay mountable, and that is not politeness.** A probe
that returned "not mine" for a v1 image would hand it to the
blank-disk policy, which formats -- so refusing to READ an old format
is a way of destroying it. v1 mounts read/write with its own geometry;
only `format` (and `fsformat tfs3 confirm`) writes v2. Each version's
geometry is a set of CONSTANTS rather than superblock parameters,
which preserves the property the fixed-size descriptor table exists
for: a reader whose primary superblock is unreadable has two candidate
values of `group0_start` to try, not an unknown one. The superblock
does carry the offsets, but a mount validates them against the
version's constants and rejects a disagreement.

The reformat also has to erase the OTHER version's backup superblock
sectors -- the wipefs rule one format version apart instead of one
filesystem apart, and the same seance it was written for. See
`docs/tfs3-spec.md`'s layout section.

## `fs_rename()` refuses an existing destination -- there is no atomic replace

POSIX `rename(2)` silently replaces the destination. `fs_rename()`
returns 0 instead, and the shell's `mv` says "remove it first".

Two reasons. The API's whole style is "a parser rejects rather than
guesses" applied to destructive operations -- and this is the one
mistake `mv` can make that a user cannot undo, because the replaced
file's blocks are gone. And the atomic version is a bigger operation
than it looks: it has to free the old target's inode inside the same
transaction, which adds a slot and a rollback path for something no
caller has asked for. Adding it later is additive; having shipped a
silent overwrite and then restricting it would not be.

Two other refusals are not policy but necessity: a directory moved
into its own subtree would detach that subtree into a cycle nothing
references, and the root has no parent to be renamed in. Renaming
something to its own path succeeds and changes nothing.

## Truncation is two phases with a commit between them, and keeps the boundary tables in memory

Shrinking obeys the same ordering as every other metadata change here
-- the inode that stops referencing a block must be durable BEFORE the
block's bit is freed, or a crash in between leaves a live file pointing
at space the allocator can hand to a second file. The obvious
implementation (free the tail, then write the inode) inverts exactly
that, and it is the double-allocation the whole discipline exists to
prevent.

That forces a commit into the middle of the operation, which creates a
second problem: phase one rewrites the pointer tables, so phase two can
no longer read from disk what it is supposed to free. The way out is
the shape of the cut. A truncation is a clean split -- at every level
each entry is wholly kept or wholly dropped -- EXCEPT for at most one
straddling entry per level. So there are at most three partially
rewritten tables, and keeping their original images in memory (12 KiB)
is enough for phase two to walk everything phase one detached; every
other table it reads is one phase one deliberately did not touch.

Growing needs none of this: both backends read an unallocated range as
zeros, so a grow moves the size field and nothing else. `truncate f
1000000000` is one inode write and no blocks.

Both backends implement this the same way and separately
(`trunc_begin`/`trunc_free` in tfs3_write.c, `trunc_detach_tail`/
`trunc_free_tail` in tfs.c), consistent with their already-parallel
block-map walks -- they persist through completely different mechanisms
(a journal transaction vs. a record write), which is most of what the
code around the walk is.

## ATA DATA SET MANAGEMENT must be issued over DMA, not PIO

DSM (the TRIM command) reads like an ordinary PIO data-out command in
the spec: set the TRIM bit in Features, put the descriptor-block count
in Sector Count, write 512 bytes of LBA ranges. The first implementation
here did exactly that, and it **silently did nothing** -- the drive
accepted the command, raised no error, returned success, and not one
byte was discarded.

QEMU dispatches DSM through `ide_sector_start_dma()` with
`IDE_DMA_TRIM` (`hw/ide/core.c`), so the range list has to arrive by
bus-master transfer. Over PIO it never arrives; the "success" is the
drive acknowledging a command whose payload it is still waiting for.

`ata nodma` does NOT stop it, which is worth knowing because it reads
like it should: that switch forces DATA transfers down the PIO path,
and TRIM keeps going out over the bus master regardless, because there
is nowhere else for it to go. Measured rather than assumed -- with PIO
forced, `stress 30` still leaves the image at its pre-run size. What
DOES disable TRIM is Bus-Master DMA never coming up at all, and
`ata_trim_supported()` accounts for that so the `ata` command can't
report "supported" on a machine where every TRIM would fail silently.

Worse, the half-issued command leaves the channel desynced, and the
next few ATA commands return garbage. That is what produced a burst of
`ata: refusing transfer past end of drive` complaints with absurd LBAs
and a `stress` run that leaked all 10,237 of its blocks -- neither of
which was a filesystem bug at all. Worth knowing before trusting any
DSM return value, and a good reminder that "the command succeeded" and
"the command did something" are different claims.

## Zero-filling a freshly allocated block is skipped only when the caller overwrites it whole

`block_for_index()`'s allocation modes (`BLK_ALLOC` vs.
`BLK_ALLOC_NOZERO`, `tfs.c`) exist because zero-filling every newly
allocated block costs a full block write, and for a sequential write
that immediately overwrites the whole block that write is pure waste --
it doubled the ATA commands per block and split the multi-block
coalescing apart, which measurement caught (the first coalescing pass
only reached 20.3 MB/s of the eventual 25.1).

Where zero-filling still always happens, and why:
- **Indirect index blocks, unconditionally.** Their unwritten entries
  are read back as block pointers, so they must be the 0 sentinel and
  not whatever a deleted file left there -- `walk_indirect()` ignores
  the NOZERO flag for them on purpose.
- **Any partially written block.** Otherwise the read-modify-write in
  `write_range_one_block()` would leak a deleted file's contents into
  the untouched part of the block.
- **Sparse gaps**, implicitly: a hole has no block at all and reads as
  zero via `read_block(0)`.

The one visible consequence: if the write that was supposed to overwrite
a NOZERO block fails, the block keeps stale content. It's past the
file's size (which only advances for bytes actually written), so no read
can reach it. See the commit that added it.


---

## `SYS_LISTDIR` disambiguates empty from missing in the SYSCALL, not in `fs_list()`

`fs_list()` returns void, and "does nothing for a path that is not a
listable directory" is its documented behaviour (`api/fs.h`). So a
missing directory and an empty one both left `SYS_LISTDIR`'s count at
zero, and `ls /nope` printed an empty listing and exited 0 -- the one
conclusion a caller must not be allowed to draw.

The obvious fix is to give `fs_list()` a return value, and it is the
wrong one: every backend would have to grow one, and the VFS is not
where this distinction matters. Ring 3 is the only caller that cannot
look for itself; the kernel can call `fs_exists()`/`fs_is_dir()` any
time it likes. So the syscall handler probes and returns `-ENOENT` or
`-ENOTDIR`, which is the same shape `opendir()` has and the same place
POSIX puts it.

**Only on the zero path**, so an ordinary listing pays nothing: the two
probes are directory walks, and a non-empty result has already proved
the directory exists by producing its children. That is also why this
did not need `fs_stat()` -- it is meaningless for the implicit root "/",
which `fs_is_dir()` handles.

Nothing in ring 3 broke, because `/bin/ls`, `/bin/tosh`, `init`, Notepad
and the file picker all already tested for a negative return; they
simply had no negative to see. `ls` was already printing
`strerror(sys_errno())` for the `-EFAULT` case, so the reason reaches a
person with no change at the call site -- which is what the errno
convention was for.

## A tunable says "do not persist me" with its FILE, not with a flag

A tunable is a setting whose `apply` writes a live kernel variable, and
the vocabulary table has always said its "survives a reboot" is
*optionally*. Building that half raised the question of how a setting
expresses it. The answer: `struct setting` gained NOTHING. A tunable
declares `CONFIG_PATH_RUNTIME` as its `file`, and `setting_persists()`
is a predicate over that.

**Why not a flag.** A `SETTING_F_RUNTIME` bit would be a second thing to
keep in step with the file, and the failure mode is silent in the worst
direction — a setting with a real path and the flag set would apply and
never write, looking correct until a reboot. The file already had to say
where the value goes; letting it also say *nowhere* keeps one source of
truth. It also makes the opposite case free: a tunable that SHOULD
survive a reboot names a real `/etc` file and needs no other change,
which is exactly sysctl's split — runtime by default, persistence opted
into through `sysctl.conf`.

**Why a sentinel path rather than an empty one.** A setting's identity
is `(namespace, name)`, and the namespace is derived by looking its file
up in the config-file registry. A fileless tunable would have namespace
`""` and be addressable only as a bare name — which the registry refuses
when ambiguous, so `heap_debug` would work only until something else
claimed that name. The sentinel registers as an ordinary config file
named `kernel`, so the existing by-path lookup resolves it unchanged and
these read as `kernel.heap_debug`. That is the word sysctl uses for the
same kind of knob (`kernel.printk`), which is the point of choosing it.

Registration refuses a tunable with no `apply`. The persisted-only
flavour (`apply == NULL`) works because the registry writes the file
itself; with no file AND no apply a value is neither held nor stored, so
`set` would report success having changed nothing observable. That is a
class of bug worth a build-time refusal rather than a runtime surprise.

**They appear in System Settings, under a `Kernel` heading.** The
alternative — hiding anything with no config file — is what sysctl vs
GSettings does, and was rejected because this desktop's Settings app is
GENERATED from the registry and hiding part of it would make the app
quietly incomplete. A visible, separate heading keeps the honesty
without putting a heap-debug toggle beside the wallpaper.
`tools/settings_test.py` asserts both the heading and its three group
pages, and the positive control (filing them under `Appearance`)
reddens the heading check alone.

## `write()` to a file took a C string, and said it had written the rest

`sys_do_write_file()` copied the user's buffer into a NUL-terminated
scratch buffer and called `fs_write(name, tmp, 1)`. `fs_write()` takes a
**C string**, not a length -- so a write containing a zero byte stored
only the prefix. And then it did the damaging part: it set the return
value to `len` regardless, so every caller was told all of it had
landed.

**Why it survived so long.** Nothing in this OS wrote binary through a
file descriptor. Notepad saves text, `tosh`'s redirection carries text,
`config` writes `key=value` lines -- none of which contain a zero byte,
so all of them worked perfectly. `/bin/mkfiles` was the first program to
write a derived byte pattern, and it broke on file 0.

The arithmetic is worth recording because it is what turned a suspicion
into a diagnosis. Asked for 1,207 bytes in two chunks, file 1 came back
**82 bytes**. Its pattern is `31 ^ (offset * 7)`, which is zero when
`offset * 7 ≡ 31 (mod 256)`, i.e. at offset 41 -- and the second chunk
starts at 1,024, which is `0 (mod 256)`, so it hits zero at its own
offset 41 too. 41 + 41 = 82, exactly. A prediction that lands on the
observed number is a different kind of evidence from a story that fits
it.

The fix is `fs_write_range()`, which takes an explicit length and treats
the buffer as raw bytes -- `fs.h` describes it as "the call a large
binary file actually wants". Append semantics are unchanged (the current
size is passed as the offset, which `fs.h` documents as the way to say
append), because making writes honour the fd's `offset` field is a
separate change: that field is maintained by the READ path only, and
every existing caller relies on writes appending.

**The lesson is about the RETURN VALUE, not the string.** A truncating
write that reported the truncation would have been found the first time
anything wrote a zero byte; one that reports success cannot be found by
checking return values at all, which is what every caller was doing.
`userland/tests/file_test.c` now round-trips a buffer with zero bytes in
the middle and asserts on the SIZE READ BACK -- deliberately not on the
write's return value, since believing that is precisely the mistake.

## The calendar arithmetic left `tz.c`, and `time()` is deliberately not UTC

Two decisions from the C library's `<time.h>` (`docs/libc-design.md`
Stage 5), both about the same thing: not letting a library invent
semantics the system does not have.

**The Gregorian arithmetic is `kernel/lib/caltime.c` now, shared into
ring 3.** It used to sit inside `tz.c`, which also owns the
`/etc/timezones` database, the persisted city choice and the two DST
rules -- so that file includes `fs.h`, `klog.h`, `etc_config.h` and
`setting.h` and can never be compiled into a ring-3 archive. The C
library needs the arithmetic and none of the machinery. Splitting it
follows the pattern already used for `geom.c` and `klineedit.c`: the
part that is pure becomes freestanding and gets compiled twice, and the
part that is POLICY stays where the policy is. A second copy of
Hinnant's `days_from_civil` in userland was the alternative, and it is
exactly the duplication the toolkit exists to end.

**`gmtime()` and `localtime()` are the same function, and `time()` does
not return UTC.** The RTC is read as local civil time with the selected
city's offset and DST already applied, and the filesystem's stored
epochs are derived from that same local reckoning -- which is an
existing, documented decision (see this file's entry on file
timestamps): it makes timestamps arithmetic-comparable without inventing
UTC handling the kernel does not have.

Given that, a `time()` returning true UTC would be worse than one that
does not. The number programs actually compare `time()` against is a
file's `st.modified`, and a UTC `time()` beside local epochs puts a
silent one-to-twelve-hour skew between two values that look like they
are in the same units. A caller cannot see it, and nothing in the type
system says otherwise. A documented simplification beats a hidden
inconsistency.

So the libc reports the system's own reckoning and says so loudly in the
header. The real fix is system-wide -- a stored UTC offset, so the
filesystem's epochs become true UTC and the offset is applied at the
display boundary -- and it is a roadmap item rather than something a
libc can do from above. When it lands, these two functions become
genuinely different and no other part of `<time.h>` changes, which is
the shape that made this safe to defer.

The same reasoning kept `clock()` out. C says it reports processor time;
the kernel tracks per-process `cpu_ns` but `SYS_PROC_INFO` is indexed by
table slot and a process cannot learn its own pid, so the honest options
were "absent" or "wall time under a name that means CPU time". Absent,
like `fork()` and `sin()` -- a link error says what is missing, and a
wrong answer in the right units does not.

## A choice's DISPLAY NAME comes from the data that produced the choice

`/etc/timezones` gained a fourth field — `losangeles,-480,us,Los Angeles`
— and `struct setting` gained an optional `choice_label` callback beside
its existing `choice`. Before this the timezone dropdown in System
Settings listed database tokens: `losangeles`, `newyork`, `saopaulo`.

**The mechanism that already existed and was not enough.**
`/etc/settings.d/<namespace>.<name>` carries `Choice.<value>=<display
name>` lines, and `setting_text_choice()` has always filled the ABI's
`label` field from them — `setting_abi.h` even used "Los Angeles" as its
worked example. That covers a list whose contents the file's author can
see: three mouse speeds, two boot targets. It does not cover a list that
is COMPUTED. The timezones come from `/etc/timezones` and the keyboard
layouts from a directory, so a `Choice.` line per entry would have to be
regenerated whenever the data changed — a second copy of the same
strings, kept true by somebody remembering, which is the shape this
project deletes on sight. **A setting whose choices are data supplies
their names from the same data.**

**Three sources, most specific first**, resolved in `SETTING_OP_CHOICE`:
`/etc/settings.d`, because that is where an installation renames or
translates one choice and it must be able to override a subsystem;
then the setting's own `choice_label`; then the value itself, which is
always presentable if not always pretty. A client draws `label`
unconditionally and never decides.

**The name is still the identity.** `losangeles` is what `timezone
<city>` matches, what `/etc/toyos.conf` stores, and what a setting's
value is; the display name is presentation, and nothing parses it back.
That is why the shell's `timezone` listing prints `Los Angeles
(losangeles)` rather than the pretty name alone — a list you cannot type
from is worse than an ugly one.

**Why the database and not a table in `tz.c`.** The file is the source
of truth for the cities themselves (that was settled when the hardcoded
array moved to `/etc/timezones`), so the names belong beside the rows
they name, where somebody editing the file can see and change them.

**And a row with three fields is still valid**, which is what makes an
existing machine work: `/etc/timezones` is seeded once and never
rewritten, so every disk that booted before this change carries 92
three-field rows. A row with no display name falls back to the
compiled-in table BY NAME, and then to the token. So an old disk shows
"Los Angeles" immediately, a hand-added city shows its token until
somebody names it, and a hand-RENAMED city stays renamed. Compare
`cursor_theme.c`: a data file that cannot be parsed costs its own
feature and nothing else.

## A partition is a block device, not an offset the filesystem carries

TFS3 was already volume-relative behind a `{base_lba, sector_count}`
seam, and `set_flat_volume()` was the one line pinning it to the whole
disk. The obvious change was therefore to have `vfs.c` set that seam per
candidate partition and probe each one. That would have worked, in about
forty lines, and it is not what happened.

**The offset lives one layer down instead**, in
`kernel/drivers/block/block_part.c`: a `struct block_device` that wraps
its parent and shifts every LBA. TFS3 needed *no change at all* — the
volume it sees is `{0, blk_sector_count()}` as before, and that now
means the partition because the device it was handed *is* the partition.

Three reasons, in the order they mattered.

**It is where Linux and Windows both put it.** Linux gives each
partition its own `struct block_device` carrying `bd_start_sect`, and
the filesystem driver never learns partitions exist; Windows stacks
`partmgr` between the disk driver and the volume manager. Teaching a
filesystem to add an offset is the layering both moved *away* from.

**It does not have to be done twice.** An offset inside TFS3 is an
offset TFS3 has; a second volume-relative backend (FAT is the obvious
one, and it is on the roadmap) would need its own copy, correct in its
own way. A partition device serves any backend that goes through
`blk_*`, which is the definition of the seam already there.

**It does not force a mount table.** The worry was `block.h`'s "one
active device", which
`docs/roadmap-details.md` had already flagged as being in mild tension
with `partition.c` enumerating partitions nothing could mount. A
partition device *replaces* its parent as the active device rather than
sitting beside it, so the active device stays singular and "Real mount
points" stays a separate, later project. One partition is mounted at a
time, exactly as one whole disk was.

**What it cost.** Two things had to learn the difference between "the
volume" and "the disk". `partition.c` reads through
`blk_disk_read_sectors()`, because `blk_read_sectors(0)` is now the
volume's first sector while the MBR is the *disk's* — a parser reading
its own partition would find no table at all. And `blk_whole_disk()` /
`blk_base_lba()` exist so that one question has one answer whatever is
mounted; they live in `block.c` rather than `block_part.c` so there is
no second call anybody can forget to make.

**And one bug it did not cause but did expose.** `partition.c` called
`ata_read_sector()` directly, predating `block.h` entirely — so on
`make run VIRTIO=1`, where there is no IDE controller, `parttable` was
reading a disk that was not there and reporting "no table" for it.
Nothing had noticed, because nothing consulted it.

## `SYS_MKPART` takes a table, not a sector

Writing a partition table from ring 3 needs *some* way to reach the
disk. The obvious primitive is a raw sector write, with `/bin/mkpart`
doing the MBR/GPT encoding itself. That was rejected, and the first
reason is the one that decided it.

**This kernel has no privilege model.** There is no uid; `SYS_QUERY` has
no check either. A general "write any sector" syscall is therefore a way
for any ring-3 process to destroy any filesystem, permanently, for the
convenience of one rare command. A syscall that takes a table
*description* can be checked — `partition_validate()` refuses overlap,
out-of-bounds and a partition sitting on the table itself — and leaves
no primitive behind for anything else to misuse. Linux's `BLKPG` and
Windows' `IOCTL_DISK_SET_DRIVE_LAYOUT_EX` are both shaped this way.

**It puts the encoder beside the decoder.** The CRC32, the 92-byte
header's field offsets, the 128-byte entry's, GPT's inclusive end LBA
and its mixed-endian GUID layout now have exactly one implementation
each, and a round trip through both is a real test — which is what
`kernel/drivers/partition_test.c` does, and what finally verified the
GPT half *in a running kernel*. See the entry above on why that was
previously only provable with a host-compiled harness.

**Some refusals are only possible in the kernel**, because it is the
only side that knows the disk's true sector count and what is mounted
from it.

**What `confirm` is not.** `MKPART_CONFIRM` is a speed bump, not a
permission check — any process can set the flag. It stops the accident
(a program that meant to read the table and passed a zeroed request),
not an attacker. It is said out loud in `abi/partition_abi.h` so that
nobody mistakes it for security, and so a future session adding uids
knows where the real check goes.

## A partitioned disk is never auto-formatted

Boot has always treated a readable disk that no backend claims as blank,
and formatted it with the default backend. That is right for a genuinely
empty disk and wrong the moment a partition table exists: the partitions
were just probed and none held a filesystem, which means *empty
partitions waiting for `fsformat`*, not free space to claim.

The failure mode if it did claim it is unusually nasty. A flat TFS3
format **survives** the presence of a GPT — TFS3 reserves volume blocks
0–7 precisely so a table can coexist — so the table would still parse
afterwards and `parttable` would still print both partitions, while a
whole-disk filesystem lay across the data they describe. A corruption
that passes its own diagnostic is worse than one that does not.

So a partitioned disk with no filesystem in any partition mounts
RAM-only and says so. **And it leaves partition 1 as the active block
device**, which is the part worth writing down: `fsformat` formats
whatever the active device is, so this is what makes "`mkpart`, reboot,
`fsformat`" put a filesystem *inside* partition 1. Restoring the whole
disk there was the obvious thing and it is the wrong thing — it would
make the one command you reach for next quietly undo the one you just
ran.

## `-boot order=d`, because a partition table looks bootable

Adding a partition table to `disk.img` broke every headless launch, in
the most confusing way available: QEMU hung with **no serial output at
all**, which reads as a kernel that died before its first `klog_write`.

SeaBIOS decides a hard disk is bootable by looking for `0x55AA` at LBA
0. That signature is part of an MBR — including the protective MBR a GPT
requires — so a partitioned data disk becomes, as far as the BIOS is
concerned, the thing to boot. It jumps into 446 bytes of filesystem data
and stops.

`toy-os.iso` is always the boot medium here and `disk.img` is always
data, so `-boot order=d` states that in the Makefile's `QEMU_RUN`, in
`tools/vm.py` and in `qmp_test.py`'s `launch_qemu_cmd()`. It was
harmless before and is load-bearing now. `tools/mkpart_test.py`'s
docstring had recorded this hazard years earlier for its own synthetic
tables; nothing had acted on it, because nothing else wrote a table.

**AMENDED: `disk.img` IS a boot medium now** — it carries GRUB and the
kernel, and an ordinary run boots it with `-boot order=c` (see "The
kernel lives on the disk" below). What did NOT change is the hazard, or
the rule that follows from it: an explicit order is still mandatory
everywhere, because the failure mode of getting it wrong is the same
silent hang. What changed is which order is right for which image, and
that is now derived from the image itself rather than fixed
(`install_grub.py`'s `boot_medium()`). The tools that build their own
images — `partition_test.py`, `virtio_boot_test.py`, `run_release.sh` —
still say `order=d`, and their comments now say why THEIR image is not
bootable rather than claiming disk.img is not.

## TFS2 was removed, and what removing a FORMAT costs

TFS3 had been the default for a long time and TFS2 was a probe-selected
second backend — 2348 lines of kernel plus 1071 of host tooling, serving
no disk anybody was creating. It was removed.

**The trap worth recording is what deleting a backend does on its own.**
`vfs.c`'s probe asks each backend whether a disk is theirs; a disk nobody
claims is "readable but unclaimed", and the blank-disk policy *formats*
that. So dropping a backend silently converts every disk in its format
into a blank one on the next boot, and destroys it — with a success
message.

That is the same rule as "an unreadable superblock is not a foreign
disk" above, arriving from the opposite direction: there the kernel
cannot read the superblock, here it reads it perfectly and no longer
speaks the format. Both end at **an unrecognised disk is not an
invitation**.

**A recognise-and-refuse guard was built, then removed at the
maintainer's request** once it was established that no TFS2 disks exist —
`disk_is_tfs2()` in `vfs.c`, plus matching refusals in `seed_disk.py` and
`check_layout.py`. It cost fifteen lines and it worked (a fabricated
TFS2 disk kept a marker at block 8 with the guard in, and had it
overwritten by TFS3's superblock with the guard bypassed). It is
recorded here rather than in the code because **the next format removal
needs the same decision made deliberately**, and the honest default is
that a guard is cheap and the failure it prevents is unrecoverable.
Removing it is reasonable when you know the disks do not exist; assuming
they do not is not the same thing as knowing.

**What was deliberately NOT collapsed.** `struct fs_ops` stays a registry
and `g_backends[]` a table with one row, because FAT32 is the next
backend and would have to undo the collapse. `volume_relative` stays for
the same reason: TFS2 was why it exists, but the hazard it guards — a
backend that reaches past `blk_*` being offered a partition and reading
the disk's LBA 0 instead — belongs to any such backend.

**`docs/tfs2-spec.md` is kept.** A byte-level description of a frozen
format cannot go stale, and several entries in this file reason from it —
TFS3's 32 KiB front reservation exists *because* TFS2's superblock
collided with the MBR, and that argument is only checkable against the
spec.

**What was lost, stated rather than glossed.** TFS2's `tfs_selftest()`
was the only test exercising double- and triple-indirect addressing;
TFS3 has the same three levels and nothing currently reaches past
single-indirect. That is on `docs/roadmap.md` as a real gap.
## The stock `disk.img` is partitioned, and a blank image is the only thing that decides

`disk.img` was a flat TFS3 volume at LBA 0 — a "superfloppy", which is a
real and legal layout but not what any installed OS looks like. It is a
GPT with the filesystem in partition 1 now.

**The reason is coverage, not cosmetics.** With a flat default, the
partition path was exercised by exactly one on-demand tool
(`tools/partition_test.py`) while every other test, every `make run` and
every GUI tool ran the flat path. That is the wrong way round for a
feature meant to be the normal case, and it is the same argument this
repo already makes for `ata nodma` and `VGA=std`: a path nothing
reaches is a guess. Both paths are still reachable — the live ISO's RAM
image is flat, `seed_disk.py --flat` builds a flat disk, and
`fs_switch_test.py`'s reformat cycle runs on whichever it is given.

**Only a BLANK image is affected.** `seed_disk.py` asks
`mkpart_test.py`'s `volume_of()` what shape an image already is and
keeps it — an existing checkout's flat `disk.img` stays flat and keeps
being seeded in place. `make clean-disk && make iso` is the opt-in, the
same one that moved TFS2 to TFS3. Nothing migrates by surprise.

**The host needed the kernel's question answered on its side.** Once the
image carries a table, every host tool that reaches into the filesystem
has to know where the volume starts, and hardcoding 2048 in each of them
is the pointer-somebody-must-maintain shape this repo keeps deleting. So
`volume_of()` is one function, beside the table encoders for the same
reason the kernel keeps its parser beside its writer, and `check_layout`,
`ls_test` and `init_test` all ask it. It returns the whole image for an
unpartitioned one, so no caller branches on the shape.

**`-boot order=d` became mandatory**, in three more launchers than the
partitioning commit had already fixed — see the entry above; a data disk
with a table looks bootable to SeaBIOS.

**One thing this DOES NOT settle, and FAT32 will force.** The scan takes
the first partition a backend claims. With one filesystem that is
unambiguous. With FAT32 (`docs/roadmap.md`) a disk could hold a FAT32
ESP in partition 1 and TFS3 in partition 2, and "first claim wins" would
make the ESP the root — which is wrong, and is why real systems name a
root volume rather than discovering one (Linux's `root=`, and an
`/etc/fstab` after that). The fix is a boot-line `root=` naming a
partition, not a cleverer probe order, and it belongs to the FAT32 work
rather than being guessed at now.

**AMENDED: that disk now exists, and half of the problem turned out to
be answerable by TYPE.** The stock image has an ESP in partition 2 and
TFS3 in partition 3. The scan skips a partition whose GPT type says it
belongs to the firmware — BIOS boot or ESP — so a future FAT32 backend
will not be offered the ESP at all, and `fsformat` cannot be aimed at
it. That is what real installers do rather than a trick: an ESP is
never a root filesystem candidate. What is still unsettled is the case
this cannot help with, TWO ordinary partitions both holding a
filesystem a backend claims, and `root=` remains the answer to that.

## The kernel lives on the disk, and `/boot` is FAT32 because GRUB cannot read TFS3

The kernel used to exist only inside `toy-os.iso`. Every boot — every
`make run`, every headless test — was a CD boot with `disk.img` attached
as data beside it, which is what a live CD looks like and not what an
installed system looks like. Moving the kernel onto the disk means one
medium holds the whole OS, and `-boot order=c` boots the machine the way
a real one boots.

**The obstacle was never the bootloader; it was the filesystem.** GRUB
boots off a disk trivially. What GRUB cannot do is READ TFS3: there is
no module for it, and writing one means a third implementation of the
format (kernel, host writer, and now a loader), living in GRUB's source
tree, under GPLv3, in an MIT repo — and rotting silently the next time
the on-disk format moves.

**What real systems do, and it is unanimous.** GRUB reads Linux's
`/boot` because it ships an ext4 driver. Where it does not — early
btrfs, ZFS, an encrypted root — every distribution's answer is the same:
a small separate `/boot` in a filesystem the loader already understands.
UEFI made that universal, and the ESP is FAT32 for exactly this reason:
firmware speaks only FAT. Windows does it with its own System Reserved
volume holding `bootmgr`. Writing a filesystem driver FOR THE LOADER is
the ZFS path, and it is why ZFS-on-root took years to become routine.

So: three partitions, which is what `tools/seed_disk.py` gives a blank
image.

    p1   1 MiB   BIOS boot   GRUB's core.img, embedded, no filesystem
    p2  64 MiB   ESP/FAT32   /boot/kernel.bin + /boot/grub
    p3  rest     data        TFS3, the OS's own filesystem

**Three alternatives were considered and rejected.** A GRUB module for
TFS3 — the "purest" answer to the literal question, and the most
expensive, for the reasons above. Blocklists (`multiboot2 (hd0)+N`),
which need no filesystem at all and are genuinely the smallest thing
that works — rejected because the kernel's size gets baked into the
boot sector, and because "a browsable `/boot`" is most of the point.
And doing nothing, which keeps a system that cannot boot itself.

**FAT32 also buys the two things next in line.** The partition is typed
as an EFI System Partition, so the roadmap's UEFI track becomes "add a
second core image to the same partition" rather than "design a second
disk layout". And when the planned FAT32 backend lands, toy-os can read
and write its own `/boot` — which is what it would take for this OS to
update its own kernel.

**The install is three writes, and each has a trap**
(`tools/install_grub.py` carries the detail):

- `boot.img` at LBA 0, patched with the LBA of `core.img` at offset
  `0x5c` **and with the existing partition table and disk signature
  (bytes `0x1b8`–`0x200`) kept**. Forget the second and the machine
  boots beautifully off a disk that now describes no partitions.
- `core.img` into the BIOS boot partition, contiguous, with the block
  list in its first sector's last 12 bytes pointed at the rest of
  itself. That is what `grub-bios-setup` patches; a `core.img` written
  without it loads one sector and jumps into nothing.
- The FAT volume, written with `mtools` (`mformat`/`mcopy` on a
  `file@@offset` window) — no root, no loop device, no mount. That is
  the property that makes this work in a plain checkout and in CI.

**Nothing reformats anything.** `install_grub.py` refuses an image with
no boot partition rather than reshaping one, and the build passes
`--optional` so a `disk.img` that predates this layout keeps building
and keeps booting — off the ISO. `make clean-disk && make iso` is the
opt-in, the same one that moved TFS2 to TFS3 and flat to partitioned.
The FAT volume is formatted ONCE and then written into, so anything the
OS itself eventually puts in `/boot` survives a rebuild.

**The boot medium is derived, not declared.** `boot_medium()` asks the
image whether GRUB is on it, and the Makefile, `vm.py`, `qmp_test.py`,
`boot_smoke_test.py` and `serial_console.py` all ask that one function.
Deriving rather than flag-flipping matters because the failure is
asymmetric: guessing CD when the disk was bootable costs a slightly
slower boot, while guessing DISK when it was not hangs with no serial
output at all (`0x55AA` at LBA 0 is all SeaBIOS checks). `BOOT=disk` /
`BOOT=cd` and `--boot` override it.

**The ISO is not deprecated and cannot be.** The live and demo images
boot with NO disk attached — that is the whole property they test — and
a release is a downloadable ISO. What changed is which medium the
ORDINARY path uses, not how many there are.

**The kernel had to learn that two partitions are not its business.**
`vfs.c`'s scan now skips a partition whose type says it belongs to the
firmware (`partition_is_firmware()`: GPT BIOS boot or ESP, MBR type
`0xEF`). Without that, the "no partition holds a filesystem — leave the
first one active" fallback hands `fsformat` the 1 MiB partition holding
GRUB. That is not a hypothetical: it is what the positive control did,
and `tools/partition_test.py`'s last phase is the check that it stays
fixed, asserting the partition NUMBER left active rather than that a
cheerful line was printed.

**What this does not do.** No UEFI boot (BIOS/i386-pc only, though the
ESP is now where a UEFI loader would go). And nothing installs toy-os
onto a disk from inside toy-os; `disk.img` is still built by the host.

*(This entry used to end "no `/boot` visible from inside the running OS,
because there is no FAT driver yet". There is one now —
`kernel/fs/fat32.c` — and the ESP is mounted at `/boot`, read-only. See
the mount-table entries below.)*

## A drive's root is a partition, and where there is no drive it is ramfs

Two rules landed together, and neither works without the other.

**The root comes from a PARTITION.** A whole-disk volume — a
"superfloppy" — is a legal shape that no installed system has had in
twenty years: Windows will not boot one, and no Linux installer
produces one. `kernel/fs/vfs.c` refuses it by name, writes nothing to
it, and says what to run instead. What that buys is one probe path
through the code that decides what to mount, on the one decision where
being wrong in the permissive direction destroys data — this file
already carries an entry about a backend removal turning every disk of
that format into "blank, go format".

**And nothing is auto-formatted any more.** The blank-disk policy wrote
a fresh filesystem over any readable disk nobody claimed. With the
whole-disk shape refused there is nowhere left for it to write — a
partition's contents are the partition's business — so
`probe_and_mount()` lost its `allow_format` parameter entirely. The
rule it carried is now true by construction.

**Which needs somewhere to land when there is no usable drive**, and
before this that place was a fiction. `vfs.c` mounted TFS3 with no disk
under it, TFS3's `init()` correctly refused, and the machine announced
`fs: active backend: tfs3 (RAM-only)` while every `fs_*` call failed —
two comments in the tree pointing at each other about a mode neither
implemented. So the refusal above could not have shipped first: its
failure path IS the fallback.

**ramfs is a real backend, not TFS3 on a RAM disk.** The cheap move was
to point `block_ram.c` at an empty buffer, and it is the wrong shape:
it means running an on-disk format over memory — superblock, block
groups, backups and a JOURNAL, all paying for durability that memory
cannot have — and it fixes the size at mount, where a filesystem in the
heap can grow into what is actually free. Linux draws the same line;
`/dev/ram*` survives mainly for compatibility. It is `ramfs` and not
`tmpfs` because tmpfs can page to swap and this kernel cannot.

**Three implementation decisions worth not re-litigating.** A node
knows its parent and nothing knows its children — listing scans, but
create, delete and rename are each a single field write that cannot
leave a dangling link, where first-child/next-sibling links are three
fields that must agree. File data is chunked at 4 KiB because
`heap_os_alloc()` asks `pmm_alloc_contiguous()`: one buffer per file
would ask for 6144 contiguous frames for a 24 MiB file and fail on a
fragmented machine while `meminfo` still showed memory free, which is
the worst diagnostic shape available. And it has a byte budget — half
of free memory at mount, tmpfs's own default — because this kernel has
no OOM killer and ramfs draws from the same frames as the allocator
everything else depends on.

**`init()` became three-valued** (1 / 0 / -1, the shape `probe()`
already had) because "mounted but not persistent" and "did not mount"
had been the same answer, and that is precisely what let the log lie.

**AND RAMFS IS NOT IN `g_backends`, WHICH IS NOT AN OVERSIGHT.** That
table is the ON-DISK registry: `fs_format_backend()` walks it and wipes
every *other* backend's signatures before formatting with the one
named. Registering ramfs there would have made `fsformat ramfs confirm`
erase a working TFS3 superblock and then fail its own persistence
check. It is reached through its own pointer instead. The general
lesson is worth more than the instance: **a registry is not a neutral
place to put something** — it is a list of things every consumer of
that registry will act on, and the consumers are not all in front of
you when you add the row.

**What this cost elsewhere.** The live image gains a partition table
rather than an exemption (the exemption would have lived in the one
function whose job is to have one rule); `seed_disk.py --flat` is gone;
`tfs3_v1_test.py` builds a partitioned image. And an existing flat
`disk.img` stops mounting — `make clean-disk && make iso`, the same
opt-in TFS2→TFS3 and flat→partitioned both used. Nothing migrates
silently.

## Why the VFS grew a mount table, and why it stayed small

`fs_ops.h` argued for years that one active backend was the whole scope:
"nothing needs two filesystems simultaneously, and it is meaningfully
more code for a capability that would sit unused." That was true and
stopped being true the moment `/boot` mattered. The disk carries a FAT32
ESP holding the kernel image that started the machine, and the running
system could not read it — `ls /boot` showed an empty directory because
there was nothing there to show.

**Why not special-case `/boot`.** The cheap version is one extra backend
pointer consulted for paths starting with `/boot`. It is also exactly
the `if/else`-that-grows shape this repo enforces against
(`tools/check_dispatch.py` exists because `syscall.c` reached 37
branches and nothing noticed), and the second mount would have to undo
it. The table is about eighty lines and a longest-prefix lookup.

**Why not Linux's size.** Linux keeps a TREE of `struct mount`, crosses
mount points during path-component walking, and carries per-process
namespaces, bind mounts and automounts. toy-os resolves a whole
normalized path against a flat table of at most six entries, once, at
the `fs_*` boundary — because its paths are already normalized before
they arrive (`api/fs.h`), which removes the entire reason Linux
resolves per component. Copy the shape, not the size.

**What the old prediction got right.** `fs_ops.h` said the struct
"doesn't need to change shape, just get looked up differently", and that
held: not one operation's signature changed. What DID have to change is
where a backend's volume comes from — `probe`/`format`/`wipe`/`init`
take a `struct block_device *` now instead of reading `blk_active()`,
which is Linux's `super_block->s_bdev` arriving for Linux's reason.
With two mounts there is no single active device a backend could
correctly assume, and one that assumed anyway read the wrong volume and
reported no error.

**The unmount refusals are the design.** The root cannot go (nothing
would resolve a path), a filesystem with an open file on it is refused,
and so is one with another mounted underneath. Linux has `-l` and `-f`
for the first two; both exist because a network filesystem can hang,
which nothing here can, and both leave a window where a process holds a
file on a filesystem that is gone. The open-file check needed no new
bookkeeping: `struct open_file` already records a descriptor's absolute
path, so the answer is a walk of the fd table — and bookkeeping that
exists only to answer one question is the kind that drifts out of step
with what it counts.

**What is NOT built, stated plainly.** A backend may be mounted ONCE
(`fs_ops.max_mounts`, and every backend declares 1). The milestone's own
suggested first proof — a second TFS3 image at `/mnt`, to isolate "does
dispatch-by-prefix work" from "does the new driver work" — is therefore
still open. What replaced it as the isolating proof is `ramfs` at
`/mnt`: a third backend, no volume at all, mounted and unmounted by
`kernel/fs/mount_test.c` on every ktest run, which exercises the same
dispatch with none of FAT32's code in the path. Raising `max_mounts`
needs per-instance backend state AND an opaque per-mount handle threaded
through every op; neither is worth doing until something wants two
volumes of one format.

## Why `/boot` is mounted read-only, and where that policy lives

The ESP holds `core.img`, `grub.cfg` and `kernel.bin` — the three things
that have to be intact for the machine to boot at all. A new FAT driver's
first outing is not where those should become writable by accident, and
FAT has no journal, so an interrupted write leaves whatever the last
completed sector left.

`mount -w`-style access is deliberately two commands (`umount /boot`
then `mount 2 /boot`) rather than a flag on the automount, because the
person typing them has then said it twice. Linux's convention is the
same: an ESP in `/etc/fstab` is conventionally `ro`, or not mounted
until something needs it.

**The policy is in `mount_boot_auto()`, not in the driver.**
`kernel/fs/fat32.c` contains no mention of a bootloader — that
separation is Linux's (`fs/fat/` is generic; the ESP is an ordinary
mount) and Windows' (FASTFAT likewise). A driver that knew what it held
would have to be told again for every other FAT volume.

## Why a FAT32-only driver, and why it accepts an out-of-spec cluster count

FAT12 and FAT16 are not "FAT32 with smaller numbers": the root directory
is a fixed-size region rather than a cluster chain, and the FAT entries
are 12 or 16 bits with a nibble-packed edge case. Supporting them is a
second set of paths through every function in the file, for a format
nothing on this machine uses. A volume that is not FAT32 is REFUSED by
name at `probe()`, which is the parser-rejects-rather-than-guesses rule.

That choice has one visible consequence. Microsoft's spec picks the FAT
type from the **cluster count** (below 65525 is FAT16), which exists so a
driver implementing all three can tell them apart. This one implements
only FAT32, so it discriminates on the BPB's FAT32-only fields instead —
`fat_size_16 == 0`, `root_entry_count == 0`, a nonzero `fat_size_32` —
and will therefore happily mount a small volume that `fsck.fat` would
call FAT16. That is what lets `kernel/fs/fat32_test.c` use a 512 KiB
image instead of a 34 MiB one, which matters because the test volume is
a `kmalloc()` and `pmm_alloc_contiguous()` fails on a fragmented machine
long before it fails on a small one. `format()` still picks a cluster
size that clears the 65525 floor wherever the volume is big enough, so
nothing toy-os *writes* is out of spec.

## Enumerating every disk is a separate question from choosing the root

The block layer used to answer both with one line: whichever driver
found a disk first got to run, and by running got to carry the root.

    if (!blk_virtio_init() && !blk_ahci_init()) blk_ata_init();

Two decisions, fused, and the fusion was invisible because **every test
in this repo boots a machine with exactly one disk** — the one shape
where "which driver ran" and "which disk is root" cannot disagree.

Neither Linux nor NT fuses them. A Linux driver registers every device
it probes (`sda`, `vda`, `nvme0n1`) whether or not anything mounts it,
and `root=` on the command line names the one to boot from — resolved
later, by the VFS, from a device that is already there. NT's PnP manager
builds a device object per device and the boot path comes from BCD's
`osdevice`. In both, *finding* a disk is not *claiming* it.

toy-os now does the same: every driver runs, every device it finds goes
into a table with a name, and the root is chosen afterwards from `root=`
or from registration order. Registration order is kept as the default
precedence — virtio, then AHCI, then ATA — because that ordering was
measured (`block_virtio.c`) and nothing about this change disputes it.

**What it cost to have them fused**, both reported from real hardware
rather than found here: a machine with a virtio disk had its SATA drive
enumerated by nobody, so `parttable` could not see it and `mount` could
not reach it; and a live boot registered the RAM image and skipped disk
init, so a live session could not touch the machine's own disks — which
is most of what a live CD is for.

**Why a name and not an index.** The table could have been positional,
with `root=2`. It is a name because a positional identity silently
renumbers when enumeration order changes, which is the failure mode
UUIDs exist to solve and which a person cannot see happening. The scheme
is `<driver><index>` rather than Linux's `sd`/`vd`/`nvme` split: that
split is historical (different subsystems grew different letters), the
driver is a genuine lever here (`novirtio` and `noahci` are boot words a
person uses), and there is no `/dev` for `/dev/sda1` to be a path into.

**Why the partition number is the TABLE's.** `ata0p3` is the third entry
in the partition table even when the two before it are the firmware's
and were skipped, so it lines up with `parttable`. Numbering by slot
order instead made the root partition `ata0p1` — a name that agreed with
nothing a person could see, and that `root=ata0p3` would have missed.

**What this deliberately did NOT fix.** A second volume of the same
format still did not mount at the time — `fs_ops.max_mounts` was 1 for
every backend. Enumeration was never meant to change that; it changed
whether the device could be NAMED and reached at all, which was the
actual blocker. Per-mount state came later; see the entry below.

## A backend's volume state is per mount, and every op is handed it

An installer has to MOUNT its target to copy files onto it, and it
could not: `fs_ops.max_mounts` was 1 for every backend, because each
kept its volume in module-level statics. A second mount would have
repointed one set of them and left the FIRST mount reading the second's
volume — silently, which is why the table refused it by name.

**What real systems do.** Linux puts every per-mount fact in
`struct super_block` (`s_bdev`, `s_fs_info`) and passes it to every
operation; a Windows filesystem driver keeps a per-volume control block
and the IRP names it. Neither swaps a global, and the reason is SMP: one
current state would serialise the whole filesystem across cores.

**toy-os now does the same: every `fs_ops` op takes `void *st`, the
mount's own state, and inside a backend it travels as `sbi`** (Linux's
name for a superblock's private info). `vfs.c`'s `FS_OP(m, op, ...)`
passes `m->state` itself, so a call cannot be handed another mount's.

**It first did it the other way, and why that stopped being right.**
The first version kept a `static ... *S` per backend and had the mount
table make a state CURRENT around every call (`state_activate`, with
`mount_enter`/`mount_leave`). The filesystem was one global critical
section with preemption held off, so a current pointer cost nothing
and saved a handle threaded through twenty signatures in ~5,800 lines.
The argument for the handle was SMP, and that looked far off.

It stopped being right before SMP arrived. Once the fs lock became a
sleeping mutex and disk waits slept under it (docs/blocking-design.md),
the next step -- dropping the lock across a data-block I/O, or one lock
per mount (docs/fslock-design.md) -- lets a second caller run while
the first is asleep mid-operation. With a global "current mount" the
sleeper resumes on whatever the other caller activated. Passing the
state is what makes a lock that can be dropped possible at all, so it
became stage 1 of that plan (2026-09-24, starting bf951457).

**The one thing activation did that is still needed.** tfs3's
activate committed ANOTHER mount's deferred journal transaction before
the incoming mount ran, because the journal staging is file-scope while
the deferred transaction belongs to one mount. That survived stage 1
as `t3_enter()` at the top of every tfs3 op, and went in stage 2 when
the journal moved into the mount. Dropped without that move, `fsformat`
on the running root could have left a commit pending for the old volume
that `idle` would then land on the new one.

**Why the state moved into a struct rather than being saved and
restored.** The alternative was to leave the statics alone and have each
mount carry a saved copy, memcpy'd in and out when the current mount
changed — a much smaller diff, and the shape `struct t3_saved` already
had for `format()`/`wipe()`. It was rejected on one property: the field
list is maintained by hand, so a new static forgotten is silent
cross-volume corruption, and nothing catches it. A struct does not
remove that hazard (a new field can still be declared outside it) but it
makes the right place obvious rather than remembered, and it costs
nothing per call instead of ~2.8 KB per alternation.

**Nesting needs nothing now.** `listdir_collect()` calls `fs_stat()`
from inside an `fs_list()` callback, which under activation meant a
whole enter/leave INSIDE the walk -- and the first version, clearing
rather than restoring, panicked the first time init read a directory.
A call that carries its own state has nothing to restore.

**What it deleted.** `struct t3_saved` and its save/restore wrappers
around `tfs3_format()`/`tfs3_wipe()`; `fat32_probe()`'s save/restore of
`g_v`; and the rule that a KTEST driving a backend directly must put the
machine's own `/boot` back afterwards. All three were one mechanism —
protecting a single shared instance from a second user — and all three
stop existing once there can be two.

## The installer is five ordinary operations, and the kernel never learns what a bootloader is

toy-os can install itself now (`/bin/install`). The question worth
recording is what the KERNEL had to grow for that, because the obvious
answer -- an install syscall -- is the wrong one.

**What real installers do.** Anaconda, the Debian installer and Windows
setup are all ordinary userland programs driving ordinary primitives:
partition an disk, make a filesystem, mount it, copy files, run
`grub-install`. None of them has a kernel interface of its own. The one
place they need privilege is writing raw sectors, and on Linux that is
just `open("/dev/sda", O_WRONLY)` gated by uid 0.

**toy-os cannot copy that last part, and that is what shaped this.**
There is no privilege model here, so a general write-any-sector syscall
would be a way for any process to destroy any filesystem for the
convenience of one rare command -- the reasoning `SYS_MKPART` already
carries. So the bootloader step is `SYS_INSTALL_BOOT`: a specific,
checkable operation that takes two IMAGES and a disk, and derives WHERE
they go from the target's own partition table. A caller cannot point a
core image at a filesystem, because it never names an LBA.

**The kernel still knows nothing about GRUB.** It never parses either
image. What it does is apply the two patches that depend on where the
bytes landed -- the core image's LBA at offset 0x5c of the boot sector,
and the block list at 0x1f4/0x1fc of the core image's first sector --
preserve the disk's own partition table at 0x1b8-0x200, and write the
boot sector LAST so a power cut leaves the old one intact. Those are
facts about placement, which is the half only the kernel knows. Handing
it two files of the wrong kind produces a disk that does not boot rather
than an error, and that is the right trade: checking would mean carrying
a second implementation of somebody else's format in ring 0.

**Where the images come from, and why `/install` is in the root.** The
first version read them out of `/boot`, where `tools/install_grub.py`
stages them beside the kernel -- which is where `grub-install` puts them
on a real system, and which works perfectly on a disk boot and not at all
on a live one. **A live session has no `/boot`:** GRUB loads the kernel
and a filesystem image into RAM, and after that nothing drives the
medium, because toy-os has no USB mass-storage driver. `/boot` is an
empty mount point and the ESP does not exist.

That matters because **installing from live media is how a real machine
gets toy-os** -- it is the only arrangement where the target disk is not
also the one being run from. So the payload is `/install` in the ROOT:
`kernel.bin`, `grub.cfg`, `boot.img`, `core.img`, staged into the seed
tree so that every toy-os filesystem carries it and `/bin/install` reads
ONE path on every medium. The alternative -- read `/boot` when it is
there, fall back otherwise -- is two code paths where one gets tested and
the other is found broken later, which this project has been bitten by
enough times to have a rule about it.

The cost is a second copy of the kernel in every root image (~4.7 MB) and
the drift it invites, since a running system now has `/boot/boot/
kernel.bin` and `/install/kernel.bin` and nothing keeps them equal after
a hand edit. Both come from one build at seed time, and updating a kernel
in place is not a thing toy-os can do yet anyway (roadmap).

The kernel reading the images off the running disk's own LBA 0 and BIOS
boot partition was the third option: no files at all. Rejected because it
is guessing, and because it cannot work from a live ISO either.

**The layout is a constraint and is stated as one.** `core.img` carries
its prefix baked in at `grub-mkimage` time, so the copy this installs
only finds its config if the ESP is partition 2 on the target as well.
Real installers generate a fresh core image per target; toy-os has no
`grub-mkimage`, so it reproduces the layout the image it copies expects.
Said out loud in `/bin/install`'s own comment and in its page, because
a session reordering those partitions would get a disk that reaches GRUB
and stops at a rescue prompt with nothing explaining why.

**Two things had to change underneath, and both were pre-existing
defects nothing had reached.** `fat32_format()`'s FAT-size solver could
OSCILLATE between two adjacent sizes and fall out on the smaller,
leaving a FAT eight bytes short of the cluster count its own layout
implies -- a volume this driver formats and then refuses to mount, for
about one size in sixty, found by a 64 MiB ESP rather than by
`fat32_test.c`, whose only volume was 512 KiB. And `SYS_LISTDIR`
truncated at 256 entries with no way to page, so copying GRUB's
305-file module directory came back short; `SYS_LISTDIR_AT` is the
offset the ABI comment had already named as the fix.

**A fourth, and it needed real hardware.** The partition re-read after
`mkpart` ADDED windows rather than replacing them, which is invisible
when the old and new layouts match -- and every test reinstalled onto a
disk whose layout already did. A 512 MB image `dd`'d onto a 119 GB disk
does not: the old `p3` was 445.9 MB, the new one 119.1 GB, so the re-read
made a SECOND window with the same name and `blk_device_by_name()`
answered with the stale first. The install wrote a correct GPT, a correct
bootloader and a correct ESP, and formatted the wrong window -- a machine
that boots fine on 0.4% of its disk. **The lesson is about the fixture,
not the code:** the reinstall test used a copy of `disk.img`, whose
partition was already the size the new table would give it, so the one
thing that could differ never did.

**And a third, found by `/install` itself.** `tools/tfs3_writer.py` --
the host-side seeding tool, a SECOND implementation of TFS3's on-disk
format -- wrote direct + single-indirect only, ~4.05 MB, with its own
docstring calling that "a deliberate cap". The kernel has grown block
maps through all three indirect levels automatically since TFS3 landed;
the host tool had simply never been handed anything big. `kernel.bin` is
4.7 MB, so a live image could not carry the payload it installs from.
Fixing it to double-indirect alone would have moved the cliff to 4 GiB
rather than removing it, so all three levels went in behind one recursive
walker/builder pair -- with the triple level marked UNEXERCISED, because
it is.

**The general shape, which this project keeps rediscovering:** a
capability nothing exercised was broken in a way no test could see,
because every test used the sizes that happened to work. A second
implementation of a format is only as complete as the biggest thing
anyone has fed it.

## The parent directory is resolved as its own step, rather than giving `fs_ops` an errno

`open("/tmp/no/such/probe.txt", O_WRONLY|O_CREAT)` returned a working
descriptor and created nothing. `sys_open()` called `fs_touch()` and
threw the answer away; TFS3 had refused correctly all along. `touch`
exited 0, `tftpd` acknowledged every block, and `tools/remote.py sync`
reported a whole tree as sent into a directory that did not exist -- the
failure was silent at the syscall, at the program and at the protocol.

The obvious fix is to make `fs_touch()`/`fs_mkdir()` return a negative
errno instead of 1/0, since the backends already know exactly why they
refused. That is the better long-term shape and it was NOT taken here.
`fs.h`'s 1/0 contract is implemented three times (TFS3, ramfs, FAT32),
consumed by every `fs_*` caller in the kernel and by `apps/shell_fs.c`,
and the diff would be far larger than the
defect -- a one-site discarded return.

What went in instead is `parent_dir_err()` in `kernel/fs/fs_syscalls.c`:
`k_path_dirname()` the target, then require the parent to exist and to
be a directory. It answers `-ENOENT`, `-ENOTDIR`, or 0, and both
`sys_open()` and `sys_mkdir()` ask it before creating anything.

**This is Linux's shape, not a workaround for not having the other
one.** `path_parentat()` resolves the parent as a separate step, which
is precisely why `ENOENT`/`ENOTDIR` come from the walk while `ENOSPC`
comes from the create -- the split exists in a kernel that could easily
have threaded one error code through instead. Windows does the same,
distinguishing `OBJECT_PATH_NOT_FOUND` from `OBJECT_NAME_NOT_FOUND`.
Splitting the walk from the create is what lets a single 1/0 create
still yield three honest answers, so `sys_mkdir()` stopped guessing
`-ENOENT` for a full record table in the same change.

Two things to know if you edit this. The check costs an `fs_exists()`
plus an `fs_is_dir()` and runs only on the creating path, so an ordinary
`open()` of an existing file pays nothing. And the descriptor is
reserved BEFORE the filesystem work and closed again if that work fails
-- `get_unused_fd_flags()`/`put_unused_fd()`'s order, kept so that a
full descriptor table is still reported without having created a file
first, and so a refusal cannot leak the slot it took.

`fs_write()`'s return was discarded on the adjacent line for the same
reason, which meant an `O_TRUNC` open whose truncate failed handed back
a descriptor over a file that still held its old contents. That is
`-EIO` now.

## A config file can have `[sections]`, and the section is an argument

`etc_config` parses `[name]` headers now. Every key after one belongs to
that section until the next header; a key before any header is at TOP
LEVEL, which is what every file written before this is made of. The four
unsuffixed entry points mean top level, so no existing file and no
existing call site changed meaning.

**Why an argument and not a dotted key.** git config parses sections and
then flattens them: its API is one namespace, `section.key`. That was
the cheaper option here by a wide margin — no new entry points, no call
sites touched — and it was rejected because this project already spends
the dot. `/etc/settings.d` files carry `Choice.losangeles=Los Angeles`
and a setting is addressed `system.font_size`, so a flattened parser
makes `[Choice] losangeles` and a literal `Choice.losangeles` the same
query, with a precedence rule invisible at the call site. Identity here
is `(section, key)`, and it is spelled that way: `etc_config_get_in`.
That is GKeyFile's shape, and systemd units, NetworkManager keyfiles and
`.desktop` entries are all read through it.

**Sections earn their place where the NAME IS DATA.** Where a section
name is a schema word the code already knows — `[Desktop Entry]`,
`[Unit]` — a dotted key prefix does the same job, and the payoff is only
compatibility with files written elsewhere. The case that could not be
expressed any other way is `/etc/net.conf`: it keyed a card by its own
MAC address, so a card could carry exactly one fact — its name — and
`dhcp` could only ever be a machine-wide answer. A section per card
carries `name` and its own `dhcp`, and `etc_config_section_count/_name`
is how `/bin/netd` asks the FILE which cards it describes rather than
knowing their names. Nothing else in the API needed enumeration; it was
built with the parser rather than after it because retrofitting it would
have meant a second pass over the writer.

**A new key lands at the end of its own SECTION, not of the file.** The
end is after that section's last `key=value` line — before any trailing
comment block, because in every INI-shaped format a comment sitting
above a header belongs to the section below it, and before the next
header. Appending at EOF is the trivially correct rewriter and was
rejected: it files the key under whatever section happens to be last, or
grows a duplicate header per edit and forces the reader to declare
first-wins or last-wins. A section that is not there yet is appended
with its header; a top-level key in a sectioned file goes ABOVE the
first header, which is the same rule stated for the top-level scope.

Two things a caller has to know. **Removing the last key of a section
leaves the header and its comments** — deleting what somebody wrote in
order to tidy up what the machine wrote is the wrong trade. And a
**section name the parser could not read back is refused at the write**
(`[`, `]`, `#`, a newline, or over `ETC_CONFIG_SECTION_MAX`), because
the key would otherwise land in whatever section came before it.

**A repeated header is ONE section.** Both runs are searched for a key,
and enumeration reports the name once, so a walk cannot hand the same
card back twice. A name is a section, not a position.

**One file per setting still stands.** `/etc/settings.d` was designed
around not having sections, and the arguments for it were never only
about the parser: a directory of small descriptors is what makes a
setting's text addable one at a time and a malformed one cost exactly
that setting. This does not reopen it. What it does retire is the half
of that reasoning which said teaching the parser sections would change
`etc_config_get(file, key)` at every call site — it did not, because the
section is an added argument rather than a changed one.

## Refreshing the id databases is a program, not `wget -O`

`hwdata update` fetches `pci.ids` and `usb.ids` and replaces them. The
obvious alternative was a line in `lspci`'s page saying to run
`wget -O /usr/share/hwdata/pci.ids https://pci-ids.ucw.cz/v2.2/pci.ids`.
That command works today and needs no code at all.

It also opens the file with `O_TRUNC`. A download that dies halfway --
which is the normal outcome on a flaky link, and the only outcome behind
a captive portal -- leaves a **truncated database that still parses**.
Both files are line-oriented and the readers stream them, so half a file
is a valid file with half the vendors in it: some devices resolve, the
rest show numbers, and nothing anywhere says why. The failure is
indistinguishable from the file having been stale in the first place,
which is the condition the person was trying to fix.

So the download goes to `<path>.new` beside the target and is renamed
over it only after it has been checked. `docs/update-design.md`
prescribes exactly this shape for `/bin/update`, and a database refresh
is a strict subset of its stage 3 -- one file, nothing mmaps it, no
self-overwrite, no `/boot`.

**The check is two numbers and neither is enough alone.** At least
64 KiB, and at least 100 lines shaped like a vendor entry. The size
floor rejects an error page, a redirect body or a portal splash; the
vendor count rejects a page that is merely large, which a 4000-line HTML
404 is. Counting only vendor lines and not device lines is deliberate: a
file of nothing but indented device lines is malformed, and counting
them would hide that.

**The swap is three steps because `SYS_RENAME` refuses an existing
target.** It is not POSIX `rename()`, which replaces. Making it replace
is a filesystem change with journal credits behind it, so the program
does the work instead: move the old copy to `<path>.old`, rename the new
one into place, delete the old. If the middle step fails the old copy is
renamed back, and there is never a moment when neither file exists under
a name something can name.

**The URLs are `/etc/hwdata.conf` rather than constants**, and the
reason is testability as much as mirrors. `tools/hwdata_test.py` points
`--from` at a Python `http.server` on the host, so the whole thing is
checked -- including both refusals and the survival of the old file --
with nothing leaving the machine.

**Downloading at BUILD time is still refused**, and this does not
reverse that decision ("pci.ids is bundled in `data/`" above). A build
that reaches the network breaks offline builds and adds a supply-chain
input; a command a person types on a running machine is a different
thing entirely, and nothing here runs it on a timer.

**Fetching is not distributing.** Both files are redistributed with this
system under their BSD terms. A copy this command fetches is one the
person running it obtained for themselves -- the same distinction
`tools/fetch_extras.py` draws for the Doom IWAD, and the reason the
fetch needs no licence prompt while publishing an image carrying the
result would.

## A committed transaction that cannot be applied takes the volume read-only, and the backend calls UP to say so

`txn_commit()` used to answer 0 for a failure before the commit point
and for one after it. The two are opposites. Before it, nothing is
durable and the caller must free what it allocated or leak. After it,
the journal is on the platter and WILL be replayed, so those blocks are
live even though the operation failed -- freeing them hands the
replayed inode pointers to space something else can take, and a later
transaction can overwrite the outstanding journal before anything
replays it.

jbd2 draws the same line and answers it the same way: a commit-time
failure calls `jbd2_journal_abort()`, and the filesystem stops writing
(`errors=remount-ro`, the default most distributions ship). NTFS sets
the volume's dirty bit and leaves it to chkdsk. Neither ever undoes an
allocation past the commit point, because the recovery pass is the only
thing that can still finish the operation, and it cannot finish it if
something else has taken the blocks. **Panicking** was the other real
option (ext4's `errors=panic`) and was declined: it loses the machine
for a fault the next mount can repair, and it makes the fault-injected
test kill the guest it is running in.

**Why one wrapper instead of sixteen edited call sites.** The three-way
answer is `txn_commit_raw()`'s; `txn_commit()` keeps the int contract
every caller already has and consumes the third outcome in one place --
cancelling the allocation log so the caller's own `alog_rollback()`
frees nothing. That is the same argument `txn_begin()` already makes
for forcing a deferred flush in the one function every transaction
passes through: a rule applied at sixteen sites is a rule a
seventeenth will not know about.

**Why the backend calls up into the mount table.** Enforcement and
reporting are different jobs. `t3_state.readonly` gates
`vol_write_sectors()` -- one place, and the backend is the only writer,
so nothing can get past it. But a volume that has silently stopped
accepting writes is the worst kind of failure, so `mount_force_readonly()`
sets `MNT_RDONLY` and `df` shows it. That call is a backend reaching
UP, which nothing else here does; the alternative was a new `fs_ops`
hook the mount layer polls, which is more machinery for one fact. It
has to be both, not just the mount flag, because `fs_ops.init()` runs
BEFORE `mount_add()` records the mount -- a replay that fails at mount
time has no entry to flag yet.

**What it costs to test.** A commit issues two barriers with the commit
point between them, so a countdown injector always hits the first;
`fault_fail_block_flushes(skip, count)` exists for the skip, and that
is the only reason it is shaped differently from every other injector
here. And the outcome is a read-only volume, which on the root would
fail every test after it -- so the post-commit KTEST skips unless a
second TFS3 mount exists, and `tools/multidisk_test.py` is the guest
that has one.


## Why TFS3's write caches outlive one operation, and the read cache's does not

`pcache`/`g_mcache` hold the pointer tables a write walks. They were
dropped at the start and end of every `do_write_inner()`, so a
sequential write re-read the same leaf and middle tables on every
syscall -- measured on a SATA SSD at ~6 metadata read commands per
64 KiB write, about half of them these tables.

They are keyed by block number, so keeping them is free until a block
changes identity. The only way that happens is a free-and-reallocate,
or a write that goes around the cache; both are now explicit
(`map_cache_forget()`). That is strictly narrower than the rule the
READ-side cache takes -- `vol_write_sectors()` drops `rcache` on any
write at all -- and the asymmetry is the point: the read walk cannot
tell a table block from a data block, and being wrong once serves
another file's data. The write walk allocated the table, so it can.

**Why not a general buffer cache instead.** That is the honest
alternative and it is on the roadmap, but it answers a different
question: a buffer cache makes a re-read cheap, while this makes the
re-read not happen. The re-read was not cold -- it was the same three
blocks, every syscall, on a path that already knew their numbers.

**The measurement.** 64 MiB sequential write on a Samsung SATA SSD:
44.1 -> 54.0 MB/s, with read commands per write syscall falling from
5.9 to 4.0 and read SECTORS more than halving. Random 4 KiB write
4.48 -> 5.47 MB/s.

## Why `SYS_WRITE_MAX` is 256 KiB, and why that is not a driver question

The constant does not size a disk command -- it sizes a TRANSACTION.
Every `fs_write_range()` flushes its own dirty pointer tables, bitmap
and group descriptors before staging the inode, so the syscall cap
decides how many times that bookkeeping is paid per megabyte. At 64 KiB
a 64 MiB sequential write issued 8311 block-layer write commands for
1024 syscalls, moving 174,328 sectors where the data was 131,072: 1.33x
write amplification, none of it the filesystem's format.

At 256 KiB the same write issues ~2180 commands moving ~142,300
sectors -- 1.086x -- and measured 92.9 MB/s against 54.0. Sequential
read went 102.9 -> 132.6 MB/s.

**Why not larger.** The bounce buffer is the ceiling: AHCI's is 64
contiguous DMA32 frames and already steps down to 16 then 1 if the pool
is fragmented. Beyond 256 KiB the contiguous ask stops being reliably
satisfiable, and the remaining gap is no longer the request size --
the block layer moves 319 MB/s during a sequential read that the caller
sees as 133, so what is left is the two copies and the syscall, not the
command.

**Why it is not a driver constant.** Each driver answers
`blkdev_max_sectors_per_xfer()` for itself and TFS3's run coalescing
asks. AHCI carries the full 256 KiB; legacy ATA cannot exceed 64 KiB
because one PRD's byte count is 16-bit and `ata.c` has no
scatter-gather. A 256 KiB syscall there is four commands in ONE
transaction, which is where most of the win was.


## Why the path cache moved into `resolve()`, and why a VFS inode cache was not built

The obvious fix for "every write syscall re-walks the path" is a VFS
inode cache -- Linux's shape, where the dcache and icache sit above the
filesystem and backends are addressed by inode rather than by path.
That was the plan. It was not built, and the reason is a measurement.

TFS3 already had a full-path cache. It simply lived inside `lookup()`,
which only the READ path called; `tfs3_write_range()` called `resolve()`
directly. Moving the cache down into `resolve()` is a dozen lines, gives
it to every door into the filesystem, and removed the entire
path-resolution cost: after it, a three-component path measures the same
as a root-level one on all four profiles, where before it cost 34% of
random 4 KiB write throughput.

What a VFS inode cache would ADD on top of that is the inode read
itself -- one 512-byte read per operation, about one of the three to
four metadata reads a write syscall still issues. Against that it needs
inode identity in `fs_ops` (every one of ~20 entry points takes a path
today), an opaque per-backend payload the VFS caches, and an
invalidation rule covering delete, rename, truncate, link, chmod, mkfs,
mount and hard links naming one inode by two paths. That is a large,
correctness-critical change to the kernel's most central subsystem for
one sector read.

It stays on the roadmap because the SHAPE is right and the reasons to
want it grow with the system -- a second filesystem with expensive
resolution, or per-inode state worth caching. It is not worth building
for what it buys today, and the honest version of that is a number
rather than a preference.

## A directory inode cache inside tfs3 -- not the VFS inode cache declined above

The entry above declined a VFS inode cache for "one sector read per
operation". A small-file CREATE turned out to cost far more than that,
measured on the Lenovo on 2026-09-28: 29.6 device reads per empty-file
create, 4.08 ms, all of it disk waits under the volume lock. Traced
under QEMU, one `open(O_CREAT|O_TRUNC)` walked its path from the root
FIVE times (`fs_exists`, `parent_dir_err()`'s `fs_is_dir`, `fs_touch`,
`create_entry`'s own resolve, and a truncate of the file it had just
created empty), and the path cache could not help: a create FLUSHED it,
and a name that does not exist yet is never cached anyway, so every walk
re-read the root and parent inodes.

**What was built instead, and why it is not the declined design:**

- **Fewer walks** (`sys_open()`, `tfs3_touch()`): the parent is examined
  only after a create fails, a just-created file is not truncated, and
  touch no longer looks up a name `create_entry` looks up anyway.
- **A create no longer flushes the path caches.** Both hold only names
  that resolved; adding a name changes none of them.
- **A cache of DIRECTORY inodes inside tfs3** (`t3_read_inode()`), eight
  entries per mount. No `fs_ops` change, no inode identity at the VFS,
  no per-backend payload: none of the cost the entry above weighed. It
  is coherent by one rule -- an inode changes only through
  `t3_txn_stage_inode()` -- with one subtlety: only a DEFERRED
  transaction's images are visible to reads, so during an ordinary one
  a read of a just-staged directory sees the OLD disk copy, and the
  transaction forgets every directory it staged again when it ends. The
  first version cleared the whole cache on every reset instead, which
  `txn_commit_raw()` calls on every SUCCESSFUL commit too -- so it was
  coherent only because it was empty; a positive control that stayed
  green is what showed it.

**What it bought:** per empty-file create 10.5 reads and 2.40 ms (from
29.6 and 4.08), per 4 KiB create 18.1 reads and 4.68 ms (from 35.0 and
6.09). **What it did NOT buy, and what did:** a `stat` beside a stream of
creates waited no less -- because one `stat()` was FOUR fs calls, four
lock acquisitions, each queued behind a create's. Making it one
(`fs_stat()` reports type and size too; the separate calls remain only
for the implicit root) halved to thirded the wait on both laptops
(`docs/bugs.md`, the `stat` entry). The two flushes a create's commit
still pays are now half of what is left.

## Why `batched` defers the allocation bitmap too, and why that is safer

`do_write_inner()` flushed the dirty bitmap and group descriptors on
every write, before staging the inode -- the set-before-use order that
keeps a crash costing a LEAK rather than a file pointing at blocks the
bitmap calls free. Under `storage.sync = batched` the inode commit was
already deferred, so that flush was the only per-write metadata write
left, and it was most of them: 1138 block-layer write commands for 256
syscalls of sequential writing.

`txn_flush_deferred()` does it once per batch now, immediately before
the commit, which preserves the ordering exactly. 623 commands, and
write amplification of 1.018x against 1.05x; 130.5 -> 140.9 MB/s.

**The crash argument runs the other way from the intuition.** Deferring
is not a durability trade: a crash mid-batch now leaves the bitmap
saying `free` and the inode unchanged, which is fully consistent and
leaks nothing. Flushing per write left blocks marked used by an inode
update that never landed. The batch's contents are lost either way --
that is what `batched` already promised.

**What made it hard to test is that `fsck` cannot see the bug.**
`tfs3_check()` walks the inode tree against the RAM bitmap, so a batch
whose bitmap never reached the device is invisible until the next mount
re-reads it; removing the flush entirely passed all 36 fs tests. The
check that catches it reads the bitmap block back through the block
layer, which is why it lives in `tfs3.c` -- and it reads the ROOT's
state (`mount_root()->state`) under `fs_exclusive_begin()`, because
nothing else says which volume's bitmap to read.

## One lock per mount, per-mount scratch, and "exclusive" is every mount's lock

**Decided 2026-09-24**, stage 2 of `docs/fslock-design.md`. The
filesystem had one sleeping lock, so a `/tmp` call waited behind a disk
wait on `/` -- ~10 ms a call under `diskbench`, measured with
`tools/fs_isolation.py`. Each mount now has its own (`struct mount`'s
`lock`), and three choices in it had an obvious alternative.

**Scratch is per MOUNT, not per call.** The design doc first planned a
kmalloc'd context per backend call. One call per volume at a time is
still true after this stage, so per mount is enough: no allocation per
syscall, and tfs3's state grows to ~250 KiB (half of it journal, which
Linux also keeps per superblock -- ext4's `s_journal`). Stage 3 drops a
volume's lock mid-operation, and THAT is when anything held across the
gap must become per call -- chosen by what it holds, not all of it now.

**"Everything" is every slot's lock, parent mount first, not a new
rwsem.** Linux has a per-superblock `s_umount` rwsem and
`freeze_super()`; a global reader-writer lock taken shared by every
FS_OP would be the closer copy. It was not built because a recursive
SLEEPING rwsem is a new primitive with its own trap -- a nested reader
behind a queued writer deadlocks, and nested FS_OPs are real here
(`listdir_collect()` stats inside an `fs_list()` callback). Taking
every mount lock needs nothing new, recursion keeps working, and the
callers (partition writes, the legacy `run`, mount-table changes) are
rare enough that its cost is irrelevant. The order is the one calls
already nest in: a path under a mount point resolves to that mount or
a deeper one, so a callback on `/` may take `/boot`'s lock and never
the reverse.

**A generation, not a reference count, keeps an unmount safe.** Linux
pins a `vfsmount` with a refcount and refuses the unmount while it is
held (`EBUSY`). Here a caller resolves a path, then may SLEEP waiting
for the lock, and in that gap the mount can go and a new one take the
slot. Refcounting every resolve would touch every fs call site; a
generation read at resolve and compared under the lock turns that race
into the failure the call would have had a moment later anyway ("no
such path"). The slot's lock is never zeroed for the same reason -- a
caller may be queued on it.

## Inode locks: release everything and restart, rather than a lock order

**Decided 2026-09-24**, stage 4 of `docs/fslock-design.md`. tfs3 has a
per-inode shared/exclusive lock (a per-mount table of held locks), and
an op that finds one busy has to wait for it without holding the volume
lock -- the holder may need that lock to finish.

**What Linux does**: a strict lock order (parent before child, two
directories by address, `s_vfs_rename_mutex` for a cross-directory
rename) so that holding one `i_rwsem` while waiting for the next cannot
deadlock. It needs the order because it holds and waits.

**What toy-os does instead**: never hold and wait. `t3_lock()` either
takes the lock at once or releases every lock the op holds, waits with
the volume lock dropped, and makes FS_OP run the op again from its
lookup. With no hold-and-wait there is no cycle to form, so there is no
order to keep and no rename mutex -- and a new op cannot break a rule
nobody has to remember. The cost is re-running a lookup after a wait,
which is cheap next to the wait itself, and a theoretical livelock that
needs a writer to win every race.

**What makes it workable is that locks are op-scoped.** `op_end` (a new
optional fs_ops slot) releases an op's locks when FS_OP's call returns,
so none of tfs3's ~40 return paths unlocks by hand, and "release all of
this op's" is well defined: every entry carries its pid and the depth
at which it holds the volume lock, so a nested call releases only its
own. A lock is never held across SYSCALLS -- a stepped write locks per
step and detects a change between steps instead.

## Allocating writes keep the volume lock (fslock 4b was built and dropped)

**Decided 2026-09-24.** With per-inode locks in place (stage 4a), the
obvious next step was to let an allocating or extending write drop the
volume lock for its data transfer, as reads and in-place overwrites
already do. It was built: a per-call rollback log, the pointer-table
caches landed before every gap, the data run written unlocked under
the file's exclusive inode lock. A two-appender race test showed it
correct -- and showed the inode lock was what made it so.

**It was dropped because it bought nothing measurable and cost
something that was.** A `stat` on the same volume during diskbench's
file-creating phase averaged ~1.4 ms with or without it (KVM, strict
and batched sync), and on a disk throttled to USB-stick speed it read
worse, 5.8-6.0 ms. The table flush before every gap made 52% more write
commands. The phase was never lock-bound the way the overwrite phase was
(~13 ms before stage 3b): what the probe waits for there is mostly CPU,
since a syscall runs with interrupts off -- the trap gate's question,
not this one.

**Revisit it** only with a probe that shows allocating writes holding
others up -- the one hint was that the measuring tool's own `cat` could
not get through on a throttled disk without it, which a spawn-latency
probe would settle. `docs/fslock-design.md` has the full account.

## A path has THREE bounds, not one: what a call may be handed, what a struct may remember, and what one component may be

**Decided 2026-09-15**, when `FS_PATH_MAX` went from 64 to 4096 so a
deep directory tree could be opened at all. The interesting part is not
the new number; it is that one number could not do the job.

`FS_PATH_MAX` was 64 and every path-shaped thing in the tree was spelled
with it -- a syscall's working buffer, a mount point, an mmap region's
backing file, a `struct dirent`'s name, a process's current directory.
That works exactly as long as the number is small. Raising it exposed
that those uses have nothing in common except the word "path":

- **`FS_PATH_MAX` (4096)** is what a CALL may be handed. It is
  transient, one or two live at a time, and it is the only one that has
  to be generous, because it is what bounds the deepest file a program
  can name. This is Linux's `PATH_MAX`.
- **`FS_PATH_STORED_MAX` (256)** is what a long-lived struct may
  REMEMBER, and it exists because storage gets MULTIPLIED. A per-region
  path at 4096 is 8 MB of kernel `.bss` (64 processes x 32 mmap
  regions), buying deep paths for the one case that least needs them.
  Windows drew this line too: `MAX_PATH` for what an API struct embeds,
  a longer form for what a call may name. A path too long for one of
  these is REFUSED, never truncated.
- **`FS_NAME_MAX` (255)** is one COMPONENT -- what TFS3 already stores
  on disk (`T3_NAME_MAX`) and what ext4 and NTFS both use. **It is the
  one of the three the LISTING ABI does not yet honour**: `struct
  sys_dirent.name` stays 64, so a file can be created with a 200-byte
  name and `readdir()` will not report it.

  That was measured, not assumed. Raising the field to 256 multiplies by
  `SYS_LISTDIR_MAX` in every `opendir()`, taking a `DIR` from ~22 KiB to
  ~72 KiB -- and ring-3 `free()` never returns memory to the OS, so the
  File Manager, which opens directories per navigation and per tick
  reload, only grows: `filemanager_test` went from **1 failure to 16**.
  Cutting `SYS_LISTDIR_MAX` to 64 to pay for it was tried first and was
  worse -- `/bin` (89 entries) and `/tests` (101) both exceed 64, so any
  caller not paging with `SYS_LISTDIR_AT` silently saw a short
  directory. Both were reverted. The real fix is an allocator that
  releases, or a variable-length record -- which is exactly why Linux's
  `struct dirent` has one -- and it is a roadmap item rather than
  something to smuggle into a path change.

**Why not just make everything 4096.** Because two of the three are
array dimensions in structs that exist once per object, and the kernel
has 64 process slots. The measurement that settled it: `sched_cwd` is
one per process (256 KB at 4096 -- affordable, and it keeps `chdir` into
a deep directory working), while `mmap_region` is thirty-two per process
(8 MB -- not affordable, and nothing wanted it).

**Why a query record is bounded by something else again.**
`QUERY_PROCPATH_MAX` is 252, not either of the above, because a query
record must fit `QUERY_RECORD_MAX` (256, `api/query.h`). A process whose
spawn path is longer reports `""` rather than a truncated one, since
both readers MATCH on that string and a nearly-right identity is worse
than an absent one.

## A path buffer is not a kernel local, and `kpath.c` cannot allocate one

A kernel stack is 16 KiB with a single guard page, so two 4096-byte path
locals in one frame is half of it and three step over the guard into
unmapped space. Linux has the same arithmetic and the same answer:
`getname()` takes a path from a slab (`names_cachep`), `putname()`
returns it, and no path is ever a local. `kpath_get()`/`kpath_put()`
(`api/kpath_buf.h`) are that pair, over `kmalloc` rather than a fixed
pool -- a pool has to guess how many paths are live at once, and this
kernel preempts inside a syscall, so the honest number is not one a
constant could name.

**The wrinkle that shaped the API: `kernel/lib/kpath.c` is compiled into
BOTH rings** (the Makefile's shared-source list strips the C library from
its include path), so it can name neither `kmalloc` nor `malloc`. Its
`k_path_normalize()`/`k_path_resolve()` kept a 256-byte local for the
join-before-collapse scratch, which is exactly what could not grow. So
the scratch became the CALLER's, passed as a `struct kpath_scratch` --
the same answer `klineedit.c` gives to the same constraint with
`struct kline_mem`, and for the same reason.

**Three places deliberately did something else**, and the pattern is
worth naming because "allocate it" is not always right:

- **`mount_resolve()` stopped copying at all.** Stripping a mount point
  from a path leaves a SUFFIX, so the backend-relative path is a pointer
  INTO the caller's string. That removed an `FS_PATH_MAX` array from
  `struct resolved` and with it a path local from every `fs_*()` in
  `vfs.c` -- nineteen frames, fixed by deleting a buffer rather than
  pooling one.
- **`tfs3.c` uses one buffer per function, per mount** (`t3_state.pb`).
  One call runs per volume at a time (the mount's lock), which is why
  they can outlive the call, as `blk` and `dirblk` do. One
  buffer per function rather than a shared stack with a depth counter:
  these calls nest and none recurses into itself, so separate buffers
  need no push/pop and cannot leak a slot down an early return.
- **A CONSTRUCTED path keeps a small local.** `/etc/settings.d/<ns>.<name>`,
  `/lib/modules/<name>.ko` and a cursor theme's shape file are bounded by
  their own shape rather than by what a caller may hand in, so they take a
  named constant of their own and stay on the stack.

## The persistent log keeps one file per boot, and a boot that fills its share stops

`logd` kept exactly two files until 2026-09-19: `toyos.log` for the
current boot and `toyos.log.1` for the previous one, half the
`storage.log_max` budget each. That answers "what did it say before I
rebooted it", which is the right question after a machine has been
rebooted once to recover it -- and the wrong one for a fault that
appears on one boot in several. The USB NIC wedge on the bare-metal
ASUS is exactly that shape, and diagnosing it means comparing a bad
boot against the good ones around it, which a window of two cannot do.

**One file per boot, numbered, rather than a deeper rotation chain.**
logrotate's `rotate N` would have been the smaller change -- extend the
existing `.1 -> .2` shift -- but it conflates two things. `logd` also
rotates on SIZE, so under a chain "keep N files" does not mean "keep N
boots": one chatty boot occupies several slots and the count stops
describing anything a person wants. journald's answer is to record a
boot identity and let `journalctl -b -2` resolve it, and that is the
shape taken here, minus the binary store: `/var/log/boot/<n>.log`, with
the counter in `/var/lib/logd.seq` so the number survives the reboot it
is naming. `log -p 3` then means three BOOTS back, always.

**And a boot that fills its share stops rather than rotating within the
boot.** This is where toy-os deliberately differs from journald, which
opens a new file and keeps going. That behaviour lets a single runaway
logger -- the concrete case: a driver polling a device that had gone
away, one line a second -- flush every older boot out of the retention
window. Losing the history to the very fault the history exists to
diagnose is the worst available outcome. So each boot gets
`storage.log_max / (storage.log_keep + 1)`, and one that reaches it
writes a line saying so and stops. The tail of a chatty boot is the
cheaper half to lose: an enumeration fault is in the first few KiB, and
the in-memory ring still has the tail for as long as the machine is up.

**The counter is `fsync`ed** for `netheal`'s reason -- a number still in
the write-back cache when the power goes is a number that never
happened, and the next boot would then file its predecessor's log under
a number already in use, overwriting the log it was meant to keep.

**The defaults are 10 boots and 8 MiB**, raised from 4 MiB: at
`8 MiB / 11` each boot gets about 745 KiB, comfortably more than the
~10 KiB a quiet boot writes and enough for a noisy one. Ten because the
faults worth retention are intermittent.

## Shutdown commits the FILESYSTEM, not just the disk cache

`system_reboot()` and `system_poweroff()` called `atac_flush()` and
nothing else. That reads like enough -- the comment beside it correctly
said a write-back cache can still be holding a write that returned
success -- and it is not, because the sector cache is a layer BELOW the
filesystem. With `storage.sync = batched`, which is the default, TFS3
keeps a journal transaction open across writes; blocks that transaction
still owns have never been handed to the sector cache, so flushing the
cache cannot save them. `fs_sync()` already knew this -- its stage 0
commits each backend's deferred transaction before touching the device,
and its own comment says flushing without committing first "would report
a durability that had not been reached". The power path simply did not
call it.

**What it cost, measured 2026-09-19.** A `config set` followed
immediately by `reboot` left `/etc/storage.conf` at ZERO BYTES and the
setting silently back at its default. Losing the new value is the
obvious half; the destructive half is that `fs_write()` truncates before
it writes, and the truncation DID reach the platter while the data did
not -- so the file's previous contents went too. It was found because a
40-boot soak ran at `storage.log_keep 10` while `config get` answered 50
the whole time, from memory, and pruned the boot logs the soak existed
to collect.

**Why the fix is `fs_sync()` before `atac_flush()` and not instead of
it.** `fs_sync()` already ends with the device flushes, so the second
call is usually redundant -- but it is the one that covers a mount whose
backend has no `sync` hook and anything written outside a filesystem at
all. Both report their own failure, because "some writes were NOT saved"
is worth two different lines when a machine is about to stop.

**The general shape**: a durability barrier belongs at the TOP of the
stack that defers, not the bottom. Linux's `reboot(2)` path runs
`ksys_sync()` -- filesystems first, then block devices -- for the same
reason, and this is that ordering.

## A delete's TRIMs are merged and sent as one list, not deferred

Deleting a 512 MiB file stopped the machine for 105 ms, and 89 ms of that
was TRIM: tfs3 discarded each freed run as it went, one DATA SET
MANAGEMENT command per run, 313 of them at about 0.29 ms each, inside
`FS_OP()` with interrupts off. A USB audio driver with a 20 ms buffer
heard it as a dropout.

**What real systems do.** Linux's ext4 `discard` option collects freed
extents and discards them at journal commit, and libata packs several
ranges into one DSM command. Most distributions ship without that option
and run `fstrim.timer` weekly instead. btrfs has defaulted to
`discard=async` since 6.2, a background queue with rate limits. Windows
sends delete notifications down the storage stack and re-trims weekly
with Optimize Drives.

**What toy-os does.** tfs3 queues the freed runs, merges neighbours (a
pointer table usually sits next to the data it maps, so a sequential
file collapses to a few ranges), and hands the list to
`blkdev_trim_ranges()`. AHCI and legacy ATA pack up to 64 ranges per
command through `blk_dsm_pack()`; a device without the list op gets one
`trim()` per range. The 512 MiB delete went from 105 ms to 21.7 ms.

**Why not deferred, which costs the delete nothing.** A deferred discard
has to be kept away from a block that has been reallocated and
rewritten, which btrfs does by tracking its free-space cache and which
tfs3 has no structure for. The queue here is flushed before the freeing
operation returns and before any allocation, so that case cannot arise.
Periodic `fstrim` remains the way to go further, and is not built.


## Several commands in flight is a synchronous batch, not an asynchronous submit

The roadmap said NCQ needed "an ASYNCHRONOUS block interface first":
submit a request, get a completion later, the shape of Linux's
`submit_bio()` and NT's IRP. What was built instead (2026-09-23) is
`blkdev_submit_batch()`: hand the device several independent transfers,
**wait for all of them**, read each one's answer. That is Linux's
PLUG (`blk_start_plug()`/`blk_finish_plug()`) rather than its bio.

**Why not the obvious async interface.** Nothing here could use one. The
filesystem is a locked, synchronous caller (each mount's lock in `vfs.c`'s `FS_OP`), so
an async submit would need a completion path, a request lifetime and a
caller that does useful work before the completion -- and the only such
caller in the tree is the journal commit, which has a list of blocks and
a barrier after them. A batch gives that caller everything async would,
with no request that outlives a function call. The async interface is
still the right end state for readahead and writeback, and a batch is
what it would be built UNDER (a batch is "submit N, wait N").

**The fallback is the ordinary path.** A device without a queue gets the
same transfers one at a time through the same code a single transfer
uses, so a batch is never a second behaviour to keep correct.

**A failed round is replayed, not diagnosed.** A failed queued command
aborts every outstanding tag, and which one failed is in the drive's NCQ
error log (READ LOG EXT page 10h), which is what Linux reads. Here the
port is recovered and the round's transfers are redone one at a time,
each getting its own answer. That repeats the round's good transfers,
which is safe only because none of them has been acknowledged to anyone
yet -- the batch has not returned.

Measured the same day, and worth knowing before building more on it: the
queue works and buys nothing measurable YET, because tfs3's commits
carry about two target blocks each (`docs/roadmap-details.md`, "Bigger
batches").

## Block-layer LBAs stay 512-byte units on a 4K-sector disk

A device carries its logical block size (`struct block_device.block_size`,
512 or 4096), and **every LBA and count in the block layer stays in
512-byte sectors whatever it says**. A 4K-sector disk is addressed as
eight sectors per block; the driver converts if its protocol needs to,
and the block layer REFUSES a transfer that does not start and end on a
block boundary.

**That is Linux's shape.** `sector_t` and `bio->bi_iter.bi_sector` are
512-byte units on every device, the request queue carries
`logical_block_size`, and a misaligned bio fails. virtio-blk's own wire
protocol does the same -- its sector fields are 512-byte units even when
`blk_size` is 4096 -- so toy-os's virtio driver needed no conversion at
all. Windows reports `BytesPerLogicalSector` and has its filesystems
format to it; that is the other half, and the one FAT32 follows here.

**The obvious alternative was device-native LBAs** (an LBA is one device
block, as in NVMe, ATA and GPT), and it was rejected for what it does to
the ~20 places that read or write ONE SECTOR into a 512-byte buffer --
TFS3's superblock and journal header, every partition-table read, the
FAT32 probe, the swap header. With native LBAs each of those would
silently transfer 4 KiB into 512 bytes on a 4K disk: a stack or heap
overflow per call, found on real hardware. With 512-byte units the same
call is a misaligned transfer the block layer refuses and logs. **The
failure mode is a refusal instead of memory corruption**, which is what
made it safe to convert the consumers one at a time. It also left TFS3's
eight-sectors-per-block arithmetic, about forty sites, untouched.

**What it costs.** The 32-bit sector index still caps a disk at 2 TiB,
where native LBAs would have made that 16 TiB on a 4K disk -- a separate
problem with a separate fix (a 64-bit index). And the table formats that
are defined in device blocks -- GPT's header, entries and geometry, MBR's
LBA fields, FAT32's BPB -- are scaled at the one place each is parsed or
written, so two units meet in `partition.c` and `fat32.c`'s
`parse_bpb()`, and nowhere else.

**Sub-block I/O goes through `blkdev_read_partial()`/`_write_partial()`**,
a bounce over the enclosing blocks. The write is a read-modify-write of
the whole block, so it is only safe for data whose neighbours the caller
holds the lock for -- true of every caller (a mount's metadata under its
lock, a partition table, format-time writes). Linux has no such helper;
its buffer cache reads whole blocks and a filesystem's block size is at
least the device's. TFS3 already had 4 KiB blocks, which is why a helper
for the few sub-block reads was cheaper than changing its unit.

**What is refused rather than converted.** A 4K-logical SATA/IDE drive
(`ata.c`, `ahci.c`: QEMU cannot present one, so a conversion would ship
untested), BIOS boot from a 4K disk (`install` and `SYS_INSTALL_BOOT`:
GRUB's i386-pc boot sector and blocklist are 512-byte formats), and a
FAT32 whose bytes-per-sector is smaller than the device's block (Linux's
vfat rule). The journal header's v2 checksum offset was `ATA_SECTOR_SIZE
- 4`, and is now the literal 508: derived from a runtime sector size it
would have changed the on-disk format on a 4K disk.

## `fs_list()` carries a context pointer, because every caller had invented a global

`fs_list(path, cb, ctx)` hands `ctx` back to `cb` per entry. Until
2026-09-26 the callback took only `(name, size, is_dir)`, so every
caller that needed state across entries kept it in a file-scope global,
and the comments said why that was safe: "syscalls in this kernel are
never reentrant or concurrent", "nothing yields". Both stopped being
true when a disk wait started to sleep -- a walk that finds an inode
busy now waits with the mount lock DROPPED (`mount_wait()`).
`SYS_LISTDIR`'s globals were re-armed by another process's listing in
that gap: init read the desktop's app entries as the contents of
`/etc/services.d` and the desktop's Start menu came back empty, 1 boot
in 10 on `main`, measured with a probe that caught every instance.

**The obvious fix was smaller and wrong for this repo:** move just the
syscall's state somewhere per-process. It fixes the one instance that
was caught and leaves four more in the same shape (`config_file.c`,
`font_faces.c`, `keyboard_config.c`, `setting.c`), each with a comment
explaining the global as a workaround for the missing parameter. The
parameter is what was missing, so it is what was added: Linux's
`iterate_dir()` passes a `struct dir_context *` through the backend to
`filldir` for exactly this reason, and with it no listing needs a
global at all. The cost was mechanical -- three backends thread one
argument through, and each caller's state became a struct -- and
`tests/listrace_test` forces the gap (the old code failed it 2 runs in
3, the new one 0 in 10).

**What it does NOT fix:** anything outside a listing that a syscall
reaches and a lock drop can interleave. `docs/smp-design.md`'s rule --
no module-level buffer in anything a syscall reaches -- is the general
form, and this was one instance of it.


## A replacing rename is a SECOND call, and the default still refuses

**The problem.** `fs_rename()` refused an existing destination, and
TFS3's own comment deferred "an atomic replace" as a separate decision.
An updater needs exactly that -- write `x.upd`, then swap it in with no
instant where `x` is missing -- and so does every portable program that
saves a file the POSIX way (Doom's savegames, mbedTLS's key store call
`rename()` expecting it to replace, and on toy-os it silently failed).

**What real systems do.** POSIX `rename(2)` replaces an existing file
atomically; Linux added `renameat2(RENAME_NOREPLACE)` as the opt-out.
Windows is the reverse: `MoveFileEx` refuses unless given
`MOVEFILE_REPLACE_EXISTING`.

**What toy-os does.** Windows' default with POSIX's libc. The VFS keeps
`fs_rename()` refusing and adds `fs_rename_replace()`; the syscall layer
keeps `SYS_RENAME` refusing and adds `SYS_RENAME2(flags)` with
`RENAME2_REPLACE`; libc's `rename()` asks for the replace, while `mv`
and the File Manager (through `ufileop`) keep `sys_rename()`. The
refusal stays the DEFAULT because the callers that must not clobber are
interactive, and a file manager that silently destroyed a file the user
forgot about is worse than one that asks; the programs that want
replacement say so through the call POSIX gave them.

**Atomic or refused, never emulated.** TFS3 does the swap as one journal
transaction (repoint the destination's dirent, remove the source's,
free the displaced inode after the commit -- clear-after-persist, as
delete does), ramfs under the mount lock; both declare `FS_CAP_REPLACE`.
FAT32 has no journal and returns `-ENOTSUP` instead of doing a
delete-then-rename behind the caller's back: a caller that accepts that
window -- `/bin/update` rotating `/boot`'s kernel to `kernel.old` --
does the two steps itself, in the order that keeps a bootable kernel
reachable. A directory on either side is refused, and so is a
cross-parent replace on a v1 journal (five credits, like a directory
move).

## `fsck` is a `/bin` program, and the check it prints stays in the kernel

Until 2026-10-07 `fsck` was a kernel-shell builtin over `fs_check()`,
which checked the ROOT only. It is `/bin/fsck PATH` now, over
`SYS_FS_CHECK`, and the checker did not move.

**What real systems do.** e2fsck and `fsck.vfat` are ring-3 programs
reading the RAW device of a volume nobody is writing; the root is
checked from the initramfs, or while it is still mounted read-only. XFS's online scrub
and `btrfs scrub` are the other shape: the kernel walks the MOUNTED
filesystem's live structures and repairs them, and a ring-3 tool
(`xfs_scrub`, `btrfs scrub start`) asks for it through an ioctl.

**toy-os follows the second, because the first needs an unmount it
cannot do.** The root is never unmounted, there is no initramfs to
check it from, and TFS3's repair writes through its journal and its
in-memory allocation state -- which a process reading the raw device
would race. So the program moved and the walk stayed: the same split as
`snddrv` and the sound core, the half that faces a person in ring 3 and
the half that owns the structure where the structure is.

**What it bought besides the ring.** A PATH names the volume, as
statfs(2) does, so `/boot` (FAT32) and `/tmp` (ramfs) can be checked for
the first time. A repair the backend cannot do is `-ENOTSUP`, decided by
a capability bit (`FS_CAP_REPAIR`, TFS3 only) rather than by FAT32
logging "reporting only" and returning success. The exit status is
e2fsck's, so a boot-time report can tell "fixed" from "left".

**`rescue fsck` keeps the old builtin**, root only, for a disk whose
`/bin` will not load -- the disk most worth checking.

## A block op is handed its device, and an LBA is 64 bits

`struct block_device`'s ops took no argument naming the device --
`read_sectors(lba, count, buf)` -- on the reasoning (a comment in
`block_part.c`) that a filesystem here holds its device rather than
being handed one per call. The cost of that showed up in every driver
that serves more than one device: `block_part.c` generated eight copies
of all eight ops, one set per partition slot, and `block_nvme.c` four
copies of seven, so the number of partitions and namespaces was a count
of hand-written thunk lines. And every LBA and sector count was 32 bits,
capping a disk at 2 TiB with 512-byte sectors.

**Changed 2026-10-07.** Every op takes `const struct block_device *self`
first, and a driver serving several devices recovers its own state from
it -- a partition slot by `offsetof` from the embedded device, an NVMe
namespace by its index in the array. LBAs, sector counts, `struct
blk_range` and `struct blk_io` are `uint64_t` through the block layer.

**Then the drivers, the table and the filesystems.** AHCI, NVMe and
virtio-blk read their 48/64-bit capacities and address with them (each
had clamped to 2 TiB and said so in the log); the GPT writer and reader
compute in 64 bits, so the backup header lands on the disk's true last
sector; a partition window and a mount's partition range are 64-bit.
TFS3's block numbers stay 32-bit -- 16 TiB in 4 KiB blocks -- but its
sector arithmetic is 64-bit (`T3_SPB` is a 64-bit constant, so every
`block * T3_SPB` is computed wide), and a volume past 16 TiB formats to
16 TiB rather than wrapping. FAT32 counts in 32 bits by format, so a
volume past that is REFUSED by `mkfs`, never formatted short. ATA stays
28-bit and refuses past 2^32 in its adapter (`blk_fits32()`); LBA48 is
its own roadmap item. `tools/bigdisk_test.py` puts a partition past
sector 2^32 on each of the three drivers and checks the image from the
host.

**And "the active device" became "the root device".** The block layer
had kept a second, implicit I/O path -- `blk_read_sectors()` and its
siblings, answering for whichever device was active -- from before
mount points were real. Nothing called it any more: every mount holds
its device and uses `blkdev_*`. It was deleted, and the one fact it
had carried -- which device the root is on -- is `blk_root()` /
`blk_root_disk()`, a name rather than an I/O path. Linux's `ROOT_DEV`
is the same thing, kept for the same reason.

The considered alternative was a `void *priv` field, Linux's
`private_data`. It was not needed: both multi-device drivers already
keep their devices in an array or an enclosing struct, so the pointer
they are handed IS the key.

## The root disk is the boot disk, not the fastest controller's

**The problem.** The root disk was whichever driver registered LAST --
`blk_register()` is last-writer-wins and the boot runs ATA, AHCI, NVMe,
virtio in that order. That encoded a preference between DRIVERS ("virtio
is faster than IDE") as a choice between DISKS, which it is not: a blank
disk on a faster controller took the root, `try_partitions()` found no
table, and a machine with a perfectly good system disk booted into ramfs.
`root=` worked around it only for someone who already knew.

**What real systems do.** None of them picks by controller. Linux needs
`root=` (or an initramfs that finds the root by UUID); systemd-boot
passes `LoaderDevicePartUUID` and `systemd-gpt-auto-generator` looks for
the root on the disk the loader came from; Windows' BCD names the boot
partition by disk signature and offset; FreeBSD's loader hands over the
device it booted from. **The loader knows where it came from, and says
so.**

**What toy-os does.** Three rules in `choose_root_disk()`, in order:

1. `root=<device>` -- unchanged, the person decides.
2. `bootpart=<PARTUUID>`, which grub.cfg passes from `probe --part-uuid
   --set=bootpart $root` (GRUB's prefix partition, the ESP or the MBR
   boot partition). The disk carrying it is the root disk. This is the
   systemd shape, done with the loader toy-os already ships.
3. By content, disks in `ROOT_PRECEDENCE` order: the first with a boot
   partition (BIOS-boot/ESP, or an active MBR slot), then the first with
   any table, then the first. Old grub.cfgs, the CD, and a GRUB whose
   probe failed land here.

**Rule 2 is AUTHORITATIVE, not the start of a search.** If the boot
disk's partitions hold nothing, the boot goes to ramfs with that disk's
first partition active, exactly as a one-disk machine does. Moving on
to the next disk with a filesystem is the friendlier-looking answer and
the dangerous one: `fsformat` formats the active partition, and an
active partition on a different disk than the one the machine booted
from is how the wrong disk gets erased.

**Considered and rejected.** Content search alone (rule 3 without 2):
it fixes the blank-disk case but cannot tell two bootable disks apart
-- an old install on a second drive would win by controller again.
Multiboot2's boot-device tag: it carries a BIOS drive number (0x80),
and nothing in the kernel can map that to a controller. Finding the ESP
whose `kernel.bin` matches the running image: reads FAT on every disk
at boot to answer a question the loader can just state.

**Getting there on a machine already installed.** System Update never
writes the bootloader or grub.cfg -- both are the machine's own, and a
bad write there is the one update a USB stick has to undo -- so a machine
installed before this has neither the `probe` module nor the lines, and
stays on rule 3. `install --bootloader confirm`, run at the machine,
rewrites the core image and EDITS grub.cfg (grubby's shape: add what it
owns, keep everything else, keep a `.bak`), the way `grub-install` is run
by hand on Linux. Either half without the other still boots.

**Two COPIES of one image still share PARTUUIDs** -- `seed_disk.py`
writes random GUIDs when it partitions a blank image, but `cp disk.img`
copies them, exactly as cloning a disk does on Linux. Precedence decides
between such copies; `mkpart_test.regenerate_guids()` is `sgdisk -G` for
a test that needs them distinct.
