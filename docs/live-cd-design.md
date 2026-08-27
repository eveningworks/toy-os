# Booting from the ISO alone -- a Live CD for toy-os

**Status: BUILT 2026-08-16** -- stages A, B and C all landed in one
pass; see "What actually shipped" at the bottom. The design below stands
as written, with two corrections recorded there.

Originally written as: **DESIGN, not implemented.** Written the way
`docs/wm-ring3-design.md`, `docs/uapp-design.md` and `docs/tfs3-design.md`
were: settle the shape and the arguments first, build it in named stages
afterwards. (That sentence read "nothing below is built yet" until the
day it all was -- kept, because the design is what it is because it was
written before the code.)

**The one-sentence version:** the ISO carries a small TFS3 image as a
GRUB module, the VFS gains a RAM block device, and the existing tfs3
backend mounts it -- so a live boot runs the same filesystem code a disk
boot does, and "no disk" stops meaning "no `/bin`".

**The disk keeps winning.** This does not change what `make run` does.
A live boot happens when the ISO is booted with no disk at all, or when
the user picks the live entry from the GRUB menu on purpose. A real disk
present and unselected is mounted exactly as it is today.

Decisions settled deliberately rather than defaulted, each with a
section below:

- **The module is a FILESYSTEM IMAGE, not an archive.** No second
  on-disk format, no unpack step, and every filesystem path stays the
  one that is already tested.
- **The indirection is a `block_device` registry**, the same
  one-struct-of-function-pointers pattern `display_driver` and `fs_ops`
  already use here -- not a special case inside tfs3.c.
- **Only TFS3 gets the indirection.** TFS2 keeps talking to ATA
  directly, because a live image is always TFS3 and rewiring a legacy
  backend to serve a feature it will never carry is cost with no
  return.
- **Writes go to RAM and are LOST at power off**, and the system already
  says so (`fs_is_persistent()`). No copy-on-write onto the disk, no
  overlay -- that is a different feature and it is out of scope below.
- **Kernel ASLR must learn about modules before any of this is safe.**
  Today it can relocate the kernel on top of one.

## What already exists, measured

The reason this is tractable is that most of the parts are in the tree
already. Check these before assuming anything needs building:

- **GRUB modules are already readable.** `multiboot_get_module()`
  (`kernel/core/multiboot.c`) walks the tag list and hands back a
  module's `[start, end)`. It survives from the old `BIN_BOOTSTRAP`
  path, which seeded `/bin` from GRUB modules and was retired in favour
  of host-side seeding (see `docs/decisions.md`) -- the loader was kept,
  the policy was not.
- **The page allocator already protects module memory.** `pmm.c`'s init
  reserves every module's range (`multiboot_get_module()` in a loop),
  and the comment there says outright that it is kept against the day a
  `module2` line comes back. So a module cannot be handed out by
  `pmm_alloc_frame()`.
- **The VFS already has a no-disk path.** `vfs.c` degrades to a
  RAM-only filesystem when there is no disk, and `fs_is_persistent()`
  already reports that honestly (`about`, `df` and `fsck` all print it).

  **The predicate said `ata_present()` when this was written, and that
  was a bug for as long as it stood** (fixed 2026-08-27): it is the
  LEGACY IDE probe, so on a machine whose only disk is AHCI or virtio
  the kernel concluded "no disk" and a live image displaced the real one.
  It is `blk_present()` now, asked by the CALLER — the `live`-on-the-
  command-line half has to be answered BEFORE the disk drivers run, and
  the is-there-a-disk half cannot be answered until after. What that path lacks is CONTENT: it comes up
  with an empty filesystem, so there is no `/bin` and nothing to run.
- **TFS3's block I/O is already one indirection wide.** Every read and
  write in `kernel/fs/tfs3.c` goes through `vol_read_sectors()` /
  `vol_write_sectors()`, which exist for partition support (`g_vol.
  base_lba` / `sector_count`). That is two functions to repoint, not a
  file to rewrite -- **TFS2 by contrast makes 24 direct `ata_*` calls**,
  which is most of why this design leaves it alone.
