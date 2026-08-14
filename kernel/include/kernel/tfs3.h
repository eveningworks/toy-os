#ifndef TFS3_H
#define TFS3_H

#include "fs_ops.h"

// The TFS3 backend (kernel/fs/tfs3.c): block groups + inodes + dirent
// blocks, per docs/tfs3-design.md. Registered ahead of tfs_ops in
// vfs.c's backend list. Read side + probe/format landed first (Stage
// B of the plan); the write path, journal transactions and fsck land
// in Stages C/D -- until then every mutating op fails honestly.
extern const struct fs_ops tfs3_ops;

#endif
