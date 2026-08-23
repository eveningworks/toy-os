# rm

**a `/bin` program.**

**Category:** Files and the filesystem

## Synopsis

    rm [-r] <path> [path...]

## Description

Delete files. A directory is deleted only when it is empty, unless `-r`
is given.

`-r` empties a tree from the leaves up and then removes each directory,
because the kernel's `fs_delete()` refuses a non-empty directory —
there is no recursive unlink behind it and there should not be. The walk
is breadth-first into a queue of at most 64 directories, then a reverse
pass over that queue, which is a post-order removal with no recursion;
a single listing is capped at 256 entries by `SYS_LISTDIR` and anything
past that is reported rather than quietly left behind.

There is no prompt and no trash. `rm -r` on the wrong path is as final
here as anywhere else.
