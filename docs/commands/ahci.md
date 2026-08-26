# ahci

**a `/bin` program.**

**Category:** System information

## Synopsis

    ahci

## Description

The SATA host bus adapter: its version, how many ports it implements,
how many command slots it offers, whether completions arrive by
interrupt — and then the drive, and then every port.

    ahci: HBA version 1.0, 6 ports implemented, 32 command slots, IRQ-driven
      drive: "QEMU HARDDISK" on port 0
      capacity: 18874368 sectors (9.0G), LBA48
      transfers: DMA, up to 128 sectors each
      TRIM: in use -- freed blocks are discarded to the host image
      NCQ: offered by the HBA, not used (one command in flight)
      64-bit addressing: offered, not needed (DMA buffers are below 4 GiB)
      port  link       speed     signature
      0     device     1.5 Gbps  0x00000101  SATA disk  <- in use
      1     empty      --        --

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

**NCQ and 64-bit addressing are reported precisely because this driver
does not use them.** The HBA offers both; the driver issues one command
at a time in slot 0, and puts its DMA buffers below 4 GiB. Saying so
beats leaving a reader to infer the capability from the hardware's own
bits and assume it is in play.

## What it deliberately does not do

It changes nothing. There is no equivalent of `ata nodma` here, because
AHCI has no second transfer path to fall back to — the polled path is
chosen by the driver when the chipset routes it no interrupt line, not
by a tunable. To force the *legacy IDE* driver instead, put `noahci` on
the GRUB command line.

It reports the ports the driver did not claim rather than hiding them,
and it does not enumerate a port multiplier's downstream devices,
because nothing here speaks to one.

## The trap

**"No AHCI controller on this machine" is an answer, not a failure.**
The default `make run` attaches a legacy IDE disk and `VIRTIO=1`
attaches a virtio one; neither has an HBA. `make run DISK=ahci` is what
puts a SATA drive on an ICH9 controller in front of this command.

**A drive listed here is not necessarily the one carrying the root.**
`<- in use` marks the port the block device sits on; `df` names the
backend actually mounted, which is `virtio-blk` on a machine that has
both.

## See also

`ata` is the same shape for the legacy IDE path. `df` says which backend
the root filesystem is mounted from. `lspci` shows the controller as a
"SATA controller" whether or not this driver claimed it.
