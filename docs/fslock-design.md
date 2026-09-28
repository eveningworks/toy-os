# Finer filesystem locking

A staged plan, in the shape `docs/blocking-design.md` and
`docs/smp-design.md` used. It answers the question stage 2 of the
blocking work left open: **once the holder of the filesystem lock can
sleep, every other caller pays one of its operations per call. What
would it take for callers touching DIFFERENT files not to wait on each
other at all?**

**Status (2026-09-24): stages 0-2, 3a, 3b and 4a are BUILT; 4b was
built, measured and DROPPED. What is left is stage 5 -- journal commits
off the volume lock -- designed, not built. Each entry below says what
it did and what it measured.**

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
  nine runs a side against 3c06673a): **no measurable difference** --
  8.9-11.9 ms under load against 9.2-10.9 ms. A three-run comparison
  first read as a small gain; more runs put it inside the noise. The
  probe waits behind diskbench's WRITES, which is 3b. A single stall
  of 120-240 ms appears on BOTH builds (docs/bugs.md).
- **Stage 3b, BUILT 2026-09-24: an OVERWRITE drops it too -- and only
  an overwrite.** A write whose whole range is blocks the file already
  has, inside its size, rewrites them with the lock dropped
  (`do_overwrite()`, `vol_write_run()`). That is ext4's direct-I/O
  overwrite, which it runs under a SHARED inode lock for the same
  reason: it allocates nothing and changes no pointer. Allocating or
  extending writes stay locked, because dropping the lock under them
  loses updates -- a write holds an inode COPY from lookup to commit,
  and two writers of one file would each commit theirs; that needs the
  per-inode lock of stage 4. What the overwrite may NOT do is commit
  its copy: it re-reads the inode and changes only the time. After a
  gap, an inode freed on the volume fails the write (the number may be
  another file now); a block freed re-reads the inode and re-checks the
  range. `fsrace_test`'s phase 2 appends to a file one thread keeps
  overwriting; its control (commit the stale copy) LOST 2034 of 2048
  records, and it passes with the code. MEASURED with fs_isolation
  `--during RND4K-write` (KVM, four runs a side against 275f5a1a):
  `stat /etc` while diskbench overwrites went from 12.4-13.1 ms to
  53-83 us, p99 >=20 ms to 0.6-0.8 ms, and the probe got all 400 of
  its calls in rather than ~200. Measured over diskbench's first phase
  instead -- SEQ-write, which creates its file and so allocates -- it
  showed nothing, which is what the design predicts and how the first
  measurement read it. **Throughput checked on the ASUS, and it caught a
  regression the KVM runs could not:** the first version dropped the
  read-side pointer cache on every overwrite, as every write does, and
  since the overwrite finds its blocks through that cache, each 4 KiB
  random overwrite re-read a table -- 5.3-5.7k IOPS against 3a's 8.0-8.4k
  on the same machine. An overwrite's targets are live data blocks, so
  it keeps the cache (a freed table can only become data through an
  allocating write, which still drops it): 7.5-8.1k, within the noise.
  It also skips re-reading its inode when nothing was staged meanwhile
  (`ino_gen`).
- **Stage 3c, BUILT 2026-09-28: a delete reads its pointer tables with
  the volume's lock dropped.** A whole-file free (`free_all_blocks()`,
  delete and truncate-to-zero) read every table under the lock: a
  512 MiB file is ~130 disk commands, and a `stat` on `/` running across
  the `rm` waited 27.5-29.9 ms at worst on the Lenovo and 38.3-41.1 ms on
  the ASUS, against 9.9-18.0 ms quiet -- growing with the file, so the
  reads and not the commit. Safe for the reason 3a is: the free runs
  after the commit, with the file's inode locked and its blocks still
  allocated, so nobody writes a table in a gap. **The TRIMs it has
  queued cross the gaps**, keeping a big delete one discard list, which
  is safe only because both allocators flush the queue before handing
  out a block (flushing before every gap instead broke the one-list
  property, which a KTEST asserts). After, 3 runs each: **7.2-10.9 ms on
  the Lenovo** (inside its quiet 5.0-16.4) and **17.3-17.4 ms on the
  ASUS** (quiet 10.1-13.7). What the ASUS still holds is not
  established; what remains under the lock is the frees, the one final
  TRIM and the bitmap writes. ext4 and XFS hold only the inode's and the
  allocation group's locks across a truncate, never a volume's.