- **The image builder is host-side and already correct.**
  `tools/tfs3_writer.py` formats, syncs a seed tree and trims; the
  Makefile's `seed` target already calls it through
  `tools/seed_disk.py`. A live image is that tool pointed at a smaller
  file.

## What is genuinely missing

Four things, and the last one is a blocker rather than a feature.

**1. A block-device abstraction.** TFS3 reaches ATA for more than the
two funnels: `ata_present()`, `ata_sector_count()`,
`ata_max_sectors_per_xfer()`, `ata_flush_now()`, `ata_trim()` and
`ata_trim_supported()`. All six are things a RAM device answers
differently (no flush, no TRIM, a size it was told). They become slots
on one struct.

**2. A RAM block device.** Reads and writes served by `k_memcpy` from
the module's frames. It is the simplest driver in the tree and should
stay that way.

**3. Boot policy, and a way to ask for it.** Which of "a disk", "a
module" and "an empty RAM filesystem" gets mounted, decided in one place
in `vfs.c`, plus the GRUB entry and the kernel command-line flag that
let a user choose rather than infer.

**4. Kernel ASLR does not know modules exist -- and this is a blocker.**
`slot_usable()` (`kernel/arch/x86_64/reloc.c`) rejects a candidate base
that would overlap the old image or the multiboot INFO structure. It
does **not** consider module ranges, because there have been no modules
since the day it was written. Put a 16 MiB module in low memory and the
relocation can copy the kernel straight over it.

The failure mode is the worst kind: it depends on the random base, so it
reproduces on some boots and not others, and `nokaslr` makes it go away
entirely -- which reads as "the ASLR code is broken" rather than "the
module was eaten". `pmm.c` reserving the range does not help, because
the relocation runs long before the PMM exists. **Stage A fixes this
first**, and it is a five-line change to `slot_usable()` plus a test.

## Why a filesystem image rather than an archive

The obvious alternative is a small TAR-ish blob the kernel walks at
boot, creating each file through `fs_write()`. It is less code on day
one and it was rejected anyway:

- **It is a second on-disk format**, with its own writer, its own
  parser, its own corruption cases and its own tests -- to carry data
  the project already has a format for.
- **It re-seeds through the write path on every boot.** Creating a few
  hundred files through the journal on each power-on is slower than
  mounting, and it exercises the write path at boot on a filesystem
  nobody asked to write to.
- **It splits the code paths.** A live boot would run "unpack, then use
  a RAM filesystem"; a disk boot runs "mount". Two ways to arrive at a
  usable `/` is two ways for one of them to rot -- and the one that rots
  is the one nobody tests, which here would be the live one.

Mounting an image instead means the live path IS the disk path with a
different device underneath, which is both less code and a stronger
statement about correctness. It is also what a real live image does
(squashfs on a loop device), and the analogy is worth keeping since it
is where an overlay would later attach.

The cost is honest and small: a block-device indirection TFS3 is already
most of the way to, and a RAM device that is a `k_memcpy` each way.

## Why the writes are lost, and why that is fine

A live volume is writable -- `/tmp` has to work, `ensure_layout()` runs
on every mount, and a shell that cannot write anything is a demo rather
than a system. Those writes land in the module's own frames and die with
the power.

That is what a live CD is, and the system already tells the truth about
it: `fs_is_persistent()` returns 0 on this path exactly as it does on a
ramfs root, so `df`, `fsck` and the About window all report "RAM, not
persistent" with no new plumbing. (When this was written the comparison
was "today's empty RAM-only boot", which turned out to be a boot with no
filesystem at all -- that is `ramfs` now, and the live image is
PARTITIONED like every other volume this OS mounts. See
`docs/rootfs-design.md`.) **Nothing should
special-case a live mount to look persistent**, and nothing should
quietly write through to a disk that happens to be present -- a live
session that mutates the user's installed system is the single worst
thing this feature could do.

Persistence, if it is ever wanted, is an overlay (a writable RAM layer
over a read-only base, with union lookup). That is a real feature with
real design questions and it is listed as out of scope rather than
half-built here.

## The device abstraction

One struct, registered like everything else that has plural
implementations in this kernel:

