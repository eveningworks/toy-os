# cp

**a `/bin` program.**

**Category:** Files and the filesystem

## Synopsis

    cp [-r] <source> <dest>

## Description

Copy a file, or a whole tree with `-r`. If `dest` is an existing
directory the source is copied *into* it under its own name, which is
what `cp notes.txt /docs` has meant since the 1970s; otherwise `dest` is
the new file's name.

There is no kernel call behind this. Copying is a read loop and a write
loop, and it lives here rather than in the kernel for the same reason
`/bin/ls` does — see `docs/decisions/shell.md`. The GUI file manager
spawns this program rather than carrying a copy loop of its own, so
there is exactly one implementation of what copying means.

Four refusals, each deliberate:

- **A directory without `-r`** is refused, not silently skipped.
  `-r` is how you say a tree was meant.
- **Copying a directory into itself** (`cp -r /docs /docs/backup`) is
  refused before it starts, rather than discovered when the disk fills.
- **Source and destination being the same file** is refused.
- **A short write** — a full disk — aborts with the reason rather than
  leaving a truncated file that looks copied.

`-r` walks breadth-first over a queue of at most 64 pending
directories, and a single directory listing is capped at 256 entries by
`SYS_LISTDIR`; both limits are reported rather than passed over
quietly.

Nothing is preserved but the contents: there are no permissions or
ownership in TFS3 to carry, and the copy's timestamp is the copy's.
