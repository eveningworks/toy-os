# hdad

**a `/bin` program.**

**Category:** System information

## Synopsis

```
hdad [-d INDEX]
```

## Options

- `-d INDEX` -- drive the PCI device at this enumeration index (the one
  [`lspci`](lspci.md) counts) instead of searching. With it, a
  controller that has no analog output is an error rather than a reason
  to try the next one.

## Description

**The HD Audio driver, as a process.** `hdad` takes an HD Audio
controller off the kernel, brings it up, routes its codec, and then
registers itself with the kernel's sound core as a device called
`hda-ring3` — so `soundd` mixes into it, and `aplay`, the Audio Player
and the per-application volume sliders all work against a driver that
is not in the kernel.

It is the end state of `docs/umdf-design.md`. [`lscodec
--tone`](lscodec.md) proved a process *could* make a card play; this is
the version the system uses.

```
/$ spawn /bin/hdad &
hdad: serving pci 4 as the machine's sound device
/$ lssound
* hda-ring3  HD Audio (ring 3)        ring3

1 device(s); -v for what each one supports
/$ aplay /usr/share/sounds/startup.wav
```

**IT NEEDS `spawn`, NOT `run`**, like everything built on
`SYS_DEV_CLAIM`: the legacy loader has no scheduler slot, so the claim
comes back `EPERM`.

**IT TRIES EACH HD AUDIO CONTROLLER UNTIL ONE CAN PLAY.** A laptop with
display audio has two, and the first is usually the HDMI/DisplayPort
one — all-digital pins, no speakers. [`lscodec`](lscodec.md) defaults to
that first controller deliberately, because it is the one that cannot
silence the machine; a *driver* wants the opposite, so `hdad` claims
each in turn, walks its codec, and hands back any that has no analog
route. `-d` pins one and skips the search.

**It runs until it is killed.** Nothing starts it at boot — that is
deliberate while it is new. `kill` it and the kernel's own `hda` driver
takes the card back on the next probe.

## What moved and what did not

**The kernel still owns the samples.** The shared ring, the exclusive
stream and the consumed-chunk zeroing are all still in
`kernel/drivers/sound/sound.c`. This process never reads or writes a
sample; it is handed the ring's *physical* address and points a buffer
descriptor at it.

**`soundd` was not changed and does not know.** It opens the stream and
mixes exactly as it did against the in-kernel driver. That is the whole
point of the exercise: the driver layer moved and the mixer above it did
not notice.

**`start()` became asynchronous**, and it is the one real semantic
change. A ring-0 driver has programmed the engine by the time `start()`
returns; this one has only posted a request into a shared page and
bumped the driver's wakeword. `running` — which apps already watch — is
published when the first period is reported, one chunk later, which at
48 kHz is 21 ms.

## What it is exposed to

**One buffer's physical address, and a device that obeys it.** There is
no IOMMU here (see `docs/umdf-design.md` for why that was decided rather
than assumed), so a ring-3 driver that programs a bus master is trusted
with DMA in a way a ring-3 program normally is not. What it buys is
crash isolation, not containment: `hdad` dying takes the sound device
with it and nothing else, where the same bug in `hda.c` is a panic.

**The codec graph is untrusted input** — node counts, widget types and
connection lists come off the card — and every read in
`kernel/lib/hda_codec.c` is bounds-checked for it. In ring 3 a lie about
them kills a program instead of the machine.

## See also

[`lscodec`](lscodec.md) for the codec graph and the one-shot tone,
[`lssound`](lssound.md) for which device is active, [`lspci`](lspci.md)
with `-k` for whether a device is claimable at all, [`aplay`](aplay.md)
to play something through it, and [`dmesg`](dmesg.md) for the claim, the
DMA grant and the registration.
