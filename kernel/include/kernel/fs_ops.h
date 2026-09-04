#ifndef FS_OPS_H
#define FS_OPS_H

#include <stdint.h>
#include "fs.h" // struct fs_stat_info + FS_CAP_* bits, for .stat/.caps below

struct block_device; // kernel/block.h -- what probe()/init() are handed

// The VFS backend interface -- fs.h's public fs_* API (kapi.h's stable
// surface, unchanged by any of this) is implemented by vfs.c as a thin
// dispatch layer over exactly one of these, chosen once in fs_init().
// Adding a second on-disk filesystem in the future means writing a new
// `struct fs_ops` (see tfs3.c/tfs3.h for the reference implementation)
// and pointing vfs.c's fs_init() at it -- fs.h, kapi.h, and every
// existing caller (apps/, syscall.c, tz.c, font_config.c, ...) need no
// changes at all, because they only ever call the fs_* wrappers, never
// a backend directly.
//
// THIS IS NOW A MOUNT-POINT SCHEME, and the prediction the paragraph
// here used to make turned out right: the struct did not change shape,
// it just gets looked up differently. vfs.c holds a MOUNT TABLE (see
// mount.h) and picks a backend per path by longest-prefix match, so
// two filesystems are readable at once -- TFS3 at `/` and FAT32 at
// `/boot`, which is what made this necessary.
//
// What DID have to change is where a backend's volume comes from.
// probe(), format(), wipe() and init() take a `struct block_device *`
// now instead of reading blk_active(): with two mounts there is no
// single active device a backend could correctly assume, and one that
// assumed anyway would read the wrong volume and report no error.
// Linux's `super_block->s_bdev`, arriving for the same reason.
//
// A backend with per-mount statics can only be mounted ONCE, and says
// so with `max_mounts` below rather than being found out.
//
// Every function pointer here mirrors a fs.h entry point exactly (same
// signature, same normalized-absolute-path contract, same return-value
// meaning) -- see fs.h's top comment for what callers can assume about
// paths. A backend only has to implement path *handling*, not path
// *normalization*; vfs.c does not renormalize before calling into a
// backend, so each backend is expected to normalize itself exactly the
// way tfs3.c's normalize()/path_is_normalized() do (that logic is
// backend-internal, not shared, since a future filesystem might have
// different path rules -- e.g. case-insensitivity, a different max
// length).
struct fs_ops {
    const char *name; // short identifier, e.g. "tfs3" -- fs_backend_name() reports it

    // FS_CAP_* bits (fs.h) this backend's FORMAT genuinely supports.
    // Same contract as display.h's caps field: one fact stated twice
    // (the bit, and -- once optional ops exist -- the matching function
    // pointer), with the probe loop in vfs.c refusing a backend whose
    // two statements disagree. Today every op below is required, so
    // the honesty check has nothing optional to cross-check yet; it
    // started biting with the first optional op, link() below.
    uint32_t caps;

    // CAN THIS BACKEND LIVE ANYWHERE BUT LBA 0? 1 if every access goes
    // through the block layer and is volume-relative, so the backend is
    // mountable from inside a partition; 0 if it addresses the disk
    // absolutely and must own the whole of it.
    //
    // This is not an FS_CAP_* bit on purpose: those describe what a
    // FORMAT supports and are reported to userland by fs_has(), while
    // this describes how the DRIVER is wired and is nobody's business
    // above vfs.c.
    //
    // EVERY BACKEND THAT EXISTS TODAY DECLARES 1, and the field still
    // earns its place. TFS2 declared 0 and was the reason it exists: it
    // made 24 direct ata_* calls that bypassed the block layer, so a
    // partition device under it was ignored and its probe read the
    // DISK's LBA 0 -- claiming a partition it had never looked at. It
    // also put its superblock at LBA 0 and its journal header at LBA 1,
    // which are the MBR and the GPT header.
    //
    // TFS2 is gone, but the hazard belongs to ANY backend that reaches
    // past blk_*, and FAT32 is next (docs/roadmap.md). The guard costs
    // one int and one `continue` in try_partitions(), which reads it on
    // every candidate -- a slot nobody consults is the other failure
    // this codebase keeps finding, so it is consulted.
    int volume_relative;

