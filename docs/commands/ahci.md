# ahci

**a `/bin` program.**

**Category:** System information

## Synopsis

    ahci

## Description

Every SATA host bus adapter: its version, how many ports it implements,
how many command slots it offers, whether completions arrive by
interrupt -- then each drive on it, and then every port.

    ahci: HBA version 1.3, 1 port implemented, 32 command slots, IRQ-driven
      NCQ: offered; 64-bit addressing: offered, used for a queued buffer above 4 GiB
      ahci0: "SAMSUNG MZNLN128HAHQ-000H1" on port 0
        capacity: 250069680 sectors (119.2G), LBA48
        transfers: DMA, up to 512 sectors each, IRQ-driven
        waits that slept: 103685
        TRIM: in use -- freed blocks are discarded to the host image
        NCQ: 32 tags; 135 batch(es) of 272 commands queued, 0 replayed one at a time
      port  link       speed     signature
      0     device     6 Gbps    0x00000101  SATA disk  <- ahci0

(The ASUS test laptop. QEMU's `ich9-ahci` reports six ports, five of them
`empty`, and a 1.5 Gbps link.) **Every SATA disk is driven**, on every
controller: a second drive is `ahci1`, a second HBA prints its own block
after a blank line, and `<- ahciN` names the block device each port's
drive became -- the name `lsblk`, `mount` and `root=` use.

**The port table is the point.** "No drive" and "a drive this driver
cannot speak to" both leave `df` reporting the same thing, and the
signature is what tells them apart: an ATAPI optical drive, an enclosure
service or a port multiplier all report a device on the link and none of
them is a disk. `link only` means DET is non-zero but not 3 — a link
that came up partially, which is a cabling answer rather than a driver
one.

**TRIM is stated rather than left as a flag**, the same way `ata` does
it: TRIM is about what happens to the HOST IMAGE, not to throughput.
With it in use a deleted file's blocks are actually released and
`disk.img` shrinks; without it the image only ever grows. It comes from
IDENTIFY word 169, read at registration.

**NCQ is used for a BATCH and nothing else.** A batch is several
independent transfers handed over together (`blkdev_submit_batch()`, the
filesystem's journal commit today); the driver queues them on up to the
tag count shown, each DMAing straight into its caller's buffer, and
waits for all of them. One request at a time -- almost everything --
still goes out alone in slot 0. The counts say how often the queue was
used, and `replayed one at a time` is how many batches the drive refused
and the driver redid command by command. `noncq` on the boot line turns
the batch path off (`docs/boot-flags.md`). A queued buffer above 4 GiB
is addressed with 64-bit PRDs when the HBA offers them.

## What it deliberately does not do

It changes nothing. There is no equivalent of `ata nodma` here, because
AHCI has no second transfer path to fall back to — the polled path is
chosen by the driver when the chipset routes it no interrupt line, not
by a tunable. To force the *legacy IDE* driver instead, put `noahci` on
the GRUB command line.

It reports the ports without a disk rather than hiding them,
and it does not enumerate a port multiplier's downstream devices,
because nothing here speaks to one.

## The trap

**"No AHCI controller on this machine" is an answer, not a failure.**
The default `make run` attaches a legacy IDE disk and `VIRTIO=1`
attaches a virtio one; neither has an HBA. `make run DISK=ahci` is what
puts a SATA drive on an ICH9 controller in front of this command.

**A drive listed here is not necessarily the one carrying the root.**
`<- ahci0` names the block device a port's drive became; which disk
carries the root is `lsblk`'s answer -- the disk GRUB booted from
(`bootpart=` in `docs/boot-flags.md`), not the first drive.

**`waits that slept`** counts command waits that parked their caller so something else could run -- a process's disk wait sleeps on the controller's interrupt, INTx or MSI. The kernel context still halts or polls, so a count that stays at 0 while a program writes means the sleep path is not being reached.

## See also

`ata` is the same shape for the legacy IDE path. `df` says which backend
the root filesystem is mounted from. `lspci` shows the controller as a
"SATA controller" whether or not this driver claimed it.
