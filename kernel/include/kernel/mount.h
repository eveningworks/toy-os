#ifndef MOUNT_H
#define MOUNT_H

#include <stdint.h>
#include "fs.h"     // FS_PATH_MAX
#include "fs_ops.h" // struct fs_ops

struct block_device;

// THE MOUNT TABLE: which filesystem answers for which part of the path
// tree. vfs.c used to hold one `g_fs` pointer and hand every call to
// it; this is what replaced it, and the reason is `/boot` -- a FAT32
// partition GRUB wrote, sitting on the same disk as a TFS3 root, which
// no amount of single-backend dispatch can make readable.
//
// WHAT REAL SYSTEMS DO. Linux keeps a tree of `struct mount`, looks up
// per path component while walking, and crosses a mount point when a
// dentry is one; the ESP is an ordinary FAT mount at /boot/efi with
// nothing in the driver aware it holds a bootloader. Windows makes a
// volume an object in the NT namespace and mounts it by drive letter or
// reparse point; its ESP is a real volume that simply has no letter.
// toy-os follows the SHAPE of the first -- a table, a longest-prefix
// lookup, a generic FAT driver -- and none of the size: no per-process
// namespaces, no bind mounts, no automounts, no fstab.
//
// THE FIVE RULES, decided up front because getting them wrong is
// silent (docs/decisions/storage.md carries the reasoning):
//
//  1. LONGEST PREFIX WINS, at a COMPONENT BOUNDARY. `/boot` claims
//     `/boot` and `/boot/grub/x`; it does NOT claim `/bootloader`.
//     Matching on a bare string prefix is the bug that would give one
//     filesystem another's files.
//  2. THE BACKEND IS HANDED A ROOT-RELATIVE PATH. `/boot/grub/x` on a
//     mount at `/boot` arrives as `/grub/x`, and the mount point
//     itself as `/`. A backend never learns where it is mounted, which
//     is what lets the same driver serve `/` and `/boot`.
//  3. MOUNTING OVER A NON-EMPTY DIRECTORY IS ALLOWED, AND HIDES IT --
//     Unix's rule. The directory must EXIST and be a directory (which
//     is Linux's rule, and the reason `ensure_layout()` creates
//     `/boot`); what was in it is untouched and comes back on umount.
//  4. AN OPERATION MAY NOT CROSS A MOUNT. rename() and link() take two
//     paths, and if they resolve to different mounts the call is
//     REFUSED rather than silently doing something else -- Unix's
//     EXDEV, and the reason `cp` exists.
//  5. `..` CANNOT ESCAPE A MOUNT ROOT, and costs no code: every path
//     reaching fs_* is already normalized with no `.`/`..` components
//     (see api/fs.h), so `/boot/..` is `/` before any mount is
//     consulted. That is a property to keep, not a check to add --
//     a backend that started accepting `..` would break it.
#define MOUNT_MAX 6

// Per-mount flags. Read-only first, because it is the one every real
// system has and the one `/boot` genuinely wants.
#define MNT_RDONLY (1u << 0)

struct mount {
    int used;
    char point[FS_PATH_MAX];        // "/" for the root, else "/boot" (no trailing slash)
    int point_len;                  // strlen(point); 1 for the root
    const struct fs_ops *fs;
    const struct block_device *dev; // NULL for a backend with no volume (ramfs)
    unsigned flags;                 // MNT_*
    int persistent;

    // This mount's backend state (fs_ops.h's state_alloc), or NULL for
    // a backend that declares none. It is what makes two mounts of one
    // backend two filesystems rather than one read twice.
    void *state;
};

// ---- making a mount's backend state current -------------------------
//
// EVERY backend call runs between these two. mount_enter() points the
// backend at this mount and hands back what was current; mount_leave()
// puts that back. RESTORE, not clear -- they NEST, because an fs_list()
// callback that calls fs_* runs a whole enter/leave inside the walk.
// At the outermost level the previous state is NULL, so a backend
// reached with no enter at all still faults rather than reading
// whichever volume ran last.
//
// Both are cheap (one indirect store) and both are safe on a backend
// with no state ops.
void *mount_enter(const struct mount *m);
void mount_leave(const struct mount *m, void *prev);

// A SCRATCH state, for an operation on a volume nothing has mounted:
// probe, format, wipe. Returns 0 only when the backend could not
// allocate one. This is what lets `mkfs` point a backend at a foreign
// device without a mounted one noticing -- there is no longer anybody's
// state to disturb, so there is nothing to save and restore.
struct fs_scratch {
    const struct fs_ops *fs;
    void *st;
    void *prev;
};