```c
struct block_device {
    const char *name;                  // "ata", "ram" -- what `df` prints
    uint32_t (*sector_count)(void);
    int  (*read_sectors)(uint32_t lba, int count, void *buf);
    int  (*write_sectors)(uint32_t lba, int count, const void *buf);
    int  (*max_sectors_per_xfer)(void);
    // Optional, behind capability bits -- the display_driver rule: a
    // device that needs a flush and does not get one is a corruption
    // bug, and a capability that disagrees with its function pointer is
    // refused at registration rather than discovered at runtime.
    unsigned caps;                     // BLK_CAP_FLUSH | BLK_CAP_TRIM
    void (*flush)(void);
    int  (*trim)(uint32_t lba, uint32_t count);
};
```

Two rules carried over from `display_driver` because both have already
paid for themselves here:

- **Capabilities are declared, not discovered**, and the registry
  refuses a device whose bits and pointers disagree.
- **A missing optional op is a normal answer, not an error.** TFS3's
  `ata_flush_now()` calls become `blk_flush()`, which is a no-op on a
  device with no cache -- and TFS3's journal barriers are still correct,
  because a RAM device has nothing that can be lost independently of
  everything else.

`fs_is_persistent()` becomes a property of the DEVICE rather than of
"was there a disk", which is what makes the live mount honest for free.

## Staging

Each stage is a commit that builds, tests and stands on its own -- the
pattern TFS3, uapp and the WM migration all used.

### Stage A -- ASLR learns about modules

The blocker, alone, so it lands verifiable rather than buried in a
feature.

- `slot_usable()` rejects a base overlapping any module range, the same
  way it already rejects the multiboot info structure.
- A KTEST asserting no module range intersects the running image.

**Proven by:** booting with a deliberately large dummy module many
times. The positive control is the point -- without the fix, some boots
must fail; a fix that cannot be shown to matter is a fix nobody can
trust. Note the check has to be done in `reloc.c`'s own terms (it runs
before the PMM, before serial, and cannot log), so the test asserts the
outcome, not the decision.

### Stage B -- the block-device registry

- `kernel/include/kernel/block.h` plus `kernel/drivers/block/` holding
  the registry and the ATA implementation.
- TFS3's two funnels and six direct `ata_*` calls go through it.
- **No behaviour change**: ATA is the only device, registered at boot,
  and `fs_switch_test.py` plus the TFS3 KTESTs must pass untouched.
  (True when this stage was written and no longer: `block.h` has three
  implementations now -- ATA, a RAM image, and virtio-blk, which is the
  PREFERRED disk when one is attached. The indirection this stage
  introduced is exactly what made that a new file rather than a
  rewrite, which is the outcome it was arguing for.)

**Proven by:** the existing filesystem suite, unchanged. This stage is
pure indirection, so anything that goes red is a mistake in it.

### Stage C -- the RAM device and the module

- A RAM block device over a `[base, size)` handed to it at init.
- `vfs.c` learns the boot policy: a live module is mounted when there is
  no disk, or when the command line asks for it.

  **BUILT, and with one correction to what is written above:** the disk
  drivers all run either way now, so a live session SEES the machine's
  disks (`lsblk`) and can mount them — it simply does not take its root
  from one. Skipping disk init on a forced live boot, which is what this
  originally did, made a live CD unable to touch the disks it exists to
  rescue.
- `grub.cfg` gains a `module2 /boot/live.img` line and a second menu
  entry; the Makefile builds `live.img` with `tfs3_writer.py` from the
  same seed tree `disk.img` uses, sized to fit rather than 9 GB sparse.

**Proven by:** a new `tools/live_boot_test.py` -- boot the ISO with **no
`-drive` at all**, then assert `/bin/ls` runs, a file written to `/tmp`
reads back, and `df` says the volume is not persistent. That last check
is the one that stops a future change from quietly writing to a disk.

### Stage D -- the details that make it a feature rather than a demo

- `df` and `about` name the device (`ram` vs `ata`).
- The GRUB menu entry is worth having even with a disk present, which
  means `make run` gains a `LIVE=1` axis.
- `tools/run_release.sh` learns to boot the ISO alone, which is the
  whole point for someone downloading a release: today it needs the
  gzipped disk image as well.

## Testing

