# lsdrv

**a `/bin` program.**

**Category:** System information

## Synopsis

```
lsdrv [-a] [-v]
```

## Options

- `-a` -- list **every** driver in the build, including the ones that
  bound nothing. Without it only drivers that are actually driving
  something are shown.
- `-v` -- print the source file each driver declared itself in and its
  one-line description, with the devices it bound on a continuation
  line, instead of the single devices column.

## Description

Lists the drivers that are **actually driving something** on this
machine, and what each one has bound.

```
/$ lsdrv
DRIVER         CLASS    DEVICES
pit            clock    clock0
ata            block    ata0
vesafb         display  fb0
i8042          input    ps2-keyboard ps2-mouse
e1000          net      net-123456
```

**`-a` adds the ones that bound nothing**, which is the other half of
the question and a real answer rather than a gap:

```
/$ lsdrv -a
DRIVER         CLASS    DEVICES
tsc            clock    (none)
pit            clock    clock0
ahci           block    (none)
ata            block    ata0
virtio-blk     block    (none)
vesafb         display  fb0
i8042          input    ps2-keyboard ps2-mouse
usb-hid        input    (none)
e1000          net      net-123456
xhci           usb      (none)
virtio-rng     rng      (none)
```

The default is the everyday question -- what is running this machine --
and on a QEMU guest more than half the rows bind nothing, which buried
it. `lspci -k` makes the same choice, naming a driver only where one is
in use.

**It answers a question none of the other listings can.** `lsblk` says
which disks are present, `lsusb` which devices are attached, `ifconfig`
which cards are configured — all of them list *devices*. A driver that
is compiled in and bound nothing appears in none of them, so nothing
else can say whether virtio-blk is in this build — which is what `-a`
answers — and which driver claimed a given USB device is otherwise only
in the boot log, which scrolls away.

**`(none)` under `-a` is a real answer, not a gap.** `tsc clock (none)`
above means the TSC driver is in this build and is driving nothing on
this machine —
which on a QEMU guest is exactly right, because the boot log says
`clocksource: tsc not offered -- no invariant TSC on this CPU`. On
hardware with an invariant TSC the same build shows `tsc clock clock0`.
That distinction is the whole point: "absent" and "present but idle" look
identical from every other command. It is also why `-a` exists at all
rather than the filtering being the only behaviour: nothing else in the
system can say that a driver is *present but idle*.

When nothing at all is bound, `lsdrv` says so in a sentence and names
the count `-a` would show, rather than printing a header with no rows.

**`-v` names the source file and says what the driver is:**

```
/$ lsdrv -v
DRIVER         CLASS    SOURCE
vesafb         display  kernel/drivers/display/vesafb.c
                        VESA linear framebuffer, mode set by GRUB
                        devices: fb0
tsc            clock    kernel/arch/x86_64/clocksource_tsc.c
                        invariant TSC, calibrated against the PIT
                        devices: (none)
```

In a hobby OS the question after "which driver is this?" is almost
always "where is that code?". `modinfo` carries `filename:` and
`description:` for the same two reasons. A driver that lives in the file
of its class registry names that file — `pit` reads
`kernel/core/clocksource.c`, because the PIT source is defined there.

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

**Not something a driver can fall out of by accident.** A driver
declares itself at file scope (`DRIVER_DECLARE`, into the `.drivers`
linker section), so it is listed because it is in the *image* — not
because some line of its `init()` was reached. `tools/check_drivers.py`
fails the build on a driver file carrying neither a declaration nor a
`driver-none: <reason>` comment.

**Still not exhaustive against a driver that binds without declaring.**
That records the binding under class `?` and logs a line naming the
driver, rather than dropping the fact — a binding nobody can see is what
this exists to fix.

## See also

[`lsblk`](lsblk.md), [`lsusb`](lsusb.md), [`lspci`](lspci.md) and
[`ifconfig`](ifconfig.md) for the devices; [`dmesg`](dmesg.md) for what
each driver said as it bound.
