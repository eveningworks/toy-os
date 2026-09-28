# kernel/fs/ -- the filesystem's locking and read rules

Loaded when working under `kernel/fs/`. Moved from the root CLAUDE.md,
which keeps the rules that bite callers elsewhere.

## The filesystem

- **A listing's state goes in `fs_list()`'s `ctx`, never a global** -- the
  walk can wait with the mount lock dropped, and another listing runs in
  the gap (`docs/conventions/storage.md`).

- **THE FILESYSTEM IS NOT RE-ENTRANT, and each MOUNT has ONE SLEEPING
  LOCK because of it.** A backend walks through per-mount scratch, so
  `FS_OP()` takes that mount's `lock` (a recursive `kmutex` in `struct
  mount`) around every backend call -- and its holder may SLEEP in a
  disk wait. So **never take it, or any kmutex, with the preemption
  guard raised or with interrupts off and no scheduler slot**: behind a
  sleeping holder that spins forever. `kmutex_lock()` logs `taken from
  atomic context` on entry when it happens. **The lock order is PARENT
  MOUNT BEFORE CHILD** (an `fs_list()` callback on `/` may stat `/boot`,
  never the reverse). A stretch longer than one fs call that must keep
  the disk quiet uses `fs_exclusive_begin()`/`_end()` (EVERY mount's
  lock, in that order), never the preemption guard -- and anything that
  reaches the DEVICE for a mount (a flush) does so under that mount's
  lock, or exclusion stops promising a quiet disk. The exceptions are
  a data read, an in-place OVERWRITE and a deleted file's pointer-table
  reads (`free_all_blocks()`), which DROP the lock through
  `mount_io_begin()` -- a block free or exclusion waits for them
  (`mount_io_drain()`), and an allocating write must never drop it.
  **Every block allocator must flush the TRIM queue before handing out
  a block**: a delete's queued discards cross its gaps on that promise.
  Inside tfs3, an op locks every inode it touches BEFORE changing
  anything (`t3_lock()`; a 0 means return now, FS_OP re-runs it), never
  unlocks by hand, and never waits holding a lock. It does NOT make an
  `fs_list()` callback safe to call `fs_*` on the same mount (that is
  recursion). Finer locking inside a volume: `docs/fslock-design.md`.
- **THERE IS NO `fs_read()`. A whole-file read goes into memory the
  caller owns: `fs_read_into(path, buf, cap)`**, which REFUSES an
  oversized file rather than truncating; a file that may be large is
  `kmalloc`'d at `fs_size()` by its caller or streamed with
  `fs_read_range()`. The same shape exists for config files:
  `etc_config_load()` + `etc_config_buf_get()` read once and answer many
  keys, because `etc_config_get()` re-reads the whole file PER KEY.