- **`tools/live_boot_test.py`** (new) -- the ISO, no disk, asserted over
  the serial console. It is the only test that can catch "the live path
  silently fell back to an empty filesystem", which otherwise looks
  exactly like success right up until you type `ls`.
- **`fs_switch_test.py` and the TFS3 KTESTs, unchanged** -- stage B's
  whole claim is that they do not move.
- **Boot the live ISO repeatedly** for stage A. One clean boot proves
  nothing when the bug is a random base.

**Ask what a broken version would still pass.** A live boot that mounted
nothing still shows a desktop -- the WM is compiled into the kernel -- so
"it booted" and "there is a screenshot" are both worthless as evidence
here. The assertion has to be that a file the ISO shipped can be read.

## Out of scope

- **A writable overlay / persistence.** A RAM layer over a read-only
  base with union lookup is a real feature; conflating it with this one
  is how the live path ends up able to mutate an installed system.
- **Compression.** A squashfs equivalent would make the image smaller
  and is entirely separate from where it is mounted from.
- **Installing to disk from a live session.** Wants a partitioner and a
  copy tool; `parttable` already parses tables, nothing writes them.
- **Booting the live image on real hardware from USB.** Should follow,
  but El Torito/hybrid-ISO layout is a `grub-mkrescue` question rather
  than a kernel one.
- **TFS2 live images.** The legacy backend keeps its direct ATA calls;
  a live image is always the default format.

## Revision history

- 2026-08-16: written, after Milestone 41's stage 0 landed. Prompted by
  "could the binaries live in the ISO so it runs without the disk, like
  a Live CD?" -- the answer being that the loader, the module memory
  reservation and the no-disk mount path all already exist, and what is
  missing is a block-device indirection TFS3 is two functions away from,
  plus one genuine blocker in kernel ASLR.


## What actually shipped (2026-08-16)

All of stages A, B and C, verified by `tools/live_boot_test.py`: the ISO
boots with **no `-drive` at all** and comes up with a real `/bin` and
`/usr`, a binary from the image runs, and `df` says
`tfs3 (RAM-only -- won't survive reboot)`.

**Stage A found exactly what this document predicted.** `slot_usable()`
weighed the old image and the multiboot info structure and knew nothing
about modules, so kernel ASLR could relocate the kernel on top of the
live image. Fixed, with a KTEST that asserts no module range intersects
the running image -- and which SKIPS with a reason when no modules are
loaded, because a green tick on a boot with nothing to hit would be a
check that cannot fail.

**Two corrections to the design above.**

1. **Persistence is a field on the device, not a capability bit.** The
   design said `fs_is_persistent()` becomes a property of the device and
   left it there; in practice the VFS computes
   `fs->init() && blk_persistent()`, because a backend genuinely cannot
   tell -- TFS3 mounts a RAM image exactly as it mounts a disk. Getting
   this wrong would have had a live session tell the user their files
   were saved.

2. **The live image cannot be small.** The design assumed an image
   "sized to fit" the seed tree. TFS3 puts 32768 blocks in a group
   (`T3_BPG`, 128 MiB at a 4 KiB block) and the kernel REFUSES a
   superblock whose blocks-per-group differs, so one group is the
   smallest volume the format allows. The live image is therefore 129
   MiB of which ~4 MiB is used, and the ISO is ~162 MiB rather than the
   ~20 MiB it was.

   That is the honest cost and it is worth writing down rather than
   hiding: it costs ISO size and boot-time RAM and nothing else. The fix
   is to let `bpg` vary for small volumes -- it is already a superblock
   FIELD, just range-checked to a single value in both the kernel and
   `tfs3_writer.py` -- which is a change to a tested filesystem's
   geometry validation and deserves its own pass rather than being
   smuggled into this one.

**What did NOT need doing.** The block-device indirection was smaller
than costed: TFS3 already funnelled every read and write through
`vol_read_sectors()`/`vol_write_sectors()`, so the whole backend moved
in 15 call-site substitutions plus one include. TFS2 kept its 24 direct
`ata_*` calls exactly as planned.

**Still out of scope, unchanged:** a writable overlay, compression,
installing to disk from a live session, and USB-bootable hybrid ISO
layout.