int mount_scratch_begin(const struct fs_ops *fs, struct fs_scratch *sc);
void mount_scratch_end(struct fs_scratch *sc);

// Which mount answers for `path`, and what the backend should be
// handed. `out_sub` must be at least FS_PATH_MAX bytes; it receives the
// root-relative path (rule 2). Returns NULL only when nothing at all is
// mounted, which is a machine with no filesystem.
const struct mount *mount_resolve(const char *path, char *out_sub, int sub_cap);

// The root mount, or NULL. What fs_backend_name()/fs_is_persistent()
// answer for, since every caller that predates mounts means the root.
const struct mount *mount_root(void);

// Iteration, for `df`, `/bin/mount` and the QUERY_FSINFO provider.
// mount_at() returns NULL past the end; the order is the order they
// were mounted, root first.
int mount_count(void);
const struct mount *mount_at(int index);

// ---- mounting and unmounting ---------------------------------------
//
// Both set *why to a short reason on failure -- a refusal a user can
// act on ("not a directory", "already mounted", "in use") is the whole
// point of a mount command, and a bare 0 would make every one of them
// read the same.

// `dev` may be NULL for a backend that needs no volume (ramfs).
// `fstype` names a backend, or is NULL to probe every one that could
// claim the device. `point` is a normalized absolute path that must
// already exist as a directory on whatever currently answers for it.
// `size_bytes` caps what the mount may hold, 0 meaning the backend
// decides. Only ramfs has anything to do with it (fs_ops.h's init).
int mount_add(const struct block_device *dev, const char *fstype,
              const char *point, unsigned flags, uint64_t size_bytes,
              const char **why);

// Refuses the root, and refuses a mount with an open file under it.
int mount_remove(const char *point, const char **why);

// A partition of the boot disk, by its 1-based number -- what
// `/bin/mount`'s device argument names, since this OS has no /dev.
// NULL if there is no such partition.
const struct block_device *mount_partition_device(int number);

// Boot-time, in two halves, and the ORDER is the point: the root has
// to be mounted before fs_init() can create `/boot` on it, and `/boot`
// has to exist before anything can be mounted over it (rule 3). So
// fs_init() runs mount_boot_root(), then its layout pass, then
// mount_boot_auto().
void mount_boot_root(void);

// Mounts what mounts itself: today, the boot disk's FAT32 ESP at
// `/boot`. Silent and harmless when there is no such partition, no FAT
// backend claims it, or `/boot` does not exist.
void mount_boot_auto(void);

// Re-read one disk's partition table and NAME the windows it describes,
// so `<disk>p<n>` becomes a device something can format and mount.
// Returns how many were named. Linux's BLKRRPART, and the same rule
// applies: the CALLER decides whether the disk is busy -- rescanning a
// disk the machine is running from would hand out windows over a
// filesystem in use, which is why sys_mkpart() only does it for a disk
// nothing is mounted from.
//
// IT REPLACES this disk's windows rather than adding to them: any that
// nothing is mounted from are released first. Adding was the original
// behaviour and it was wrong in a way that booted -- a partition that
// changed SIZE got a second window with the same name, and the stale one
// answered `blk_device_by_name()`, so `install` formatted the old window
// and left a 119 GB partition holding a 441 MB filesystem.
//
// A window something IS mounted from is kept, and then the table has a
// name that no longer describes what is on the disk -- which is why
// sys_mkpart() only rescans a disk nothing is mounted from.
int mount_rescan_disk(const struct block_device *disk);

// Re-run the boot probe after a format wrote a new filesystem.
// Returns 1 if `expect` ended up as the root backend. Drops every
// mount first: a reformat invalidates the root, and anything mounted
// under it was reached through a path the root owns.
int mount_reprobe_root(const struct fs_ops *expect);

// The ON-DISK backend registry, reached by name -- what `fsformat`
// resolves its argument through. ramfs is deliberately NOT in it (see
// mount.c), so `fsformat ramfs` finds nothing and refuses.
const struct fs_ops *mount_backend_named(const char *name);

// The wipefs rule: erase every OTHER backend's signatures from `dev`
// before formatting it with `target`, so nothing stale -- a primary
// the new format does not happen to overwrite, or a far-away backup
// superblock -- can outclaim the freshly written filesystem at the
// next probe.
int mount_wipe_others(const struct fs_ops *target, const struct block_device *dev);

#endif
