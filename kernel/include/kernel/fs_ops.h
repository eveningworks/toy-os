#ifndef FS_OPS_H
#define FS_OPS_H

#include <stdint.h>
#include "fs.h" // struct fs_timestamps, for .stat below

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
    uint64_t (*size)(const char *path);
    uint32_t (*read_range)(const char *path, uint64_t offset, void *buf, uint32_t len);
    int (*write_range)(const char *path, uint64_t offset, const void *buf, uint32_t len);

    // Steppable write -- Phase 2 of the async-I/O roadmap item (see
    // docs/roadmap.md). Same effect as write_range above, but split so
    // a caller can advance it one block at a time instead of blocking
    // to completion in one call. `void *` here (not fs.h's own opaque
    // handle type) since this header is backend-agnostic and doesn't
    // need to know the handle's real shape any more than every other
    // function pointer here does -- see fs.h's fs_write_range_begin()/
    // fs_write_range_step() for the full contract every backend
    // implementing these two must honor. Both required (not optional/
    // NULLable) since there's exactly one backend today (tfs.c) and it
    // implements them -- see this header's top comment on why a
    // mount-point scheme (which might want optional capabilities per
    // backend) isn't what this struct is for.
    void *(*write_range_begin)(const char *path, uint64_t offset, const void *buf, uint32_t len);
    int (*write_range_step)(void *handle); // returns an fs_step_result (fs.h) as a plain int -- see that header for why

    // Steppable read -- Phase 4 of the async-I/O roadmap item, the read
    // counterpart to write_range_begin/_step above. Same reasoning for
    // being required (not optional/NULLable): exactly one backend
    // today, and it implements both. See fs.h's fs_read_range_begin()/
    // fs_read_range_step() for the full contract every backend
    // implementing these two must honor -- note read_range_step takes
    // an extra `out_total` out-param write_range_step doesn't need (a
    // read can finish short at EOF; a write can't).
    void *(*read_range_begin)(const char *path, uint64_t offset, void *buf, uint32_t len);
    int (*read_range_step)(void *handle, uint32_t *out_total); // returns an fs_step_result (fs.h) as a plain int

    int (*is_dir)(const char *path);
    int (*exists)(const char *path);
    void (*list)(const char *dir_path, void (*cb)(const char *name, uint32_t size, int is_dir));
    int (*stat)(const char *path, struct fs_timestamps *out); // see fs.h's fs_stat()

    // Backs fs_disk_usage() -- see fs.h's doc comment for the
    // byte-scaled, metadata-excluded contract every backend must
    // honor here.
    int (*disk_usage)(uint64_t *out_used_bytes, uint64_t *out_total_bytes);

    // Backs fs_check() -- see fs.h for the full contract (what a
    // repair pass will and won't fix, and why double-allocation is
    // deliberately report-only). Required, not optional/NULLable, same
    // reasoning as the steppable pairs above.
    int (*check)(int repair, struct fs_check_result *out);
};

#endif
