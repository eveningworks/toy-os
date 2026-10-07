#ifndef FS_H
#define FS_H

#include <stdint.h>
#include "timer.h" // struct rtc_time, for fs_stat()'s timestamps below
#include "mount_abi.h" // struct fs_check_result, shared with SYS_FS_CHECK

// This is the stable, backend-agnostic filesystem API -- kapi.h's only
// filesystem include, and the only header apps/ or the rest of the
// kernel should ever call into for file/directory access. As of the
// VFS split, it's implemented by kernel/fs/vfs.c, which dispatches
// every call below to whichever `struct fs_ops` backend is active (see
// kernel/include/kernel/fs_ops.h) -- today that's always TFS3, the only
// flat/directory filesystem. Nothing in this header changed shape when
// that split happened, on purpose: adding a future filesystem means
// writing a new backend and pointing vfs.c's fs_init() at it, not
// touching this file or any of its callers.

// A whole path, and a single component, are two different bounds --
// FS_PATH_MAX is Linux's PATH_MAX and FS_NAME_MAX is its NAME_MAX.
// They were one number (64) until paths moved off the kernel stack.
//
// **A BUFFER THIS SIZE MAY NOT BE A LOCAL.** A kernel stack is 16 KiB
// with a single guard page, so two of these in one frame is half of it
// -- which is why Linux allocates a path from a slab (`getname()`) and
// never stacks one. Ring 0 calls kpath_get()/kpath_put()
// (kernel/include/kernel/kpath_buf.h); kpath.c itself is shared-source
// and cannot allocate at all, so its normalize/resolve take the
// caller's scratch.
#define FS_PATH_MAX 4096

// One component. 255 is what TFS3 stores on disk (T3_NAME_MAX), and
// what ext4 and NTFS both use.
#define FS_NAME_MAX 255

// **A PATH THAT IS STORED PER OBJECT, rather than passed and dropped.**
// FS_PATH_MAX bounds what a call may be HANDED; this bounds what a
// long-lived struct may REMEMBER, and the two differ because the second
// gets multiplied. A per-mmap-region path at 4096 is 8 MB of kernel
// .bss (64 processes x 32 regions), which buys nothing -- the deep
// paths worth having are the ones being opened, not the ones being
// recorded. Windows drew the same line: MAX_PATH for what an API
// struct embeds, the longer form for what a call may name.
//
// A path too long for one of these is REFUSED, never truncated -- a
// shortened path names a different file (kpath.h's rule).
#define FS_PATH_STORED_MAX 256

void fs_init(void);

// Every path below is a normalized absolute path: it must start with
// '/' (a bare name like "notes.txt" is silently treated as "/notes.txt"
// for backward compatibility with callers that predate directories --
// see tfs3.c's normalize()), and must not contain "." or ".." components
// or a trailing slash (other than the root "/" itself) -- callers that
// want cwd-relative paths or ".."-style navigation (see the shell's
// `cd`/`pwd`) resolve to a normalized absolute path themselves before
// calling any of these.

// Returns 1 on success, 0 on failure (bad/too-long path, table full,
// parent directory doesn't exist, etc).
int fs_touch(const char *path);
int fs_write(const char *path, const char *data, int append);

// Creates an empty directory. The parent must already exist (or be the
// implicit root "/"). Returns 1 on success, 0 on failure.
int fs_mkdir(const char *path);

// Deletes a file, or a directory -- but only if the directory is empty
// (no direct children). Returns 1 on success, 0 on failure (doesn't
// exist, or a non-empty directory -- no recursive delete).
int fs_delete(const char *path);

// Renames or moves an entry: `oldpath` gets the name `newpath`, which
// may sit in a different directory. Works for files and directories
// alike. Returns 1 on success, 0 on failure.
//
// Three refusals worth knowing, all returning 0 rather than guessing:
//   - `newpath` ALREADY EXISTS. Replacing it is fs_rename_replace(),
//     a separate call so that `mv` and the file manager, which must
//     not silently destroy a file the user forgot about, cannot reach
//     it by accident -- the reverse of Linux, where rename(2) replaces
//     and renameat2(RENAME_NOREPLACE) is the opt-out.
//   - a directory moved INTO ITS OWN SUBTREE (`mv /docs /docs/old`),
//     which would detach the subtree into a cycle nothing references.
//   - either path being the root.
// Renaming something to its own current path succeeds and changes
// nothing.
//
// On a backend with a journal (TFS3) this is ATOMIC: a crash leaves
// exactly one of the two names, never both and never neither. TFS2
// stores whole paths per record, so renaming a DIRECTORY there
// rewrites each descendant's path as its own record write and a crash
// mid-way can leave the subtree half-moved -- reported by that
// backend's own doc comment, and the reason the atomicity promise
// above names the backend.
int fs_rename(const char *oldpath, const char *newpath);

