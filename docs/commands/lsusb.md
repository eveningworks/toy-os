# lsusb

**a `/bin` program.**

**Category:** System information

## Synopsis

    lsusb [-v] [-D]

## Options

- `-v`, `--verbose` -- also print what the device says about itself: its
  `iManufacturer` and `iProduct` strings, its device class, and the
  interface class, subclass and protocol.
- `-D`, `--descriptors` -- hex-dump each device's configuration
  descriptor and decode it, one line per descriptor.

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

**`-D` prints the raw configuration descriptor**, as a hex dump and then
one decoded line per descriptor: the interfaces and their alternate
settings, the endpoints with their transfer type and bytes per interval,
and — for an audio device — the UAC version and each alternate's format.

That is the flag to reach for when a device is listed with `, no driver`
and you want to know why. A class driver refuses a device by walking
descriptors nothing else keeps, so the bytes are the only account of it;
they also paste straight into a KTEST fixture, which is how a device
nobody here owns gets tested at all (`sound_usb.c`'s fixture is QEMU's,
captured exactly this way).

`, no driver` after the speed means the device was enumerated and
described but nothing in this build claimed it. That is not an error —
this build binds HID boot keyboards and mice, hubs, and UAC1 audio, so a
printer or a mass-storage device is listed and idle. An audio device
listed this way has a REASON in `dmesg`: the driver names the UAC version
and every alternate setting it saw before refusing.

## What it deliberately does not do

Nothing here talks to a device. `lsusb` reads `QUERY_USB` and
`QUERY_USBDESC`, fact providers filled once during enumeration, so it
cannot hang on a misbehaving device and does not re-read descriptors —
`-D` prints the copy the kernel kept, not a fresh transfer. It also does
not decode the class section at the end of `usb.ids`; the short class
table in the program covers what this OS can actually encounter.

`-D`'s decode is a SUMMARY, not real `lsusb -v`'s full one: enough to
answer "why did no driver take this", which for an audio device means
the UAC version, the alternate settings and each one's format. Anything
it does not know is printed as a type and a length, and the hex above it
is complete either way.

There is no device tree. Devices behind a hub are enumerated and listed,
but the listing is flat — the `Port` column is the port on the device's
own parent, so two devices on different hubs can both say `Port 1`.

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
