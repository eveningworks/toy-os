#ifndef FSWATCH_H
#define FSWATCH_H

#include <stdint.h>

// Path watches, for the compositor: a change AT a watched path, or
// directly inside a watched directory, posts WIN_EV_FSWATCH with the
// watch's id (abi/win_proto.h). SYS_FS_WATCH is the only way in.
//
// **IT EXISTS SO A DESKTOP DOES NOT POLL THE DISK.** The alternative was
// fs_generation(), one counter for every write in the machine, and the
// WM re-read its configs whenever it moved -- so a program writing to
// /var/tmp all day had the compositor reading /etc every frame, each
// read queuing for the filesystem lock behind that program's disk
// wait. inotify's shape, minus the fd: the events go where the
// compositor already waits.
//
// MATCHED BY HASH, not by string: a watch stores the FNV-1a of its
// path, and vfs.c hands over the hashes of a changed path and of its
// parent. A stepped write finishes with no path in hand, and 16 slots of
// FS_PATH_MAX would be 64 KiB of kernel data to avoid a collision whose
// only cost is one spurious reload.
#define FSWATCH_MAX 16

// The hashes of `path` and of its parent directory ("/" for a top-level
// name). `path` is absolute and canonical, as vfs.c receives it; a
// trailing slash is ignored.
void fswatch_hash(const char *path, uint64_t *self, uint64_t *parent);

// Registers `path` for `pid`. Returns the id (> 0), the SAME id for a
// path that pid already watches, or -ENOSPC when the table is full.
int fswatch_add(int pid, const char *path);

// Something at `self` changed; its parent is `parent`. Fires each watch
// on either. vfs.c calls this on every successful mutation.
void fswatch_note(uint64_t self, uint64_t parent);

// Drops every watch `pid` holds.
void fswatch_owner_gone(int pid);

// How many times watch `id` has fired since it was added. For a KTEST.
uint32_t fswatch_fires(int id);

#endif