// fs_rename(), except that an existing FILE at `newpath` is replaced --
// POSIX rename(2), and the write-`.new`-then-rename shape an updater
// needs. On a backend with FS_CAP_REPLACE (TFS3) the swap is ONE
// journal transaction: a crash leaves the old file or the new one
// under `newpath`, never neither. Without the cap an existing
// destination is refused, never emulated with delete-then-rename:
// a caller that accepts that window says so by doing it itself.
// A directory on either side of a replace is refused. With nothing at
// `newpath` this is exactly fs_rename().
//
// A PROCESS THAT HAS THE OLD FILE MAPPED IS NOT PROTECTED. An mmap
// region names a PATH, not an inode, so a replaced /lib library feeds
// its not-yet-touched pages from the NEW file. docs/update-design.md
// is why /bin/update stages a library replacement for the next boot.
int fs_rename_replace(const char *oldpath, const char *newpath);

// Sets a file's size exactly, ftruncate(2)-style. Shrinking frees the
// blocks past the new end; growing extends the file with zeros and
// does NOT consume blocks for the new range (both backends read an
// unallocated range as zeros), so growing a file to a gigabyte is a
// metadata-only operation. Returns 1 on success, 0 on failure --
// including `path` naming a directory, which sizes itself.
//
// Setting the size a file already has is a successful no-op.
int fs_truncate(const char *path, uint64_t size);

// Read a small file whole, into memory the CALLER owns. THIS IS THE
// ONLY WHOLE-FILE READ: the one that handed back a pointer into a
// backend's shared staging buffer is gone, because that pointer's
// lifetime could not be stated in a preemptible kernel -- a ring-3
// file read freed it under a kernel-side parse (docs/decisions.md,
// "fs_read_into() reads into the CALLER's buffer"). There is no shared
// buffer anywhere in this path -- fs_size() plus fs_read_range() into
// `buf` -- so there is nothing for a concurrent reader to invalidate.
// A file that may be large is kmalloc'd at fs_size() by its caller
// (the ELF loaders) or streamed with fs_read_range().
//
// Returns the number of bytes read, or 0 for a missing file, a
// directory, a read failure, or a file that does not fit. A file larger
// than `cap` is REFUSED rather than truncated: a half-read config file
// parses as a valid config file with keys silently missing, which is
// the failure this project's parser conventions exist to prevent. When
// it returns non-zero it NUL-terminates at buf[n] (so `cap` must leave
// room for it, and text callers can scan the result as a string).
uint32_t fs_read_into(const char *path, void *buf, uint32_t cap);

// Returns a file's size in bytes without reading any of its data, or 0
// if `path` doesn't exist or names a directory -- the cheap way to
// find out whether a file fits a fs_read_into() buffer, or needs
// fs_read_range() instead.
uint64_t fs_size(const char *path);

// Streaming read for files too large to load whole -- copies up to `len` bytes starting at
// byte `offset` into caller-owned `buf`. Returns the number of bytes
// actually copied: less than `len` at/near end-of-file, 0 at or past
// EOF (or on any error -- this doesn't distinguish the two, same
// "0 means nothing happened" contract the rest of this header uses).
// No file-handle/cursor concept -- every call is a fresh, independent
// range read, same "no open state to leak or get out of sync" spirit
// as the rest of this API; a caller streaming a whole file just calls
// this in a loop with an increasing `offset`.
uint32_t fs_read_range(const char *path, uint64_t offset, void *buf, uint32_t len);

// Flush everything buffered on the way to a platter, on every mounted
// volume: a driver's software write-back cache first, then each
// device's own cache. `*wrote_out` (may be NULL) is sectors moved out
// of a SOFTWARE cache, 0 on a machine with none.
//
// Returns 0 if anything failed, and that is not cosmetic -- it means
// data is still only in RAM, or only in the drive's volatile cache.
int fs_sync(uint32_t *wrote_out);

