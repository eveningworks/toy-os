# mount

**a `/bin` program.**

**Category:** Storage

## Synopsis

    mount [-r] [-t <fstype>] <device|partition|none> <mountpoint>
           mount                  list what is mounted

## Description

`/bin/mount` attaches a filesystem at a path, and with no arguments lists what
is already attached. Both halves go through the mount table in
`kernel/fs/mount.c`; the listing is read from `QUERY_FSINFO`, the same record
`df` formats differently, so there is no syscall here that only `mount` uses.

    device   on             type     options
    ata0p3   /              tfs3     rw
    ata0p2   /boot          fat32    ro

**The source is a DEVICE NAME, or a partition number, or `none`.**

A name is what `lsblk` prints — `ata0p3`, `ahci0p1`, `virtio0` — and it is the
only form that can reach a second disk. Every driver enumerates at boot and
every device it finds is named (`kernel/include/kernel/block.h`), so a machine
with two drives can mount either: `mount ahci0p1 /mnt`.

A bare NUMBER is the older form and still works, but it can only ever mean a
partition of the **root's** disk — `mount 2 /boot` is "the second partition of
the disk the root came from". That was unambiguous when one disk was all the
kernel could see. It no longer is, so prefer the name; the number is kept
because it is what `parttable` prints and what existing scripts pass.

A filesystem that needs no volume at all is named by type instead, with `none`
as the source — `mount -t ramfs none /mnt` gives you a scratch filesystem in
RAM.

**A name this boot did not find is refused by name**, which is a different
answer from "nothing recognises the filesystem on that volume" — the first
means no such device, the second means the device was found and read and no
backend claimed what was on it. Worth knowing when a mount fails: they point
at different problems.

**The mount point must already exist and be a directory**, which is Linux's
rule. Mounting onto nothing would create a path that exists only while
mounted and that no listing of the parent could show. `/boot` and `/mnt` are
created on every root at boot for exactly this (see `docs/filesystem-layout.md`).

**Mounting over a non-empty directory is allowed, and hides it** — also Unix's
rule. What was in the directory is untouched and comes back on `umount`.

Without `-t`, every backend that could claim the volume is asked to probe it
and the first one that recognises its own superblock wins. With `-t`, only that
backend is asked, and a volume it does not recognise is refused rather than
mounted as something it is not.

## What it deliberately does not do

- **No `/etc/fstab` and no `-a`.** There is no fstab on this system. What
  mounts automatically is decided in `mount_boot_auto()` — today, the boot
  disk's FAT32 ESP at `/boot`, read-only — and that is a policy in one
  function rather than a file format plus a parser.
- **No remount-in-place.** To change a mount's flags: `umount` it and mount it
  again. Two commands, no new mechanism, and no window where a filesystem is
  half-way between two sets of rules.
- **No per-filesystem option string.** Linux's fifth `mount(2)` argument is a
  free-form `data` string each filesystem parses itself. No backend here has
  options, so there is nothing for such a parser to parse.
- **It is not privileged, because this kernel has no privilege model.** Any
  process can call it. Mounting is not destructive — nothing is formatted or
  overwritten — so unlike `mkpart` there is not even a `confirm` flag standing
  in for a check. `abi/mount_abi.h` names `sys_mount()` as where a real check
  goes when uids exist.

## Limits

- At most six mounts at once (`MOUNT_MAX`).
- **A backend may be mounted once.** TFS3, FAT32 and ramfs all keep their
  state in module-level statics, so a second mount of the same backend is
  refused by name rather than silently repointing the first one. See
  `fs_ops.h`'s `max_mounts` for what raising it would take.

  **This is what stops a second TFS3 disk being mounted**, now that a second
  disk is reachable at all. The device resolves and the volume is read; the
  backend is simply already in use by the root. Enumeration did not change
  this and was not meant to — it changed whether the device could be NAMED.
- An operation naming two paths — `mv`, `ln` — is refused across a mount
  boundary. That is Unix's `EXDEV`, and `cp` is the answer.

## Examples

    mount                          # what is mounted, and how
    mount ata0p2 /boot             # the ESP, by name
    mount ahci0p1 /mnt             # a partition on the SECOND disk
    mount 2 /boot                  # the same ESP, by number, on the root's disk
    mount -r 2 /boot               # read-only (what boot does)
    mount -t ramfs none /mnt       # a scratch filesystem in RAM
    mount -t fat32 3 /mnt          # only FAT32 is asked; anything else is refused

## See also

`lsblk` (what the devices are called), `umount`, `df`, `parttable`, `mkpart`, `fsformat`, `fsck`
