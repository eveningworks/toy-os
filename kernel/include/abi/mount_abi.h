#ifndef MOUNT_ABI_H
#define MOUNT_ABI_H

#include <stdint.h>

// The kernel<->userland contract for SYS_MOUNT and SYS_UMOUNT.
//
// WHAT REAL SYSTEMS TAKE. Linux's mount(2) is
// `mount(source, target, filesystemtype, flags, data)` -- five
// arguments, the last of them a free-form per-filesystem option string;
// Windows has no mount syscall at all (a volume is an object and
// mountvol/SetVolumeMountPoint work through the object manager).
//
// This is Linux's shape with the two parts toy-os cannot honestly
// carry left out. There is no `data` string, because no backend here
// has per-filesystem options and inventing a parser for none would be
// the second implementation of nothing. And SOURCE IS NOT A PATH: this
// OS has no /dev, so a volume is named by its PARTITION NUMBER on the
// boot disk (`2`), or by a backend name for one that needs no volume
// (`ramfs`). A struct rather than five registers, for the same reason
// SYS_MKPART takes one: one user pointer to validate instead of four.
//
// NO PRIVILEGE CHECK, and that is stated rather than implied -- this
// kernel has no uid. Mounting is not destructive (nothing is formatted,
// nothing is overwritten), so unlike SYS_MKPART there is not even a
// confirm flag to stand in for one. What it CAN do is hide a directory
// and expose a volume's contents, so a real system would gate it; the
// place to put that check when uids arrive is sys_mount().

// Per-mount flags. Mirrors kernel/mount.h's MNT_* one for one -- two
// names for one bit, because an ABI header may not include a kernel
// one, and the kernel-side mount_add() is what enforces it.
#define SYS_MNT_RDONLY 0x1

#define MOUNT_SOURCE_MAX 16 // "2", "ramfs" -- a partition number or a backend name
#define MOUNT_FSTYPE_MAX 16 // "fat32", "tfs3", or empty to probe
#define MOUNT_POINT_MAX  64 // FS_PATH_MAX, spelled out since abi/ has no fs.h

struct mount_request {
    char source[MOUNT_SOURCE_MAX];
    char fstype[MOUNT_FSTYPE_MAX];  // empty = probe every backend that could claim it
    char point[MOUNT_POINT_MAX];    // a normalized absolute path that already exists
    uint32_t flags;                 // SYS_MNT_*
    // A SIZE LIMIT FOR THIS MOUNT, in MiB; 0 means the backend decides.
    //
    // This is the `data` string's job in Linux, done as a typed field
    // instead -- the header above argues against inventing a parser for
    // options no backend had, and ramfs is the first backend that has
    // one. A number needs no parser and cannot be mistyped into
    // something that silently means nothing.
    //
    // It fits in what used to be `reserved`, so the struct is the same
    // size it always was and a caller built before this still compiles
    // to a zero here, which is the default. That is why the field was
    // reserved in the first place.
    uint32_t size_mib;
};

// --- SYS_MKFS ---------------------------------------------------------
//
// WHY THIS IS NOT `fsformat` WITH AN ARGUMENT. fs_format_backend() --
// the ring-0 `fsformat` command -- reformats the volume this machine is
// RUNNING FROM, unmounting everything first and re-probing after. That
// is a different operation from "put a filesystem on that other
// partition", which is what an installer wants and which must not
// disturb the running system at all.
//
// The device is named the same way mount_request.source names one, so
// there is one spelling of "which volume" across the ABI.
struct mkfs_request {
    char source[MOUNT_SOURCE_MAX];  // "ahci0p3" -- a partition, never a disk
    char fstype[MOUNT_FSTYPE_MAX];  // "tfs3", "fat32" -- never probed
    uint32_t flags;                 // MKFS_CONFIRM
    uint32_t reserved;
};

// The caller states that it knows this destroys whatever is on the
// volume. Same stand-in as MKPART_CONFIRM, and the same caveat: it is a
// speed bump, not a permission check, because this OS has no privilege
// model to make it one.
#define MKFS_CONFIRM 0x1

// --- SYS_FS_CHECK -----------------------------------------------------
//
// THE CHECK RUNS IN THE KERNEL, AGAINST THE MOUNTED VOLUME; a ring-3
// `fsck` asks for it and prints the result. That is XFS's online scrub
// and `btrfs scrub`, not e2fsck: the checker walks the backend's live
// state and repairs through its journal, which a process reading the
// raw device could not do without unmounting -- and the root is never
// unmounted. The volume is named by any PATH on it, as statfs(2) does.
//
// The counts below are BLOCKS (or, for records_used, inodes), with the
// meanings fs.h's fs_check() documents.
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
    // WHICH VOLUME the path named -- filled by SYS_FS_CHECK after the
    // pass, so a caller need not resolve the path a second time.
    char fstype[MOUNT_FSTYPE_MAX];  // "tfs3"
    char point[MOUNT_POINT_MAX];    // "/" -- the mount the path resolved to
};

// Also fix what can be fixed without guessing (fs.h's fs_check()).
// Refused with -ENOTSUP by a backend that can only report.
#define FSCK_REPAIR 0x1

#endif