// Make one FILE durable: commit the backend holding its path, then
// flush the device under it. Scoped to that file's VOLUME rather than
// to the file -- nothing here is held per file, so there is no narrower
// thing to flush. 0 on failure, which means the data is not on the
// platter.
int fs_sync_path(const char *path);

// Idle-time work for any backend that defers something. Called from
// scheduler_idle(); cheap when there is nothing to do.
void fs_idle(void);

// Streaming write -- writes exactly `len` bytes from `buf` starting at
// byte `offset`, extending the file (allocating new blocks as needed)
// if `offset + len` goes past the current end. NOT the same as
// fs_write(path, data, 1)'s append flag -- pass the file's current
// fs_size() as `offset` to append; passing an `offset` past the
// current end zero-fills the gap. Returns 1 on success, 0 on failure
// (bad path, a directory, or the disk ran out of free blocks -- the
// file's size/content on partial failure is whatever blocks were
// successfully allocated and written before the failure, not rolled
// back). Unlike fs_write(), `buf` is treated as raw bytes, not a
// NUL-terminated C string -- this is the call a large binary file
// (e.g. a saved video, per the feature this was built for) actually
// wants, written a chunk at a time rather than needing the whole file
// in memory first.
int fs_write_range(const char *path, uint64_t offset, const void *buf, uint32_t len);

// Steppable write -- Phase 2 of the async-I/O roadmap item (see
// docs/roadmap.md), built on top of the same per-block work
// fs_write_range() does, but split so a caller can advance it one
// block at a time instead of blocking until the whole write finishes.
// No real caller uses this yet (fs_write_range()/fs_write() are
// unchanged, still fully blocking) -- this exists so the next step
// (Phase 3: wm_run() polling one so the GUI stays responsive during a
// save) has something to build on, and is proven standalone today via
// the shell's `steptest [mb]` diagnostic command.
//
// fs_write_range_begin() starts the operation and returns an opaque
// handle, or NULL on any of the same setup failures fs_write_range()
// already reports via a 0 return (bad path, a directory, file-table
// full) -- on NULL, nothing is pending, don't call step(). `buf` must
// stay valid and unchanged by the caller until stepping reaches a
// terminal result.
//
// fs_write_range_step() advances one block's worth of work and
// returns FS_STEP_PENDING (call again), FS_STEP_DONE, or
// FS_STEP_FAILED. On either terminal result the handle is already
// cleaned up internally -- don't call step() again or free anything.
// On FS_STEP_DONE the file's size/modified-time/on-disk directory
// record are already updated, identical to what a successful
// fs_write_range() call leaves behind -- nothing about the file's
// resulting state reveals whether it was written steppably or in one
// blocking call.
void *fs_write_range_begin(const char *path, uint64_t offset, const void *buf, uint32_t len);
enum fs_step_result { FS_STEP_PENDING = 0, FS_STEP_DONE = 1, FS_STEP_FAILED = 2 };
enum fs_step_result fs_write_range_step(void *handle);

// Steppable read -- Phase 4 of the async-I/O roadmap item (see
// docs/roadmap.md), mirroring the steppable write pair above exactly:
// same opaque-handle/begin-then-step shape, same one-block-per-step
// contract, same "handle already cleaned up on any terminal result"
// rule. fs_read_range()/fs_read() are unchanged, still fully blocking.
//
// fs_read_range_begin() starts the operation and returns an opaque
// handle, or NULL on any of the same setup failures fs_read_range()
// silently treats as "0 bytes" (bad path, a directory, `path` doesn't
// exist) -- on NULL, nothing is pending, don't call step(). `buf` must
// stay valid and unchanged by the caller until stepping reaches a
// terminal result. Like fs_read_range(), reading past end-of-file is
// clamped, not an error -- begin() clamps `len` against the file's
// actual size once, up front, exactly like fs_read_range() does.
//
// fs_read_range_step() advances one block's worth of work and returns
// FS_STEP_PENDING (call again), FS_STEP_DONE, or FS_STEP_FAILED. On
// either terminal result the handle is already cleaned up internally --
// don't call step() again or free anything. Unlike the write side,
// step() also takes `out_total`: written on every call (including
// FS_STEP_PENDING, so a caller could show progress), holding the
// number of bytes copied into `buf` so far -- definitive once step()
// returns FS_STEP_DONE, and the same number fs_read_range() itself
// would have returned for the same call. Needed because a read (unlike
// a write) can legitimately finish having copied fewer bytes than
// requested (the EOF clamp above) -- there's no other way for the
// caller to learn that count once the handle is gone. May be NULL if
// the caller doesn't care.
void *fs_read_range_begin(const char *path, uint64_t offset, void *buf, uint32_t len);
enum fs_step_result fs_read_range_step(void *handle, uint32_t *out_total);

