# fsck

**a `/bin` program.**

**Category:** Disk and filesystem maintenance

## Synopsis

```
fsck [-r] [PATH]
```

## Options

- `PATH` -- any path on the volume to check; the default is `/`. A
  relative path is resolved against the current directory, so `cd /boot`
  then `fsck .` checks the ESP.
- `-r`, `--repair` -- also fix what can be fixed without guessing.
  `fsck repair` means the same, the spelling the kernel's own messages
  use.
- `-h`, `--help` -- the options.

## Description

Walks every file's block tree against the free-block bitmap. On TFS3 it
also verifies inode checksums, link counts and `.`/`..`, reclaims
orphans, and with `-r` restores a damaged primary superblock from its
backups. FAT32 (`/boot`) is checked for cross-linked clusters and FAT
copies that disagree, and is never repaired -- a half-repair on the
partition holding the bootloader is worse than a report. Read-only
unless `-r` is passed.

```
/$ fsck /boot
fsck: checked (read-only) fat32 on /boot
  records in use:        0
  blocks referenced:     7661
  ...
fsck: clean.
```

**THE CHECK RUNS IN THE KERNEL** (`SYS_FS_CHECK`), against the MOUNTED
volume, and this program prints it -- XFS's online scrub and `btrfs
scrub`, not e2fsck. The checker walks the backend's live state and
repairs through its journal, which a program reading the raw device
could only do on an unmounted volume, and the root is never unmounted.
It holds the volume's lock for the whole pass, so other programs'
reads and writes on that volume wait it out.

**Double-allocated blocks are reported, never repaired**: choosing
which of two files keeps a shared block destroys the other's data.
Delete one of the files instead.

## Exit status

e2fsck's: `0` clean, `1` problems were found and all of them fixed, `4`
problems are left (always, for a double allocation), `8` the check
itself failed -- a repair of a read-only mount or a filesystem that can
only report (`ramfs`, `fat32`). `2` is a usage error.

## When /bin is damaged

The kernel shell keeps its own copy, for the root only: `rescue fsck`
and `rescue fsck repair` ([`rescue`](rescue.md)).

## See also

[`df`](df.md) for what is mounted where, [`sync`](sync.md) to flush the
disk cache, and [`dmesg`](dmesg.md) for what a check logged.
