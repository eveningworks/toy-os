# soundd

**a `/bin` program.**

**Category:** Services and the system

## Synopsis

    soundd

## Description

The system sound daemon. It owns the machine's one PCM stream and mixes
every program that wants audio into it, so two programs can be audible
at the same time.

It is a **service**, not something to type. **It ships DISABLED**, like
`telnetd` and `tftpd` — `service enable soundd` copies its descriptor
into `/etc/services.d` and init starts it from then on.

**Why disabled**: an audio client stalls instead of finishing while the
daemon mixes it, on real hardware, deterministically — and not in QEMU
on either emulated card (`docs/bugs.md`). Until that is understood,
turning it on trades "two programs can be audible" for "a program that
plays a sound may not exit", which is the worse of the two.

The kernel deliberately hands out **one exclusive stream** and never
mixes — `docs/decisions/drivers.md` has why, and it is the same call
ALSA, CoreAudio and WASAPI make. Before this daemon that meant the
second program to ask for sound got `-EBUSY` and was **silent for the
rest of its life**: starting Minesweeper while the Audio Player had the
card left the game with no audio at all, and the only notice was a line
in the log. A userspace server owning the device is how every real
system answers that — ALSA's `dmix`, PulseAudio, PipeWire, Windows'
audio engine.

## How a client finds it

There are no unix sockets here and no fd passing, so the rendezvous is a
**name**:

| Name | What it is |
|---|---|
| `snd.server` | The beacon. It exists exactly while a daemon is running. |
| `snd.<pid>` | One client's ring, created by that client. |

A program calling `usnd_init()` (`userland/lib/usnd.h`) opens the beacon;
if it is there it creates its own ring and becomes a client, and if it is
not it falls back to the kernel's stream. **No application code knows
which it got** — that is what `usnd_sink.h`'s two rows are for, and it is
why nothing in `aplay`, the Audio Player, Minesweeper or Doom changed
when this arrived.

The rings are shared memory (`SYS_SHM_OPEN`, `MAP_SHARED`), the same
shape as the kernel's own stream (`abi/sound_abi.h`), so the daemon
plays the part the hardware plays for a single client: it advances
`hw_pos` as it consumes and **zeroes each chunk before moving past it**.
That one rule is why a client that dies needs no cleanup here — its ring
goes quiet on its own instead of looping.

The namespace itself is a queryable fact, `QUERY_SHM` — one record per
object, with its size, its reference count and the pid that created it —
so the daemon's client list is readable from outside the daemon.

## What it deliberately does not do

- **No resampling.** Every client speaks the one ABI format (48 kHz
  s16le stereo); conversion is the library's job, in the client, where
  `usnd` already does it.
- **No per-application volume.** `system.volume` is the card's master
  and is applied by the driver below this. A per-client gain is the
  obvious next thing and is on `docs/roadmap.md`.
- **No priority, ducking or routing.** A mix is a saturating sum.
- **It does not resample or reorder for latency.** A client's audio
  reaches the card one chunk later than it would on the raw stream —
  about 43 ms.

## Exclusivity, and what still bypasses it

While the daemon runs it holds the card, so a program that opens
`SYS_SND_OPEN` **directly** gets `-EBUSY`. That is the correct answer
and it is the same distinction WASAPI draws between shared and exclusive
mode. `/tests/tone` is such a program — it tests the raw ring on
purpose — so stop the daemon before running it:

    service stop soundd

## Exit status

`0` when there is no sound device, so a machine without one leaves the
service `exited` rather than crash-looping. Non-zero only when there is
a card it could not serve.

## See also

`aplay`, `service`, `config` (the `system.volume` setting).