// True if `path` names an existing directory, or is "/" (the implicit
// root, which always "exists" without needing its own entry).
int fs_is_dir(const char *path);

// True if `path` names an existing file or directory (or is "/").
int fs_exists(const char *path);

// True if `path` is on a mount that refuses every change (MNT_RDONLY) --
// what a failed mutation asks to answer EROFS rather than guess.
int fs_readonly(const char *path);

// Calls cb(ctx, name, size, is_dir) for every direct child of `dir_path`
// (which must be an existing directory, or "/") -- `name` is just that
// child's own last path component (e.g. "notes.txt", not
// "/docs/notes.txt"), and `size` is meaningless (0) for directories.
// Table order, not sorted. Does nothing (no callback calls) if
// `dir_path` doesn't exist or isn't a directory.
// `ctx` is handed back to `cb` for every entry -- Linux's dir_context.
// A listing's state goes in it, NEVER in a global: the walk can sleep
// in a disk wait with the mount lock dropped, and another caller's
// listing then runs in the gap (it did: init's SYS_LISTDIR delivered
// the desktop's entries to itself).
typedef void (*fs_list_cb)(void *ctx, const char *name, uint32_t size, int is_dir);
void fs_list(const char *dir_path, fs_list_cb cb, void *ctx);

// The canonical per-entry metadata every backend reports, whatever it
// stores on disk.
//
// `ino` is a stable, unique-per-entry identity number: a real inode
// number on a backend that has them (TFS3), a synthetic-but-stable one
// where the format doesn't (TFS2 reports its table slot index, which
// already never moves for the life of the entry) -- the same trick
// Linux's VFS plays for FAT. Callers may compare inos for identity
// within one boot of one filesystem; nothing more is promised.
//
// `created`/`modified` are SECONDS SINCE THE UNIX EPOCH, **UTC** --
// ktime_read() converted through cal_rtc_to_epoch(). They were
// local-derived until the timezone left ring 0 (api/tz.h), which made
// them arithmetic-comparable but not comparable with anything else;
// they are ordinary epochs now, and a display converts in ring 3. A
// backend that stores civil
// time on disk (TFS2's 7-byte RTC fields, format unchanged) converts
// at stat time; a backend that stores epoch natively (TFS3) reports it
// straight through -- see FS_CAP_EPOCH_TIME below. Display formatting
// goes back through tz_epoch_to_rtc(). Set at creation
// (fs_touch()/fs_mkdir()), `modified` bumped on every data-changing
// fs_write(); touching an existing file stays a no-op and does NOT
// bump `modified`.
struct fs_stat_info {
    uint64_t ino;
    uint64_t created;
    uint64_t modified;
    // PERMISSION BITS, in the usual octal shape (0644, 0755). A backend
    // whose format has none reports the default for the type rather
    // than zero: a caller cannot act on "unknown", and every question
    // worth asking -- is it executable, may I write it -- has to have
    // an answer. FS_CAP_MODE says whether the number came off the disk
    // or was supplied.
    uint16_t mode;
    uint16_t nlink;   // hard links; 1 where the format has no count
    // What fs_is_dir() and fs_size() would say, so ONE call answers a
    // stat(): each fs_*() call takes the volume's lock, and three in a
    // row queued behind whatever else was using the volume three times.
    uint8_t  is_dir;
    uint64_t size;    // 0 for a directory, as fs_size() reports it
};

// Fills *out with `path`'s identity + timestamps. Returns 1 on
// success, 0 if `path` doesn't exist. Meaningless (returns 0) for the
// implicit root "/", which has no entry of its own -- same as every
// other per-entry call in this header.
int fs_stat(const char *path, struct fs_stat_info *out);