    // HOW MANY TIMES THIS BACKEND MAY BE MOUNTED AT ONCE. 1 for a
    // backend whose volume state is a single set of module-level
    // statics; more when each mount gets its own.
    //
    // This is declared rather than discovered because the failure is
    // silent: a second mount of a single-instance backend does not
    // crash, it quietly re-points one set of statics, so the FIRST
    // mount starts reading the second one's volume.
    //
    // The way past 1 is the three ops below -- the mount table owns a
    // state object per mount and makes it CURRENT around every call.
    // A backend that declares max_mounts > 1 without them is refused
    // at mount time (mount.c's caps_are_honest()).
    int max_mounts;

    // ---- per-mount state ---------------------------------------
    //
    // Allocate, make current, free. A backend keeps its volume state
    // in one heap struct reached through a `static struct X_state *S`,
    // and these three are how the VFS says which mount a call belongs
    // to. Linux hands `struct super_block *` to every op instead; this
    // sets it at the chokepoint rather than threading it through
    // twenty signatures, which it can do because the filesystem is
    // already one global critical section (vfs.c's FS_OP preemption
    // guard). THAT is the assumption to re-read on the day toy-os has
    // a second core -- see docs/smp-design.md.
    //
    // All three or none, checked at mount time.
    //
    // `state_activate` RETURNS WHAT WAS CURRENT, so a caller restores
    // rather than clearing. That is what makes the pair nest: an
    // fs_list() callback that calls fs_* runs a second enter/leave
    // inside the walk, and clearing instead of restoring left the outer
    // walk with no state at all (a GP fault the moment init read a
    // directory). The OUTERMOST leave still restores NULL, so a backend
    // reached with no activate at all faults on a NULL deref -- a panic
    // naming the line -- instead of writing one volume onto another.
    //
    // THE TRAP: nothing checks that a state struct is COMPLETE. A
    // per-volume field left outside it is shared by every mount, and
    // the symptom is cross-volume corruption with no error anywhere.
    void *(*state_alloc)(void);
    void (*state_free)(void *st);
    void *(*state_activate)(void *st);

    // Detection only -- read this backend's superblock location and
    // judge it. NEVER formats, never mounts, NO SIDE EFFECTS BEYOND THE
    // READ -- and that last clause became load-bearing with mount
    // points. A probe now runs while this backend may already be
    // MOUNTED SOMEWHERE ELSE, so one that records the device it was
    // handed repoints the live mount at a volume it was only asked to
    // look at. Both backends here save and restore their volume state
    // around the read because of it; the mount table also declines to
    // probe a backend that is already at its mount limit. Only called when a disk is actually present (vfs.c
    // owns ata_init()/ata_present() sequencing). Returns:
    //   1  this is my filesystem (magic + version + validation passed)
    //   0  readable, but not mine (blank or foreign bytes)
    //  -1  could not read the superblock at all -- vfs.c treats this
    //      as "refuse to touch the disk" (the data-loss lesson in
    //      tfs3.c's init comment), never as "blank, go format"
    int (*probe)(const struct block_device *dev);

    // Erase every signature by which probe() would recognize this
    // backend's filesystem on the disk -- the primary superblock AND
    // any backups. The wipefs rule, learned the hard way: formatting
    // a TFS3 disk as TFS2 overwrote TFS3's primary (it sits inside
    // TFS2's record-table region) but not its far-away backups, so
    // the TFS3 probe kept claiming the disk via backup and "mounted"
    // a corpse. fs_format_backend() calls every OTHER backend's
    // wipe() before formatting with the chosen one, so a reformat is
    // a clean identity change, not a seance. Idempotent; returns 1
    // on success (nothing to wipe counts as success).
    int (*wipe)(const struct block_device *dev);

    // Write a fresh, empty filesystem to the disk. Does NOT mount it
    // (fs_init()/fs_format_backend() call init() after). Returns 1 on
    // success, 0 on failure (too-small disk, write errors). Only
    // called deliberately: from the blank/foreign-disk policy in
    // fs_init(), or from the user-facing `fsformat` command -- a
    // backend never formats on its own initiative anymore.
    int (*format)(const struct block_device *dev);

