# Finer filesystem locking

A staged plan, in the shape `docs/blocking-design.md` and
`docs/smp-design.md` used. It answers the question stage 2 of the
blocking work left open: **once the holder of the filesystem lock can
sleep, every other caller pays one of its operations per call. What
would it take for callers touching DIFFERENT files not to wait on each
other at all?**

**Status: stages 0, 1 and 2 are BUILT (2026-09-23, 2026-09-24).
Stages 3-4 are designed, not built.**

## Why, in one measurement

`tools/latency_under_io.py`, KVM, one host, 2026-09-23. With `ata.c`'s
DMA wait asleep and `diskbench` writing to `/var/tmp`, the compositor
queued **~17 ms for the filesystem lock on every fs call it made** --
the length of one `diskbench` write -- and made dozens a frame: 0.6-1.7 s
frames, against 5-22 ms at HEAD, where nothing could be mid-call while
the WM ran. Taking the WM's config reads off its frame path (pushed
config, `WIN_EV_FSWATCH`/`WIN_EV_SETTING`) removed most of it; what is
left is structural. Any call anywhere -- a Terminal `ls` during a copy,
an app saving a file -- still waits one holder operation, because there
is one lock and it covers every inode on every mount.

## What real systems do

- **Linux** has no filesystem-wide lock. A per-inode `i_rwsem` (shared
  for readers, exclusive for a write or a directory change), a
  per-superblock rename mutex for cross-directory renames, parents
  locked in a fixed order, and jbd2 HANDLES: an operation reserves
  journal credits, stages its blocks under a short lock, and the commit
  waits until no handle is open. ext2 ran under the Big Kernel Lock
  until 2.4, and the BKL itself took until 2.6.39 to go.
- **NTFS** locks per file: every FCB carries two ERESOURCEs (the main
  and the paging resource), shared or exclusive, and the log is its own
  object with its own lock.

**toy-os should follow the shape** -- per-object locks, a journal with
handles, a fixed lock order -- and deliberately NOT the size: no
separate paging resource (there is no page cache, `docs/pagecache-
design.md`), and no lock-free lookup (RCU walk is an SMP answer to an
SMP problem this kernel does not have yet).

## What the global lock is protecting today

Measured against `tfs3.c` rather than assumed:

- **Per-operation scratch**, which could become per-call: `g_blk` and
  `g_ptr_blk` (4 KiB each, ~95 uses), `g_norm_scratch`, **17
  function-local `static char norm[T3_PATH_BUF]`** (4 KiB each since
  paths grew to 4096), `dirblk`, the read-side pointer cache `g_rcache`,
  the write-walk cache `g_mcache`, the allocation rollback log `g_alog`,
  the TRIM queue. The kernel stack is 16 KiB, so a per-op context has
  to be kmalloc'd or per-thread, never a stack local.
- **Genuinely shared state**: the live-mount pointer `S` (gone in
  stage 1); ONE journal
  for the whole kernel (`g_txn_img[32]`, 128 KiB, plus `g_txn_owner`,
  `g_txn_deferred` and friends); per mount, the bitmaps, the rotor, the
  name caches `ncache`/`lcache` (flushed wholesale on any namespace
  change) and the one-deep leaf cache `pcache`.
- **There is no in-memory inode**: every op re-reads it (`read_inode`),
  so there is nothing yet for a per-inode lock to live in.
- fat32 has the same shape at a smaller size (`g_dirsec`, `g_datasec`,
  `g_tmpsec`); ramfs has no module scratch.

## The stages

Each ships on its own, and each is measured on
`latency_under_io.py` against the one before.

- **Stage 0 -- stop the lock being bypassed. BUILT 2026-09-23.** The
  mount paths (`mount_add`, `mount_remove`, the probe's
  `mount_scratch_begin`) repointed `S` with no lock, so a probe during a
  sleeping FS_OP resumed the sleeper on another volume's state; they
  hold it now, and `FS_OP()` re-checks the mount once it has the lock,
  because an unmount can complete while a caller sleeps waiting for it.
  AHCI got a driver lock (one slot, one bounce buffer), and virtio-blk
  WAITS for its turn where a busy flag used to fail the request.
- **Stage 1 -- mount state passed, not global. BUILT 2026-09-24.**
  Every `fs_ops` op takes `void *st`; inside a backend it is `sbi`, and
  `S`, `state_activate` and `mount_enter()`/`mount_leave()` are gone.
  One piece was not mechanical: activating a different tfs3 mount used
  to commit the other mount's deferred transaction, which survives as
  `t3_enter()` at the top of every tfs3 op -- and goes in stage 2 with
  the journal. `latency_under_io.py` (KVM, `+invtsc`, three alternating
  runs a side): loaded compositor wake 6.5-7.3 ms before, 6.6-7.5 ms
  after -- no change, which is what a stage that moves no lock should
  show.