// Change `path`'s permission bits. Returns 0, or a negative errno:
// -ENOENT for a path that is not there, -ENOTSUP for a filesystem whose
// format has nowhere to keep them (ask FS_CAP_MODE first if you want to
// know before trying).
//
// **ONLY THE LOW TWELVE BITS.** The type is the filesystem's own and is
// not a caller's to set -- a chmod that could turn a file into a
// directory would be a corruption primitive, which is why POSIX's
// chmod takes permissions only and why S_IFMT is masked off here.
int fs_chmod(const char *path, uint16_t mode);

// ---- backend identity & capabilities ----

// Capability bits a filesystem backend declares (struct fs_ops .caps).
// Same rule as display.h's DISPLAY_CAP_*: a capability and its
// optional operation are one fact stated twice -- callers ask the bit,
// and the VFS's probe loop refuses a backend whose bits and function
// pointers disagree (enforcement bites once the first optional op
// exists; see fs_ops.h). Bits describe what the ACTIVE backend's
// format genuinely supports, not what's implemented in the kernel yet.
#define FS_CAP_INODES     (1u << 0) // real on-disk inodes (ino is not synthetic)
#define FS_CAP_HARDLINKS  (1u << 1) // format carries link counts
#define FS_CAP_SYMLINKS   (1u << 2) // format carries symlinks (resolution may still be unimplemented)
#define FS_CAP_EPOCH_TIME (1u << 3) // timestamps stored as epoch natively, not converted at stat time
#define FS_CAP_MODE       (1u << 4) // format stores permission bits (else stat reports a default)
#define FS_CAP_REPLACE    (1u << 5) // a rename can replace an existing file atomically (fs_rename_replace)
#define FS_CAP_REPAIR     (1u << 6) // fs_check()'s repair pass changes the volume (else it only reports)

// The active backend's short name ("tfs3") -- diagnostic, for
// df/fsck/about-style output. Valid after fs_init(); never NULL.
const char *fs_backend_name(void);

// The active backend's FS_CAP_* bits / a single-bit convenience test.
uint32_t fs_capabilities(void);
int fs_has(uint32_t cap);
// fs_has() for the mount that holds `path` rather than the root -- the
// question to ask before an op on /boot or /tmp. 0 for a bad path.
int fs_path_has(const char *path, uint32_t cap);

// Hardlink: a second name for an existing file. Returns 1 on success,
// 0 on failure -- including "the active filesystem has no hardlinks"
// (check fs_has(FS_CAP_HARDLINKS) first for a better error message;
// the `ln` shell command does exactly that). Directories are always
// refused. Both names are the same inode afterwards (fs_stat()'s ino
// agrees), and the data is freed only when the last name is deleted.
int fs_link(const char *existing, const char *newpath);

// A counter bumped by every operation that changes what is ON the
// filesystem -- create, write, mkdir, delete, rename, truncate, link,
// and a reformat. Never decreases; the VALUE is meaningless, only
// whether it differs from one you saved earlier.
//
// This is the cheap half of "watch a directory" on a kernel with no
// inotify. A watcher saves the counter, compares it (one integer, no
// I/O) as often as it likes, and re-reads the directory only when it
// has moved. The alternative is re-listing the directory on a timer,
// which means a real disk read every few seconds forever on a
// completely idle machine -- the thing this exists to avoid.
//
// GLOBAL: a watcher gets woken by changes it does not care about. That
// was meant to cost "one directory read for a false positive", and for
// two readers it was far more -- init re-read all twelve service
// descriptors, ~40 fs calls, whenever anything anywhere changed. A
// reader that wants ONE directory uses fs_generation_of() below.
uint64_t fs_generation(void);

// The same, SCOPED: moves when `path` itself or a direct child of it
// changes (a create, delete, rename, write or truncate there). No
// registry and no lifetime -- a small table indexed by the path's hash,
// so a collision can make an unrelated change look like one (a false
// positive, never a missed change). `path` must be absolute and
// normalized, as every fs_*() caller's is. Never 0.
uint64_t fs_generation_of(const char *path);

// Reformat the disk with the named backend ("tfs3") and
// remount by re-running the probe loop. DESTROYS the current
// filesystem contents -- callers own the confirmation UX (see the
// `fsformat` shell command). Returns 1 on success (new fs mounted), 0
// on unknown name, format failure, or no disk. Physical-shell only by
// convention: it yanks the filesystem out from under any open state.
int fs_format_backend(const char *name);

