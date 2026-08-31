# install

**a `/bin` program.**

**Category:** Storage

## Synopsis

    install [--disk <name>] [--esp <MiB>] confirm
      --disk <name>  the target (`lsblk`); refuses the one this machine runs from
      --esp <MiB>    size of the FAT32 /boot partition (default 64)
      confirm        required -- this ERASES the target disk

## Description

`/bin/install` puts the running system onto another disk and makes that disk
boot. Five steps, each of which is an existing command's operation done through
the same syscall that command uses:

| step | what it is |
| --- | --- |
| partition | `SYS_MKPART` — the same call `mkpart --disk` makes |
| format | `SYS_MKFS` — twice, FAT32 then TFS3 |
| mount | `SYS_MOUNT` — the target's root at `/mnt`, its ESP at `/mnt/boot` |
| copy | `ufileop`, the library `cp -r` and the File Manager share |
| bootloader | `SYS_INSTALL_BOOT` — the boot sector and GRUB's core image |

The four files it writes onto the target's boot partitions come from
**`/install`** — `kernel.bin`, `grub.cfg`, `boot.img`, `core.img` — which every
toy-os filesystem carries.

Nothing here is a special path into the kernel. An installer that needed one
would be an installer whose steps could not be checked by hand — every one of
these can be typed at a shell and watched.

The layout it writes:

| | size | type | holds |
| --- | --- | --- | --- |
| `p1` | 1 MiB | BIOS boot | GRUB's `core.img`, no filesystem |
| `p2` | `--esp` (64 MiB) | ESP, FAT32 | the kernel, `grub.cfg`, GRUB's modules |
| `p3` | the rest | TFS3 | the root |

## Installing from live media

**This is the path a real machine uses**, and it is why `/install` exists. Write
`toy-os-live.iso` to a stick or a CD, boot it, and the root is a filesystem
image GRUB loaded into RAM — the machine's own disks are enumerated but nothing
is mounted from them, so `install --disk <name>` can erase and rewrite the one
you point it at.

`/boot` is **empty** in a live session: there is no ESP, because nothing drives
the boot medium once GRUB has loaded the kernel and the image (toy-os has no
USB mass-storage driver). That is exactly why the payload travels in the root
rather than being read out of `/boot` — one path on every medium.

## What it deliberately does not do

- **It does not install onto the disk this machine is running from,** and
  refuses by name. There is no reading of "reinstall over myself" that ends
  with a working machine.
- **It does not ask anything.** No partitioner, no package selection, no
  hostname prompt — one command line, so a test can drive it and a person can
  read back what they typed.
- **It does not resize or preserve anything on the target.** The whole disk
  becomes the layout above.
- **It does not generate a new `core.img`.** See the trap.
- **It does not copy GRUB's module directory** (305 files, ~4 MB). `core.img`
  already contains every module `grub.cfg`'s `insmod` asks for, so the target
  boots without it; what that target cannot do is have a *host* `grub-install`
  run against it later without re-copying them.

## The trap

**The layout is not a preference, it is a constraint.** `core.img` carries its
prefix baked in at the host's `grub-mkimage` time — `(hd0,gpt2)/boot/grub` — so
the copy this installs only finds its config if the ESP is **partition 2** on
the target as well. Anaconda and the Debian installer generate a fresh core
image per target instead; toy-os has no `grub-mkimage` of its own, so it
reproduces the layout that the image it copies expects. Changing the partition
order here without rebuilding `core.img` produces a disk that reaches GRUB and
stops at a rescue prompt.

**`/install/grub.cfg` is written verbatim, so any `KCMDLINE` baked into this
build is baked into the target too.** A system built with `make iso
KCMDLINE="root=ata0p3"` installs a target that also insists on `ata0p3` — which
is right if the target's root really is its third partition (it is) and wrong
the moment that disk is attached as something other than `ata0`.

**`confirm` is a speed bump, not a permission check** — the same caveat
`mkpart` and `mkfs` carry, and for the same reason: this OS has no privilege
model.

## Limits

- **The target must be a whole disk**, not a partition.
- **A build made without GRUB's BIOS target carries no `/install`**, and says so
  before erasing anything. Such a build boots perfectly; it just cannot install
  itself.
- **`/tmp` is not copied**, and `/boot` and `/mnt` are recreated as empty mount
  points rather than copied as directories — `/boot` because it is the
  *source's* ESP and goes to the target's separately, `/mnt` because it is
  where the target itself is mounted.
- **A file that fails to copy does not stop the rest**, but the install as a
  whole then reports failure and says the target is in an unknown state.

## Typical use

    lsblk                              # find the target
    install --disk virtio0             # prints the plan, changes nothing
    install --disk virtio0 confirm     # ~30 seconds
    poweroff

Then boot that disk on its own. From live media it is the same three lines,
with the machine's own disk as the target.

## See also

`mkpart` (partition a disk), `mkfs` (put a filesystem on one partition),
`mount`, `lsblk` (what disks exist), `fsformat` (reformat the RUNNING root).
