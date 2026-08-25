#ifndef FAT32_H
#define FAT32_H

#include "fs_ops.h"

// FAT32, as a VFS backend -- the format GRUB reads, and therefore the
// format `/boot` is in on this OS's own disk (tools/install_grub.py
// writes it with mtools). See kernel/fs/fat32.c for the layout and the
// decisions; kernel/mount.h for how it comes to be mounted at /boot.
//
// Nothing about this driver knows it holds a bootloader, which is the
// same separation Linux keeps: `fs/fat/` is a generic driver and the
// ESP is an ordinary mount. What DOES know is the mount policy, which
// mounts it read-only by default.
extern const struct fs_ops fat32_ops;

#endif
