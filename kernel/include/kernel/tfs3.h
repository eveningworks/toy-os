#ifndef TFS3_H
#define TFS3_H

#include <stdint.h>
#include "fs_ops.h"

// The TFS3 backend (kernel/fs/tfs3.c): block groups + inodes + dirent
// blocks, per docs/tfs3-design.md. Registered ahead of tfs_ops in
// vfs.c's backend list. Read side + probe/format landed first (Stage
// B of the plan); the write path, journal transactions and fsck land
// in Stages C/D -- until then every mutating op fails honestly.
extern const struct fs_ops tfs3_ops;

// WHAT PATH RESOLUTION HAS COST SINCE BOOT: lookups, the block reads
// they caused, and the time in them. Read by QUERY_FSSTAT.
//
// Every fs_*(path, ...) call resolves from the root, so this is a
// per-syscall cost a throughput number cannot separate from the data
// traffic beside it. Any pointer may be NULL.
void tfs3_lookup_stats(uint64_t *calls, uint64_t *reads, uint64_t *ns);

// How many transactions the IDLE path has forced (storage.sync =
// batched). Distinct from commits caused by anything else opening a
// transaction, which is the only way a test can tell the two apart.
uint64_t tfs3_idle_commits(void);

// TEST SEAM (tfs3_test.c): how `ino` is locked on this state right now --
// 2 exclusive, 1 shared, 0 not held. See tfs3.c's t3_lock().
int tfs3_test_lock_mode(void *st, uint64_t ino);

#endif
