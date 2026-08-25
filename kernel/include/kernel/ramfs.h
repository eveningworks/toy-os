#ifndef RAMFS_H
#define RAMFS_H

#include <stdint.h>
#include "fs_ops.h"

// The ramfs backend (kernel/fs/ramfs.c): a filesystem in the kernel
// heap, with no block device under it at all. It is what vfs.c mounts
// wherever there is no usable drive -- a diskless boot, or a drive
// holding no filesystem this kernel can mount.
//
// It is chosen by POLICY, never by probe(): ramfs_probe() always
// answers 0, because there is no superblock to recognise. See
// docs/rootfs-design.md.
extern const struct fs_ops ramfs_ops;

// ---- the test seam ----
//
// The KTESTs run inside a LIVE kernel with a real root mounted, so
// they must not mount ramfs through vfs.c and swap the active backend
// out from under the rest of the suite. They mount this instance
// directly and put it back, the same discipline partition_test.c
// applies to the block device.
//
// `budget_bytes` overrides the half-of-free-memory default so a test
// can reach the full-filesystem path without allocating a gigabyte;
// 0 keeps the default.
int ramfs_test_mount(uint64_t budget_bytes);
void ramfs_test_unmount(void);
uint64_t ramfs_test_used(void);

#endif
