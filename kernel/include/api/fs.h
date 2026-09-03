#ifndef FS_H
#define FS_H

#include <stdint.h>
#include "timer.h" // struct rtc_time, for fs_stat()'s timestamps below

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

// Raised from 32 to 256, which is an ON-DISK LAYOUT change (the record
// table sits between the journal and the free-block bitmap, so a
// different file count moves FS_BITMAP_START_LBA and everything after
// it) -- hence the FS_DISK_VERSION bump in TFS2 and the one-time
// reformat that comes with it.
//
// 32 was not a comfortable margin any more, it was nearly exhausted:
// the shipped disk.img already used 25 of them (17 /bin binaries plus
// /bin, /etc, /etc/kbs and four /etc files), and /etc/toyos.conf makes
// 26 the moment any setting is saved. The next handful of seeded
// binaries would have hit "table full" -- which surfaces as a bare 0
// return from fs_touch(), not an obvious out-of-slots message.
//
// The cost of 256 is small and bounded: 256 in-memory `struct file`
// entries (~44KB of .bss) and 256 one-sector records on disk, against
// a disk that's gigabytes. Directories consume a slot each too (an
// empty directory is just an entry with no data). Backend-specific
// (TFS2's own limit), but lives here since callers may reasonably want
// to size buffers against it regardless of which backend is active.
#define FS_MAX_FILES 256

// Was FS_NAME_MAX (a single flat name, 32 bytes) before directory
// support -- now holds a full absolute path like "/docs/notes.txt", so
// it needed more room. Per-slot on-disk size doesn't actually change
// (still fits the same 5 sectors -- see FS_RECORD_SECTORS in TFS2).
#define FS_PATH_MAX 64

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
//   - `newpath` ALREADY EXISTS. There is deliberately no atomic
//     replace here (POSIX rename(2) has one); a caller that wants to
//     overwrite deletes the destination first and owns the decision,
//     and the shell's `mv` says so rather than silently destroying a
//     file the user forgot about.
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

// Calls cb(name, size, is_dir) for every direct child of `dir_path`
// (which must be an existing directory, or "/") -- `name` is just that
// child's own last path component (e.g. "notes.txt", not
// "/docs/notes.txt"), and `size` is meaningless (0) for directories.
// Table order, not sorted. Does nothing (no callback calls) if
// `dir_path` doesn't exist or isn't a directory.
void fs_list(const char *dir_path, void (*cb)(const char *name, uint32_t size, int is_dir));

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
// `created`/`modified` are SECONDS SINCE THE UNIX EPOCH -- but derived
// from local civil time (tz.c's rtc_read_local(), converted via
// tz_rtc_to_epoch()), with no zone recorded. That makes timestamps
// arithmetic-comparable, which the old broken-down struct rtc_time
// shape wasn't; it does NOT make them UTC. A backend that stores civil
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
};

// Fills *out with `path`'s identity + timestamps. Returns 1 on
// success, 0 if `path` doesn't exist. Meaningless (returns 0) for the
// implicit root "/", which has no entry of its own -- same as every
// other per-entry call in this header.
int fs_stat(const char *path, struct fs_stat_info *out);

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

// The active backend's short name ("tfs3") -- diagnostic, for
// df/fsck/about-style output. Valid after fs_init(); never NULL.
const char *fs_backend_name(void);

// The active backend's FS_CAP_* bits / a single-bit convenience test.
uint32_t fs_capabilities(void);
int fs_has(uint32_t cap);

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
// Deliberately GLOBAL rather than per-path: a watcher gets woken by
// changes it does not care about, and pays one directory read for a
// false positive. Per-path watches would need a registry, a lifetime
// and an eviction policy, all to save a read that only happens when
// something actually changed anyway.
uint64_t fs_generation(void);

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

// What one pass over the filesystem found, and (if it was a repair
// pass) what it changed. Counts are of BLOCKS unless noted.
struct fs_check_result {
    uint32_t records_used;        // in-use table slots walked
    uint32_t blocks_referenced;   // distinct blocks reachable from those records
    uint32_t leaked;              // marked allocated, referenced by nothing
    uint32_t referenced_but_free; // referenced by a record, marked free
    uint32_t double_allocated;    // referenced from more than one place
    uint32_t out_of_range;        // pointers naming a block outside the usable range
    // Only nonzero on a repair pass -- what was actually changed.
    uint32_t reclaimed;           // leaked blocks returned to the free bitmap
    uint32_t marked_allocated;    // referenced-but-free blocks marked allocated
    uint32_t pointers_cleared;    // out-of-range pointers zeroed
};

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
// it is a data-destroying guess this can't make for you. Sort those out
// by deleting one of the files named in the log.
//
// Returns 1 on a completed pass, 0 if the filesystem isn't disk-backed
// (nothing to check -- RAM-only mode has no persistent bitmap).
int fs_check(int repair, struct fs_check_result *out);

#endif
