# lscodec

**a `/bin` program.**

**Category:** System information

## Synopsis

```
lscodec [-v] [-d INDEX]
```

## Options

- `-v` -- one line per widget: its nid, type, capability word, and for
  a pin complex its pin capabilities, default configuration and
  connection list. Without it only the summary and the routes are
  printed.
- `-d INDEX` -- drive the PCI device at this enumeration index (the one
  [`lspci`](lspci.md) counts and [`lspci -k`](lspci.md) reports against)
  instead of the first HD Audio controller found.

## Description

Reads the **codec graph** of an HD Audio controller: what the card says
about itself, and which analog output the driver would route.

It is the first ring-3 driver in this tree. It takes the controller off
the kernel (`SYS_DEV_CLAIM`), maps its registers (`SYS_DEV_MAP_BAR`),
takes a DMA buffer for its command ring (`SYS_DEV_DMA_ALLOC`), brings
the controller out of reset, drives the CORB and RIRB itself, walks the
graph, and hands the controller back. `docs/umdf-design.md` stage 3 is
the plan it implements and why.

```
/$ spawn /bin/lscodec
lscodec: pci 4 00:1b.0 8086:9ca0 claimed
lscodec: bar0 at 40000000, command rings at 40001000 (phys 1a2000)
lscodec: hd audio 1.0 gcap 0x4401 (4 in, 4 out) codecs 0x1
codec 0: vendor 14f1:5751
  afg nid 01, 26 widget(s)
  speaker route: 1a -> 0b -> 02  (pin 1a dac 02)
  speaker volume: nid 02, 74 steps of 0.75 dB, 0 dB at 57, mute-capable
  headphone route: 19 -> 0c -> 03  (pin 19 dac 03)
  headphone volume: nid 03, 74 steps of 0.75 dB, 0 dB at 57, mute-capable
lscodec: 1 codec(s), releasing pci 4 back to the kernel
```

**IT NEEDS `spawn`, NOT `run`.** The legacy loader has no scheduler
slot, so the claim comes back `EPERM` -- as it does for anything built
on `SYS_DEV_CLAIM`.

**It takes the sound card away while it runs**, which on a machine with
one controller means a few hundred milliseconds with no audio device,
and the kernel re-probing it afterwards. Anything playing at the time
loses its stream.

**On a machine with two controllers the default is the safe one.** It
claims the FIRST class-04:03 device, which on a laptop with display
audio is the HDMI/DisplayPort controller -- no analog output, no
speakers, so nothing it does can silence the machine. `-d` is how to
point it at the other one deliberately.

**"no analog output" is an answer, not a failure.** A display-audio
codec's pins are all digital, so there is no route to find, and the
kernel's own `hda` driver says the same thing about the same card
before declining to register it.

## What it deliberately does not do

**It does not configure anything.** Every verb it sends is a read, bar
the one that powers the function group up so its widgets will answer.
Routing, amplifiers, pin control and the stream stay in ring 0, in
`kernel/drivers/sound/hda.c` -- that is the half that makes sound, and
moving it needs stages 4 and 5 of the design.

**It is not a second parser.** `kernel/lib/hda_codec.c` is compiled
twice, into the kernel and into this binary, so what it prints is what
the kernel's driver sees -- the `geom.c`/`klineedit.c` rule. A
difference between the two would be a bug in the transport, never in
the walk.

**It cannot be the reason sound stops working.** Every exit path
releases with `DEV_RELEASE_REBIND`, which re-probes the device. If the
process is KILLED instead, the claim still drops but the device is
deliberately left unbound -- run `lscodec` again and let it finish, and
the rebind on its way out puts the kernel driver back.

## See also

[`lspci`](lspci.md) for the controller and, with `-k`, whether a device
is claimable at all; [`lsdrv`](lsdrv.md) for what `hda` is driving;
[`dmesg`](dmesg.md) for the kernel's own view of the claim, the DMA
grant and the re-probe; [`aplay`](aplay.md) to check sound still works
afterwards.
