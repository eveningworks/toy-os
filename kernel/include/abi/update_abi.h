#ifndef ABI_UPDATE_ABI_H
#define ABI_UPDATE_ABI_H

// The contract between /bin/update and the kernel for a replacement
// staged across a reboot (docs/update-design.md, "Applying at boot").
//
// /bin/update downloads each file to `<target>` UPDATE_STAGED_SUFFIX,
// verifies it, and appends `<target>\n` to UPDATE_PENDING_PATH. At the
// next boot the kernel renames every staged file over its target BEFORE
// it spawns init, so nothing has a library mapped while it changes --
// Windows' PendingFileRenameOperations, which smss applies for the same
// reason. An OLD userland writes this list and a NEW kernel reads it, so
// the format only ever grows.
//
// A line names the TARGET only: the source is always the target plus
// the suffix, so the list cannot move an arbitrary file anywhere.
#define UPDATE_PENDING_PATH  "/var/lib/update/pending"
#define UPDATE_STAGED_SUFFIX ".upd"

#endif