// Put an empty filesystem of type `fstype` on ONE device -- the
// installer's mkfs, and deliberately not fs_format_backend() above.
// That one reformats the volume this machine is RUNNING FROM and
// unmounts and re-probes the world to do it; this one disturbs nothing
// and REFUSES a device something is mounted from, because an installer
// handed the running root is one being asked to saw off its own branch.
// Returns 1, or 0 for an unknown fstype, a mounted target, or a format
// that failed.
struct block_device;
int fs_format_device(const struct block_device *dev, const char *fstype);

// 1 if the active backend found (or formatted) a usable disk via
// ata.c, so every fs_touch()/fs_write()/fs_mkdir()/fs_delete() above is
// being persisted to it and files survive a reboot; 0 if no disk was
// found, in which case this is exactly the old in-memory-only behavior
// (files vanish on reboot) -- see tfs3.c's init for the
// detection/fallback logic. Purely informational (the `about`
// shell/GUI screens use it to say which mode they're in) -- every
// fs_* call above works the same either way.
int fs_is_persistent(void);

// Fills *out_used_bytes/*out_total_bytes with the active backend's data
// block usage, both already scaled to bytes (not block counts) so
// callers never need to know the backend's own block size. `total`
// is usable data space only -- reserved metadata blocks (superblock,
// journal, record table, bitmap itself) are excluded, same "how much
// can I actually store" framing `df` gives on a real system, not the
// raw disk size. In RAM-only mode (see fs_is_persistent()) this
// reports the much smaller in-memory cap instead of the real disk
// size, since that's the actual ceiling files are hitting. Returns 1
// always -- there's no failure mode, this just reads bitmap state
// that's already resident.
int fs_disk_usage(uint64_t *out_used_bytes, uint64_t *out_total_bytes);

// The same numbers for ONE mount rather than for the root -- what the
// QUERY_FSINFO provider reports per mount, and what `df` prints a row
// of. `m` is a `const struct mount *` from kernel/mount.h, taken as
// void * because api/ headers may not include kernel/ ones.
//
// IT EXISTS SO THE PREEMPTION GUARD STILL APPLIES. Asking a mount's
// backend directly is one line and skips vfs.c's FS_OP() section, which
// is the guard every other path through this header takes -- and the
// backends are not re-entrant, so a caller preempted inside that call
// corrupts the walk. See vfs.c's FS_OP comment for what that looked
// like the first time.
int fs_mount_usage(const void *m, uint64_t *out_used_bytes, uint64_t *out_total_bytes);

// ---- consistency check / repair (`fsck`) ----

// What one pass found and changed is `struct fs_check_result`, in
// abi/mount_abi.h because SYS_FS_CHECK hands it to ring 3.
//
// Walks every in-use record's block tree (direct + indirect), compares
// what's reachable against the free-block bitmap, and fills *out.
//
// `repair` == 0 is a pure read: it touches nothing on disk, so it's
// always safe to run. `repair` != 0 additionally fixes what can be
// fixed WITHOUT guessing:
//   - a leaked block (allocated, unreferenced) is returned to the free
//     bitmap -- the case the truncate/delete ordering deliberately
//     trades for, see docs/decisions.md;
//   - a referenced-but-free block is marked allocated, closing the
//     window where the allocator would hand it to a second file;
//   - an out-of-range pointer is zeroed, turning it into a hole.
// Double-allocated blocks are always REPORTED, never repaired: two
// records genuinely claim the same block, and choosing which one keeps
// it is a data-destroying guess this can't make for you. The pass
// COUNTS them and names no file yet (docs/roadmap.md).
//
// Returns 1 on a completed pass, 0 if the filesystem isn't disk-backed
// (nothing to check -- RAM-only mode has no persistent bitmap).
int fs_check(int repair, struct fs_check_result *out);

// fs_check() on the volume holding `path` rather than the root -- what
// SYS_FS_CHECK runs. 0, or -ENOENT (no mount answers for it), -EROFS (a
// repair on a read-only mount), -ENOTSUP (a repair the backend cannot
// do, FS_CAP_REPAIR), -EIO (the backend could not complete the pass).
int fs_check_at(const char *path, int repair, struct fs_check_result *out);

// Whether a backend call is in flight on the mount answering for
// `path`, and the pid holding it (0 for the kernel context). Per MOUNT:
// each has its own lock. DIAGNOSTIC ONLY -- a caller that branched on
// this would be racing the answer. See vfs.c's FS_OP.
int fs_lock_held_at(const char *path);
int fs_lock_owner_at(const char *path);

#endif
