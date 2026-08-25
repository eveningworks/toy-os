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
    uint32_t reserved;
};

#endif