    // Called once this backend is chosen, from fs_init(). Mounts what
    // probe() claimed (or format() just wrote). Returns:
    //   1  mounted, and every mutating call below is written through
    //      to real persistent storage
    //   0  mounted, but NOT persistent -- files vanish on reboot (what
    //      ramfs always answers; same meaning as fs_is_persistent()'s
    //      doc comment in fs.h, just per-backend here)
    //  -1  could not mount at all
    // Re-validates the disk itself (probe()'s answer isn't carried
    // over) so it degrades safely even if the disk changed between the
    // two calls.
    //
    // THE -1 IS WHY THIS IS THREE-VALUED, and it was a fiction before
    // it existed: TFS3 has no RAM-only mode, so every 0 it returned
    // meant "did not mount" -- and vfs.c, reading that as "mounted,
    // not persistent", announced an active backend on a machine where
    // every single fs_* call failed. The same 1/0/-1 shape probe()
    // already uses, for the same reason: two different kinds of no.
    int (*init)(const struct block_device *dev);

    int (*touch)(const char *path);
    int (*write)(const char *path, const char *data, int append);
    int (*mkdir)(const char *path);
    int (*del)(const char *path); // backs fs_delete() -- named del, not delete, to read fine if this header is ever pulled into a C++ tool
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
    // NULLable) since there's exactly one backend today (TFS3) and it
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

    // Both required, both backends implement them -- see fs.h's
    // fs_rename()/fs_truncate() for the contract (what is refused,
    // and which backend promises atomicity).
    int (*rename)(const char *oldpath, const char *newpath);
    int (*truncate)(const char *path, uint64_t size);

    int (*is_dir)(const char *path);
    int (*exists)(const char *path);
    void (*list)(const char *dir_path, void (*cb)(const char *name, uint32_t size, int is_dir));
    int (*stat)(const char *path, struct fs_stat_info *out); // see fs.h's fs_stat() -- ino + epoch times, converted by the backend if its format stores something else

    // Backs fs_disk_usage() -- see fs.h's doc comment for the
    // byte-scaled, metadata-excluded contract every backend must
    // honor here.
    int (*disk_usage)(uint64_t *out_used_bytes, uint64_t *out_total_bytes);

    // Backs fs_check() -- see fs.h for the full contract (what a
    // repair pass will and won't fix, and why double-allocation is
    // deliberately report-only). Required, not optional/NULLable, same
    // reasoning as the steppable pairs above.
    // Land anything the backend is holding back, before the block layer
    // is flushed. OPTIONAL -- NULL means "nothing is ever deferred",
    // which is true of every backend that commits as it goes.
    //
    // It exists because `storage.sync = batched` lets TFS3 keep a
    // journal transaction open across writes, and a `sync` that flushed
    // the DEVICE without first committing that transaction would report
    // durability it had not achieved. Returns 0 if the commit failed,
    // which fs_sync() must not treat as cosmetic.
    int (*sync)(void);

    int (*check)(int repair, struct fs_check_result *out);

    // ---- optional ops (the caps rule becomes real here) ----
    //
    // Everything above is required. From here down, an op may be NULL
    // when the backend's format can't support it -- and each optional
    // op is paired with an FS_CAP_* bit, ONE FACT STATED TWICE:
    // callers ask fs_has(), vfs.c's probe loop REFUSES a backend
    // whose bit and pointer disagree (caps_are_honest(), modeled on
    // display.c's). Don't add an optional op without its bit.

    // Optional, and NOT paired with a cap bit -- it is not a format
    // capability, it is a driver having something to release. Called by
    // mount_remove() after the volume is flushed and before the slot is
    // forgotten, so a backend can drop caches and free per-mount
    // buffers. A backend with nothing to release leaves it NULL.
    void (*umount)(const struct block_device *dev);

    // FS_CAP_HARDLINKS. Adds a second name for an existing FILE
    // (never a directory -- that makes the tree a graph); both names
    // are the same inode, and the data is freed only when the last
    // name goes. Paths follow the same normalized-absolute contract
    // as everything above.
    int (*link)(const char *existing, const char *newpath);
};

#endif
