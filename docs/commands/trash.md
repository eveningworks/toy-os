# trash

**a `/bin` program.**

**Category:** Files and the filesystem

## Synopsis

    trash COMMAND [ARG]...

## Options

- `put FILE...` -- move each FILE (a file or a folder) into its volume's
  Recycle Bin. A name the bin already holds gains a number, and the new
  name is printed: `dusk.jpg -> dusk.2.jpg`.
- `list` -- everything in the bins, newest first: the item's name in its
  bin, its size (or `folder`), when it was deleted, and where from.
- `restore NAME...` -- put an item back where it was deleted from. NAME
  is the first column of `trash list`. Refused when something now has
  that name, or when the folder it came from is gone -- it is not
  re-created.
- `empty` -- delete everything in the bins for good.
- `-h`, `--help` -- every command in one page.

Exit status: 0 done, 1 something refused or failed, 2 bad usage
(`Try 'trash --help'`).

## Description

The Recycle Bin from a prompt -- the same bins the File Manager's Delete
and the desktop's Delete use, so either sees what the other did. `gio
trash` and trash-cli are this command on a Linux desktop.

```
/$ trash put /home/notes.txt /usr/share/pictures/dusk-24bit.bmp
/$ trash list
dusk-24bit.bmp              1.4M  2026-10-06 09:41  /usr/share/pictures/dusk-24bit.bmp
notes.txt                     2K  2026-10-06 09:41  /home/notes.txt
/$ trash restore notes.txt
/home/notes.txt
```

**One bin per volume**, because putting something in the bin is a
rename, never a copy: the system volume's bin is `/home/.Trash`, any
other disk's is `.Trash-0` at its top. Each holds `files/` and
`info/NAME.trashinfo` (the original path and the time), the freedesktop
Trash layout, so a Linux desktop reading the same disk sees the same
bin. A RAM volume (`/tmp`) and a read-only one (`/boot`) have no bin:
`trash put` refuses, and `rm` deletes.

**`rm` stays permanent**, as it is on every system with a bin.

See also: [`rm`](rm.md), [`mv`](mv.md).
