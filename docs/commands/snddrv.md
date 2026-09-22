# snddrv

**a `/bin` program.**

**Category:** System information

## Synopsis

```
snddrv [-d INDEX] [--driver NAME] [--usb-id VID:PID] [--usb-clock ID] [-v]
```

## Options

- `-d INDEX` -- drive the PCI device at this enumeration index (the one
  [`lspci`](lspci.md) counts) instead of searching every device.
- `--driver NAME` -- load only the plugin with this name (`hda`,
  `ac97`), instead of every `.so` in `/lib/snd`.
- `--usb-id VID:PID` -- for a driver that finds its own device on a bus
  this host cannot enumerate, drive the one with these ids, as
  [`lsusb`](lsusb.md) prints them (`--usb-id 0b05:19a8`). A bare decimal
  is an xHCI slot instead, which is what the logs name and the only way
  to tell two identical DACs apart.
- `--usb-clock ID` -- pin a UAC2 device's clock to this entity instead
  of asking its clock SELECTOR which pin it is on. A DIAGNOSTIC: a
  selector's pins are different clocks -- a Sound BlasterX G6 offers a
  DSP path (15) and a stereo-direct one (16) -- and the DEVICE decides
  which it reports, so two runs of one build can exercise different
  clocks and sound different. Pin it to compare them by ear. The bind
  line always says which clock was used.
- `-v` -- say which plugins loaded, and which devices were declined.

## Aiming it at one USB DAC

**YOU DO NOT NEED THIS TO SEE A SECOND DAC.** The in-kernel driver
binds every attached USB DAC (up to four) and each gets its own row --
`usb-<vid><pid>`, labelled from the device's manufacturer string -- so
the tray's volume popup and the `audio_device` setting pick between
them with nothing started by hand. `--usb-id` is for running a chosen
DAC through the RING-3 driver instead, which is what a test does when
it needs to say which implementation a result came from:

```
/$ spawn /bin/snddrv --driver usbaudio --usb-id 0b05:19a8
```

The ring-3 host claims that device away from the kernel driver and
registers its own row for it.

**WITHOUT `--usb-id` THE DEVICE IS NOT PREDICTABLE, and that is not
laziness in the plugin.** The kernel's device table hands out the first
FREE entry, so unplugging one device moves the next one up: which DAC
"the first audio device" means depends on what was unplugged earlier in
the boot. The plugin skips devices it cannot claim and tries the next,
but which of several it lands on is still history-dependent.

## Description

**The sound driver host.** It claims a sound controller, loads the
driver for it out of `/lib/snd/`, and registers with the kernel's sound
core as a device — so `soundd` mixes into it and [`aplay`](aplay.md),
the Audio Player and the per-application volume sliders all work against
a driver that is not in the kernel.

```
/$ spawn /bin/snddrv
snddrv: hda serving pci 6 as hda-ring3
/$ lssound
* hda-ring3  HD Audio (ring 3)        ring3

1 device(s); -v for what each one supports
```

**ADDING A SOUND CARD DOES NOT REBUILD THIS PROGRAM.** The drivers are
`dlopen`'d plugins and this host scans the directory, so support for a
new chip is a `.so` dropped into `/lib/snd/` — the split Windows UMDF
has between `WUDFHost.exe` and a driver DLL, and DriverKit between its
host and a `.dext`. `userland/include/snd_driver.h` is the contract a
plugin implements; `docs/umdf-design.md` is why any of it exists.

**IT NEEDS `spawn`, NOT `run`**, like everything built on
`SYS_DEV_CLAIM`: the legacy loader has no scheduler slot, so the claim
comes back `EPERM`.

**IT TRIES EACH CANDIDATE UNTIL ONE PLAYS.** A laptop with display audio
has two HD Audio controllers and the first is usually the HDMI/
DisplayPort one — all-digital pins, no speakers — so a host that took
the first match would be the system's sound device and play nothing. A
driver declining a device is a normal answer, not a failure; the card is
handed back and the next one tried.

## What moved and what did not

**The kernel still owns the samples.** The shared ring, the exclusive
stream and the consumed-chunk zeroing are all still in
`kernel/drivers/sound/sound.c`. This process never reads or writes a
sample: it is handed the ring's *physical* address and points the
card's engine at it.

**`soundd` was not changed and does not know.** It is the mixer, not a
driver — it opens the stream and mixes exactly as it did against an
in-kernel driver.

**`start()` became asynchronous.** A ring-0 driver has programmed the
engine by the time `start()` returns; this one has only had a request
posted into a shared page. `running` — which apps already watch — is
published when the first period is reported, one chunk later, which at
48 kHz is 21 ms.

**A polite kill hands the card back; a crash does not.** `snddrv`
catches `SIGTERM`, `SIGINT` and `SIGHUP` and releases the device so the
kernel driver can take it, because a claim dropped by a *dying* process
deliberately leaves the device unbound. It also `setsid()`s, so closing
the shell that spawned it does not take it down with the session.

## See also

[`lssound`](lssound.md) for which device is active and what it supports,
[`lscodec`](lscodec.md) for an HD Audio codec's own graph,
[`lspci`](lspci.md) with `-k` for whether a device is claimable at all,
[`aplay`](aplay.md) to play something through it, and
[`dmesg`](dmesg.md) for the claim, the DMA grant and the registration.
