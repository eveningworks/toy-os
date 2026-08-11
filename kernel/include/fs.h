#ifndef FS_H
#define FS_H

#include <stdint.h>
#include "timer.h" // struct rtc_time, for fs_stat()'s timestamps below

// This is the stable, backend-agnostic filesystem API -- kapi.h's only
// filesystem include, and the only header apps/ or the rest of the
// kernel should ever call into for file/directory access. As of the
// VFS split, it's implemented by kernel/drivers/vfs.c, which dispatches
// every call below to whichever `struct fs_ops` backend is active (see
// kernel/include/fs_ops.h) -- today that's always tfs.c, the original
// flat/directory filesystem. Nothing in this header changed shape when
// that split happened, on purpose: adding a future filesystem means
// writing a new backend and pointing vfs.c's fs_init() at it, not
// touching this file or any of its callers.

// Doubled from the original flat filesystem's 16 -- directories now
// consume a slot each too (an empty directory is just an entry with no
// data), and disk.img has ample room either way (see tfs.c's top
// comment for the math). Backend-specific (tfs.c's own limit), but
// lives here since callers may reasonably want to size buffers against
// it regardless of which backend is active.
#define FS_MAX_FILES 32

// Was FS_NAME_MAX (a single flat name, 32 bytes) before directory
// support -- now holds a full absolute path like "/docs/notes.txt", so
// it needed more room. Per-slot on-disk size doesn't actually change
// (still fits the same 5 sectors -- see FS_RECORD_SECTORS in tfs.c).
#define FS_PATH_MAX 64

// Vestigial: this was a real hard per-file ceiling under TFS2 v1
// (inline data, one fixed-size slot). TFS2 v2's block-addressed on-disk
// format (12 direct + single/double/triple indirect blocks -- see
// docs/tfs2-spec.md) removed that ceiling entirely; fs_write()/
// fs_read() now go through the same storage fs_write_range()/
// fs_read_range() do, limited only by available RAM (for the
// whole-file fs_read()/fs_write() calls) or disk free space. Kept
// defined since nothing currently uses it, but not referenced by
// tfs.c at all anymore -- don't treat it as a real limit when sizing a
// new buffer against it (see kernel/core/etc_config.c and
// apps/editor.c/.h for callers that used to and were corrected).
#define FS_DATA_MAX 2048

void fs_init(void);

// Every path below is a normalized absolute path: it must start with
// '/' (a bare name like "notes.txt" is silently treated as "/notes.txt"
// for backward compatibility with callers that predate directories --
// see tfs.c's normalize()), and must not contain "." or ".." components
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

// Returns pointer to file data (not null-terminated beyond size) and sets
// *out_size, or NULL if not found or if `path` names a directory.
//
// This loads the WHOLE file into one heap-allocated (kmalloc, see
// heap.h) buffer and hands back a pointer into it -- fine for the
// small config/text files every existing caller (Notepad, the shell's
// cat/write, editor.c) actually uses, but it cannot work at all for a
// file that doesn't fit in available RAM (this kernel has no virtual
// memory/swap): a multi-GB file will simply fail here (returns NULL)
// once the allocation itself fails, no matter how big TFS2's on-disk
// format can go. For anything that might be large, check fs_size()
// first and use fs_read_range() to read it in bounded chunks instead
// -- see those two below. The returned pointer is only valid until the
// next fs_read()/fs_write() call (the backend reuses one staging
// buffer rather than leaking a fresh allocation every call -- see
// tfs.c).
const char *fs_read(const char *path, uint32_t *out_size);

// Returns a file's size in bytes without reading any of its data, or 0
// if `path` doesn't exist or names a directory -- the cheap way to
// find out whether a file is small enough to fs_read() whole, or large
// enough that it needs fs_read_range() instead.
uint64_t fs_size(const char *path);

// Streaming read for files too large to load whole (see fs_read()'s
// updated doc comment above) -- copies up to `len` bytes starting at
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

// `created`/`modified` are broken-down local time (via tz.c's
// rtc_read_local(), same source SYS_GETTIME uses -- see timer.h),
// not a Unix epoch integer: this kernel has never needed a
// civil-date<->epoch conversion for anything else, so storing the
// same struct rtc_time everything else already uses avoids adding
// one just for this. A host-side tool reading these off an on-disk
// TFS2 image converts them itself if it wants epoch seconds -- see
// docs/tfs2-spec.md. Set once at creation (fs_touch()/fs_mkdir()) and
// bumped on every fs_write() that actually changes a file's data;
// touching an already-existing file is a no-op today (matches
// tfs_touch()'s existing behavior) and does NOT bump `modified`.
struct fs_timestamps {
    struct rtc_time created;
    struct rtc_time modified;
};

// Fills *out with `path`'s created/modified timestamps. Returns 1 on
// success, 0 if `path` doesn't exist. Meaningless (returns 0) for the
// implicit root "/", which has no entry of its own -- same as every
// other per-entry call in this header.
int fs_stat(const char *path, struct fs_timestamps *out);

// 1 if the active backend found (or formatted) a usable disk via
// ata.c, so every fs_touch()/fs_write()/fs_mkdir()/fs_delete() above is
// being persisted to it and files survive a reboot; 0 if no disk was
// found, in which case this is exactly the old in-memory-only behavior
// (files vanish on reboot) -- see tfs.c's tfs_init() for the
// detection/fallback logic. Purely informational (the `about`
// shell/GUI screens use it to say which mode they're in) -- every
// fs_* call above works the same either way.
int fs_is_persistent(void);

#endif
