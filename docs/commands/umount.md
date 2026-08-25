# umount

**a `/bin` program.**

**Category:** Storage

## Synopsis

    umount <mountpoint>

## Description

`/bin/umount` detaches a filesystem. The volume is **flushed before the mount
is forgotten** — the kernel does that, because a write-back cache's failure
surfaces at the flush, and after the mount slot is cleared there is no owner
left to report it to.

Whatever the mount point's directory held before it was mounted over comes
back.

## What it refuses, and why

- **The root.** There would be nothing left to resolve a path against.
- **A filesystem with a file still open on it.** The kernel walks its open-file
  table and looks for a path under the mount point — `struct open_file` already
  records a file descriptor's absolute path, so this needs no bookkeeping that
  could drift out of step with what it counts.
- **A filesystem with another one mounted underneath it.** Unmount the deeper
  one first.

All three are `EBUSY`-shaped: the same command would succeed later.

## What it deliberately does not do

- **No `-l` (lazy) and no `-f` (force).** Both exist on Linux because a network
  filesystem can hang, which nothing here can, and both leave a window where a
  process holds a file on a filesystem that is gone. Not implemented, and that
  is a decision rather than an omission.

## Examples

    umount /boot
    mount 2 /boot                  # ...and back, read-write this time

## See also

`mount`, `df`, `sync`, `fsck`
