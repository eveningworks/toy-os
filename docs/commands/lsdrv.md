# lsdrv

**a `/bin` program.**

**Category:** System information

## Synopsis

```
lsdrv [-v]
```

## Description

Lists the drivers **this build has**, and what each one is currently
driving.

```
/$ lsdrv
DRIVER         CLASS    DEVICES
vesafb         display  fb0
vmsvga         display  (none)
pit            clock    clock0
tsc            clock    (none)
ahci           block    (none)
xhci           usb      (none)
usb-hid        input    usb-keyboard usb-mouse
e1000          net      (none)
ata            block    ata
virtio-blk     block    (none)
```

**It answers a question none of the other listings can.** `lsblk` says
which disks are present, `lsusb` which devices are attached, `ifconfig`
which cards are configured — all of them list *devices*. A driver that
is compiled in and bound nothing appears in none of them, so "is
virtio-blk in this build?" had no answer short of reading the source.
And which driver claimed a given USB device was recorded only in the
boot log, which scrolls away.

**`(none)` is a real answer, not a gap.** `tsc clock (none)` above means
the TSC driver is in this build and is driving nothing on this machine —
which on a QEMU guest is exactly right, because the boot log says
`clocksource: tsc not offered -- no invariant TSC on this CPU`. On
hardware with an invariant TSC the same build shows `tsc clock clock0`.
That distinction is the whole point: "absent" and "present but idle" look
identical from every other command.

**`-v` names the source file** each driver registered from:

```
/$ lsdrv -v
DRIVER         CLASS    SOURCE
vesafb         display  kernel/drivers/display/vesafb.c
                          fb0
tsc            clock    kernel/arch/x86_64/clocksource_tsc.c
                          (none)
```

In a hobby OS the question after "which driver is this?" is almost
always "where is that code?". `modinfo` carries `filename:` for the same
reason. A driver registered by its class registry rather than from its
own file names the registry — `pit` reads `kernel/core/clocksource.c`,
because the PIT has no driver file of its own to declare from.

## The model

Linux answers both halves from `/sys/bus/*/drivers/<drv>/` — a directory
per driver with a symlink per bound device — and prints the per-device
half as `lspci -k`'s "Kernel driver in use". This is that, without a
filesystem: `QUERY_DRIVER`, one record per driver.

## What it is not

**Not a dispatch table.** Nothing is called through this registry — a
driver still plugs into `block_device`, `display_driver`, `clocksource`
and the rest to actually be used. Registering wrongly makes a *report*
wrong; it cannot make a device fail.

**Not a module list.** There is nothing to load or unload: every driver
here is compiled into the kernel, which is why "present but idle" needs
saying at all. `lsmod`'s question does not exist here.

**Not exhaustive by construction.** A driver appears because it declares
itself. One that forgets is invisible, and nothing yet checks for that —
`docs/roadmap.md` carries it.

## See also

[`lsblk`](lsblk.md), [`lsusb`](lsusb.md), [`lspci`](lspci.md) and
[`ifconfig`](ifconfig.md) for the devices; [`dmesg`](dmesg.md) for what
each driver said as it bound.
