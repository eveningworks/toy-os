#ifndef TFS_H
#define TFS_H

#include "fs_ops.h"

// tfs.c's backend vtable -- see fs_ops.h for what this is and why it
// exists. Backend-internal: only vfs.c (fs_init()) should reference
// this. Nothing else, including apps/, should ever #include this
// header -- they go through fs.h instead.
extern const struct fs_ops tfs_ops;

#endif
