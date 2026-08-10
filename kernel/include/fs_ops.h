#ifndef FS_OPS_H
#define FS_OPS_H

#include <stdint.h>

// The VFS backend interface -- fs.h's public fs_* API (kapi.h's stable
// surface, unchanged by any of this) is implemented by vfs.c as a thin
// dispatch layer over exactly one of these, chosen once in fs_init().
// Adding a second on-disk filesystem in the future means writing a new
// `struct fs_ops` (see tfs.c/tfs.h for the reference implementation)
// and pointing vfs.c's fs_init() at it -- fs.h, kapi.h, and every
// existing caller (apps/, syscall.c, tz.c, font_config.c, ...) need no
// changes at all, because they only ever call the fs_* wrappers, never
// a backend directly.
//
// Deliberately NOT a mount-point scheme (multiple backends active at
// once, routed by path prefix) -- there's exactly one active backend
// at a time, decided in fs_init(). That's the whole scope: this solves
// "swap which filesystem toy-os uses" (or add a new one to choose
// from), not "use two filesystems simultaneously" -- nothing needs the
// latter yet, and it's meaningfully more code (cross-mount path
// resolution, conflicts at mount boundaries) for a capability that
// would sit unused. If that ever changes, this struct is the same
// building block a mount table would dispatch through per-mount --
// it doesn't need to change shape, just get looked up differently.
//
// Every function pointer here mirrors a fs.h entry point exactly (same
// signature, same normalized-absolute-path contract, same return-value
// meaning) -- see fs.h's top comment for what callers can assume about
// paths. A backend only has to implement path *handling*, not path
// *normalization*; vfs.c does not renormalize before calling into a
// backend, so each backend is expected to normalize itself exactly the
// way tfs.c's normalize()/path_is_normalized() do (that logic is
// backend-internal, not shared, since a future filesystem might have
// different path rules -- e.g. case-insensitivity, a different max
// length).
struct fs_ops {
    const char *name; // short identifier, e.g. "tfs" -- diagnostic only

    // Called once, from fs_init(). Returns 1 if this backend found (or
    // formatted) real persistent storage and every mutating call below
    // is being written through to it, 0 if it's falling back to
    // RAM-only behavior (files vanish on reboot) -- same meaning as
    // fs_is_persistent()'s doc comment in fs.h, just per-backend here.
    int (*init)(void);

    int (*touch)(const char *path);
    int (*write)(const char *path, const char *data, int append);
    int (*mkdir)(const char *path);
    int (*del)(const char *path); // backs fs_delete() -- named del, not delete, to read fine if this header is ever pulled into a C++ tool
    const char *(*read)(const char *path, uint32_t *out_size);
    int (*is_dir)(const char *path);
    int (*exists)(const char *path);
    void (*list)(const char *dir_path, void (*cb)(const char *name, uint32_t size, int is_dir));
};

#endif
