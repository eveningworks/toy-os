# mkpart

**a `/bin` program.**

**Category:** Storage

## Synopsis

    mkpart [--disk <name>] [--mbr|--gpt] <size>[K|M|G]|rest [<size>|rest ...] confirm
      --disk <name>  which disk (`lsblk`); default is the one this machine booted
      --gpt          write a GPT (default)
      --mbr          write a legacy MBR -- at most 4 partitions
      <size>         one partition of that size; `rest` takes what is left
      bios:|esp:     prefix a size to type it -- a BIOS boot partition for
                     GRUB's core.img, or an ESP holding the kernel (/boot)
      confirm        required -- this destroys the disk's current contents

## Description

`/bin/mkpart` writes a partition table to a disk. It is the write half of
`parttable`, which reads one.

**Which disk.** With no `--disk`, the one this machine booted from — which is
all this command could reach at all until 2026-08-31, and is why the default is
that rather than "the first one". `--disk` takes a name `lsblk` prints
(`ata1`, `virtio0`); a **partition** name is refused, because a table written
inside a partition describes windows into itself.

Sizes are laid out end to end from LBA 2048 (the 1 MiB alignment every modern
tool uses), in the order given, with no gaps and no reordering — what you type
is what lands, which is the only layout predictable from a command line. A bare
number is **sectors**; a `K`/`M`/`G` suffix is bytes, rounded **down** to a
whole sector. Exactly one partition may be `rest`, which takes everything left
after the sized ones.

**Typing a partition.** A size may carry a `bios:` or `esp:` prefix, which
writes the GPT type GUID the boot scan recognises — a BIOS boot partition
(`21686148-…`, where GRUB's `core.img` goes, no filesystem in it) or an EFI
System Partition (`C12A7328-…`, which boot mounts at `/boot`). A **role** rather
than a raw GUID because a caller stating sixteen bytes can state any sixteen,
and the scan's "this is the firmware's, never a root" test only works when what
`mkpart` writes is something it recognises. Everything else is basic data.

A GPT gets the full standard geometry: a protective MBR at LBA 0, a primary
header at LBA 1, 128 entry slots across LBAs 2–33, and a backup header and
entry array in the last 33 sectors. Only the first four slots are ever filled
here — see the limits below — but the array is full-sized, so a partition
editor on another system can add to a table this wrote.

## What it deliberately does not do

- **It does not format anything.** A partition is a range of sectors; putting a
  filesystem in one is `mkfs` (a named partition) or `fsformat` (the volume this
  machine is running from). Separate verbs because they are separate decisions,
  the same split as `fdisk` and `mkfs`.
- **It does not remount a disk the machine is running from.** On the boot disk,
  or any disk something is mounted from, the new table takes effect at the
  **next boot** — Linux is the same, and its kernel likewise refuses to re-read
  a table on a busy disk. On a disk nothing is mounted from, the kernel *does*
  re-read it, so `<disk>p<n>` become devices immediately (Linux's `BLKRRPART`).
  That is what lets one program partition, format and mount a target without a
  reboot in between.
- **There is no interactive mode.** `fdisk`'s prompt-driven editor is a lot of
  program for a machine with one disk, and a command line can be read back
  later and driven by a test.
- **No extended/logical MBR partitions.** They are a linked list of sectors
  scattered through the disk, and GPT is the answer to wanting more than four.

## Limits

At most **four** partitions, which is MBR's hard limit and enough for a disk
this size; `--mbr` refuses a fifth outright. MBR start/length fields are 32-bit,
so `mkpart` refuses rather than truncates a partition that would not fit in one
— a truncated start LBA is a partition somewhere else entirely.

The kernel validates independently of this program and refuses a table whose
partitions overlap each other, run past the end of the disk, or sit on top of
the table's own reserved sectors.

## The trap

**`confirm` is only demanded for a disk in USE.** The word is required when the
target is the boot disk or carries a mount, and not when it is a second disk
nothing is running from — making somebody type it for a disk the command is not
touching is how the word stops meaning anything. A zeroed request names no
disk, which is the boot disk, so the accident case is still covered.

**And `confirm` is a speed bump, not a permission check.** toy-os has no privilege
model — there is no uid, and `SYS_QUERY` has none either — so any process can
set the flag that `confirm` sets. What it stops is the accident, not the
attacker. If uids ever arrive, `sys_mkpart()` in
`kernel/drivers/partition_syscall.c` is where the real check goes.

**A partitioned disk is never auto-formatted.** Once a table exists, boot no
longer treats an unclaimed disk as blank — it would otherwise lay a whole-disk
filesystem across every partition the table describes, and TFS3 would *survive*
that (it reserves volume blocks 0–7, so the table stays readable) while the
partitions were being overwritten. Instead, boot leaves the first partition
that is OURS as the active volume so that `fsformat` claims it.

**"Ours" excludes the firmware's.** A GPT BIOS boot partition or an EFI
System Partition is skipped by the scan entirely — the stock `disk.img` begins
with both (GRUB's `core.img`, then the FAT32 `/boot` holding the kernel), and
pointing `fsformat` at either would destroy the machine's ability to boot.
Real installers make the same distinction; an ESP is never a root filesystem
candidate.

## Typical use

    mkpart --gpt 64M rest confirm     # two partitions, on the boot disk
    reboot
    fsformat tfs3 confirm             # a filesystem in the active partition
    df                                # reports the PARTITION's size, not the disk's

A second disk needs no reboot, because nothing is mounted from it:

    lsblk                             # find its name
    mkpart --disk ata1 --gpt 64M rest confirm
    mkfs ata1p2 confirm               # ata1p1/ata1p2 exist already
    mount ata1p2 /mnt

## See also

`parttable` (read a table), `fsformat` (put a filesystem in the active volume),
`df` (what is mounted, and how big it is).