- **Stage 4a, BUILT 2026-09-24: per-inode locks, and their rules.**
  A per-mount table of HELD locks (a futex-hash shape, not an inode
  cache): readers take a file SHARED, writers and namespace changes
  EXCLUSIVE, the parent directory too for create/delete/link/rename.
  Locks are op-scoped -- `FS_OP` releases an op's through the new
  `op_end` slot -- so no op unlocks by hand. **No op waits holding a
  lock**: a busy one makes it release all of its own, wait with the
  volume lock dropped (`mount_wait()`), and be re-run by FS_OP from its
  lookup. That removes deadlock by construction, so the lock order and
  the rename mutex this entry planned are not built: Linux needs them
  because it holds and waits. `fs_exclusive_begin()` waits until no
  OTHER op holds one. A read nested in an fs_list() callback cannot
  wait and proceeds unlocked, which readers already tolerate. It also
  fixed two faults in the stepped write that predate it: its pointer
  cache was left dirty across steps, where another stream's begin()
  dropped it (blocks read back as holes), and it committed a stale inode
  copy over a write made between its steps -- it now FAILS instead.
  Three KTESTs, each red under its control. No throughput change (ASUS
  random writes 7.8k IOPS, as 3b).
  The planned lock ORDER (parents by inode number) and per-mount
  RENAME MUTEX were deliberately not built: no op holds a lock while
  waiting, so neither has a deadlock to prevent (docs/decisions/
  storage.md, "Inode locks: release everything and restart").
- **Stage 4b, BUILT AND DROPPED 2026-09-24: allocating writes drop the
  volume lock.** Under 4a's exclusive inode lock, `do_write_inner()`
  dropped the volume lock for each whole-block data run -- which needed
  a per-call rollback log (the mount's could be reset by another op in
  the gap) and the pointer-table caches landed before every gap. It was
  CORRECT: a two-appender race in `fsrace_test` lost about 6% of
  its records (116 of 2048) with the inode lock disabled and none with it, in QEMU
  and on the ASUS. It WON NOTHING MEASURABLE: `stat /etc` during
  diskbench's SEQ-write averaged ~1.4 ms on 4a and 1.2-2.0 ms on 4b
  (KVM, strict and batched sync), and on a disk throttled to 15 MB/s
  4b's probe read 5.8-6.0 ms. And it COST: 52% more write commands
  (8929 vs 5873 for 256 MiB; 5% more sectors) for the table flushes.
  SEQ-write was simply not lock-bound the way the overwrite phase was
  (~13 ms before 3b). Dropped by the rule it was measured under. One
  thing it may have been doing unmeasured: on the throttled disk,
  fs_isolation's own console polls (a `cat` spawned from `/`) could not
  get through on 4a during SEQ-write and could on 4b. A spawn-latency
  probe would settle it; that is a reason to measure again before
  rebuilding it, not a claim that it helps.
- **Stage 5 -- journal commits off the volume lock.** What still holds
  the volume lock longest is a metadata commit: journal images, targets
  and two device-cache flushes (25-225 ms per flush measured on the ASUS
  under load, ata.c). Measured 2026-09-28 on both laptops (`docs/bugs.md`,
  the `stat` stall): a `stat` on `/` during `diskbench` waits 146-181 ms
  at worst under `storage.sync = strict`, a commit per write, and 8-75 ms
  under the default `batched`, which defers it -- so under the default
  what this stage buys is those tails. It does NOT touch the ~4 ms
  average a small-file create holds the lock for, in either mode.
  jbd2's shape: an op reserves credits and stages into the RUNNING
  transaction under a short journal lock, and a commit -- including its
  flushes -- runs while other ops stage into the next one. The batched
  transaction (`storage.sync = batched`) is the seed of it. This is the
  biggest remaining source of long waits on one volume, and the riskiest
  change in this plan (the honest risk below is about exactly this).

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
