#ifndef FS_H
#define FS_H

#include <stdint.h>

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
const char *fs_read(const char *path, uint32_t *out_size);

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
