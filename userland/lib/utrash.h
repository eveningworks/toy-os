#ifndef ULIB_UTRASH_H
#define ULIB_UTRASH_H

// THE RECYCLE BIN: a delete that can be taken back, on the freedesktop
// Trash specification's layout so a Linux desktop reading the same disk
// sees the same bin. Each bin is a directory holding `files/` (what was
// deleted, under a name unique in that bin) and `info/<name>.trashinfo`
// (where it came from and when):
//
//     [Trash Info]
//     Path=/usr/share/pictures/dusk-24bit.bmp
//     DeletionDate=2026-10-06T09:41:12
//
// **ONE BIN PER VOLUME, because a delete must be a RENAME** -- copying a
// tree to another disk to "delete" it would be slow, could fail half
// way, and would need the space it is supposed to free. The system
// volume's bin is `/home/.Trash` (the spec's "home trash"; toy-os has
// one home); any other persistent volume's is `<mount>/.Trash-0` (the
// spec's per-user top-directory bin). A RAM volume and a read-only one
// have NONE: utrash_bin_for() says 0 and the caller deletes for real,
// after saying so -- what Windows does for a drive with no Recycle Bin.
//
// The info file is created FIRST with O_EXCL, which claims the name in
// the bin atomically; the rename follows, and a failed rename removes
// the claim. A `files/` entry with no info file is an orphan the
// listing skips, never a crash. `rm` stays permanent, as it is on every
// system that has a bin.
//
// Path= is percent-encoded per the spec, and a parser REJECTS a file it
// cannot read rather than guessing at an original location.

#include <stdint.h>
#include "rtctime.h"

#define UTRASH_PATH 256

struct utrash_item {
    char name[64];             // its name in the bin's files/
    char bin[UTRASH_PATH];     // the bin it is in
    char orig[UTRASH_PATH];    // where it was deleted from
    struct rtc_time deleted;   // UTC, like every other timestamp
    uint64_t size;             // 0 for a folder
    int is_dir;
};

// The bin `path` would go to: 1 and the bin's directory in `out`, or 0
// when its volume has none (RAM, read-only, or the path is a bin).
int utrash_bin_for(const char *path, char *out, int cap);

// Move `path` into its volume's bin. 0, or -errno (-ENOTSUP: no bin on
// that volume). `out`, when given, describes where it went -- what an
// Undo needs to put it back.
int utrash_put(const char *path, struct utrash_item *out);

// Every item in every bin, newest first. Returns how many were written
// (at most `cap`); a bin that cannot be read is skipped.
int utrash_list(struct utrash_item *out, int cap);

// The item's current path, inside the bin.
int utrash_item_path(const struct utrash_item *it, char *out, int cap);

// Put it back where it was. -EEXIST when something now has that name,
// -ENOENT when the folder it came from is gone (it is NOT recreated:
// Windows recreates it, which restores a path the user deleted on
// purpose). 0 on success.
int utrash_restore(const struct utrash_item *it);

// Forget an item whose files/ entry the caller has removed for good
// (ufileop_remove, which reports progress a big folder needs): drops
// its info file. 0 or -errno.
int utrash_forget(const struct utrash_item *it);

#endif
