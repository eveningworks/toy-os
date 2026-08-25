# lsusb

**a `/bin` program.**

**Category:** System information

## Synopsis

    lsusb [-v]

## Description

The USB devices the xHCI driver enumerated at boot, one paragraph each:
the root port, the xHCI slot id, the vendor and product ids, the speed,
and the interface class.

Names come from `/usr/share/hwdata/usb.ids`, a verbatim copy of the USB
ID Database, seeded onto the disk image at build time — the same file
and the same path a Linux distribution's `lsusb` reads. It is streamed a
kilobyte at a time rather than loaded, because it is ~730 KB and this
process's heap is a bump allocator with no free. Without the file the
numeric ids still print, with a note on stderr.

**`-v` shows what the DEVICE says about itself**, which is the half the
database cannot give: USB devices carry Manufacturer and Product string
descriptors, and PCI devices have no equivalent. The two disagree more
often than you would expect, and QEMU is a good example — every one of
its HID devices reports the id `0627:0001`, which the database names
"QEMU Tablet", while the device's own `iProduct` string correctly says
"QEMU USB Keyboard" or "QEMU USB Mouse". Real `lsusb` prints the
database name by default too, so this matches it rather than being
cleverer than it.

`, no driver` after the speed means the device was enumerated and
described but nothing in this build claimed it. That is not an error —
this build has a HID boot keyboard and mouse driver and nothing else, so
a printer or a mass-storage device is listed and idle.

## What it deliberately does not do

Nothing here talks to a device. `lsusb` reads `QUERY_USB`, a fact
provider filled once during enumeration, so it cannot hang on a
misbehaving device and does not re-read descriptors. It also does not
decode the class section at the end of `usb.ids`; the short class table
in the program covers what this OS can actually encounter.

There is no device tree, because there are no hubs — every device here
is on a root port.

## The trap

**"No USB devices" is not "no USB controller".** A machine booted
without `USB=xhci` has no controller at all, and one with a controller
and nothing plugged into it has no devices; both print the same line
here. `lsdev` is what distinguishes them — it names the controller, its
port count and whether it is interrupt-driven.

## See also

`lsdev` (the serial debug console) names the controller and every
registered input source. `usb`, also on the debug console, dumps the
controller's registers, both rings and the per-endpoint state.