- **Stage 2 -- per-op scratch, and the journal moves into the mount.**
  After this, **one lock PER MOUNT** is honest -- a `/boot` read stops
  waiting behind `/` -- which `vfs.c`'s comment has always said is the
  first thing that would earn a finer lock.
  - **2a, BUILT 2026-09-24: the state moves.** The journal, the pointer
    caches, the TRIM queue, the rollback log and every per-call buffer
    are in `struct t3_state` (~250 KiB, half journal); fat32's sector
    buffers in its state. PER MOUNT rather than the per-call context
    first planned here: one call per volume at a time is still true,
    so per mount is enough, costs no allocation per call, and leaves
    stage 3 to move to per-call only what it holds across a dropped
    lock. `t3_enter()` went with the owner. It also fixed a live bug:
    the file-scope read-side pointer cache served one TFS3 volume's
    table for another's block (`tfs3_test.c` goes red on the old code).
  - **2b, BUILT 2026-09-24: one lock per mount.** `struct mount`
    carries a recursive `kmutex` and a generation; `FS_OP` takes the
    mount's lock and fails the call if the slot's generation moved
    while it waited (an unmount, or an unmount and a new mount in the
    same slot). A slot is emptied without zeroing its lock.
    `fs_exclusive_begin()` takes every slot's lock, parent mount before
    child -- the order `fs_list()` callbacks nest in -- behind a small
    `g_excl` so two exclusive holders cannot order against a changing
    table. Every mount-table change runs under it. `vfs.c`'s step table
    got its own lock; the one fs lock had covered it by accident.
    MEASURED with `tools/fs_isolation.py` (KVM, `+invtsc`, three
    alternating runs a side against stage 1, 52f39e9e): a `stat` on
    `/tmp` once a tick while `diskbench` loads `/var/tmp` averaged
    9.6-10.7 ms at stage 1 (p99 18.5-20 ms) and 69-77 us here (p99
    271-280 us); alone, 99-119 us against 12 us, because stage 1's one
    lock also queued it behind the desktop's own work on `/`. What
    remains under load is CPU, not the lock: a syscall runs with
    interrupts off. `latency_under_io.py` sees none of this -- the
    compositor's reads are on `/`, which is stage 3-4's problem.
- **Stage 3a, BUILT 2026-09-24: a data READ drops the volume's lock.**
  A whole-block run is read straight into the caller's buffer with the
  mount's lock released (`mount_io_begin()`/`_end()`, tfs3's
  `vol_read_run()`), Linux's direct-I/O shape. What makes that safe is
  a per-mount count of open gaps that three things wait on before they
  proceed (`mount_io_drain()`, Linux's `inode_dio_wait()` but per mount):
  a block FREE, `fs_exclusive_begin()`, and so unmount. A reader stops
  short if any block was freed during its gap (`free_gen`), since its
  inode copy may be stale. KTEST: the device sees the read unlocked,
  and never under exclusion; `fsrace_test` races a reader against a
  truncate-and-reuse and checks every byte. **The race is not reachable
  today even without the drain** -- the reader holds the ATA driver's
  lock for its transfer, so the reuse write queues behind it -- which is
  `heaprace_test`'s lesson again: correct by inspection, a fixture for
  when a driver queues commands. MEASURED (fs_isolation `/etc`, KVM,
  three runs a side): 9.0-10.0 ms under load against 10.4-11.2 ms --
  small, because what that probe waits behind is diskbench's WRITES.
  One 3a run had a single 242 ms stall (HEAD's worst: 34 ms), not
  explained. 3b is the write side.
- **Stage 3 -- inside one volume.** A journal lock with jbd2-style
  handles, an allocator lock over the bitmaps and the TRIM queue, and a
  name-cache lock. **The cheaper intermediate worth measuring first:**
  release the volume lock across the data-block I/O in `do_write_inner`
  and `read_range_impl` -- that is where the time goes, and it may
  capture most of the latency before stage 4 exists.
- **Stage 4 -- an inode table with a per-inode rwsem.** Readers take it
  shared; a directory operation locks the parent; a cross-directory
  rename takes a per-mount rename mutex and then both parents in inode
  order, and re-checks after locking. **This is the stage that fixes
  the measured case** -- `/etc` against `/var/tmp` is one volume.
  `t3_write_step` keeps an inode copy across calls, so a stepped write
  either holds its inode lock across steps or re-reads.

## The honest risk

**The batched transaction assumes one thing runs inside tfs3 at a
time**, three ways: reads are served from the staged journal image
(`vol_read_sectors`), any `txn_begin()` commits somebody else's pending
work, and a directory growing commits mid-operation (`dirent_insert`).
Getting any of those wrong loses writes that already reported success,
with nothing logged. And every automated test here runs TCG, which
serialises, so a missing lock passes the suite. The evidence has to be
a KVM soak with a positive control that removes one lock and shows the
soak failing -- `docs/smp-design.md`'s testing section is the shape.
`tools/kvm_soak.py` is that soak, and its sensitivity is MEASURED: with
the mount lock removed entirely it went red in 2 rounds of 6. Run it
`-n 10` for a stage here, and run the control the same way.
