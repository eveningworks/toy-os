# soundd

**a `/bin` program.**

**Category:** Services and the system

## Synopsis

    soundd

## Description

The system sound daemon. It owns the machine's one PCM stream and mixes
every program that wants audio into it, so two programs can be audible
at the same time.

It is a **service**, not something to type: `data/etc/services.d/soundd`
starts it at boot and init restarts it if it crashes. Run it by hand
only to see what it says.

The kernel deliberately hands out **one exclusive stream** and never
mixes -- `docs/decisions/drivers.md` has why, and it is the same call
ALSA, CoreAudio and WASAPI make. With no daemon running, the second
program to ask for sound gets `-EBUSY` and is silent for the rest of its
life, the only notice being a line in the log. A userspace server owning
the device is how every real system answers that -- ALSA's `dmix`,
PulseAudio, PipeWire, Windows' audio engine.

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
which it got** -- that is what `usnd_sink.h`'s two rows are for, and it
is why `aplay`, the Audio Player, Minesweeper and Doom carry no code of
their own for either case.

The rings are shared memory (`SYS_SHM_OPEN`, `MAP_SHARED`), the same
shape as the kernel's own stream (`abi/sound_abi.h`), so the daemon
plays the part the hardware plays for a single client: it advances
`hw_pos` as it consumes and **zeroes each chunk before moving past it**.
That one rule is why a client that dies needs no cleanup here — its ring
goes quiet on its own instead of looping.

`lsshm` lists the namespace, so the daemon's clients are readable from
outside the daemon — one row per ring, named `snd.<pid>`. A healthy
client reads `REFS 4`: its own descriptor and mapping, plus the
daemon's.

**An `(unlinked)` row is not a client.** The kernel releases an object's
name when its creator dies, and the daemon ignores anything so marked.
Holding one instead would leave the daemon's own reference as the only
thing keeping that name alive, so the loss would go unnoticed and the
live ring a recycled pid creates behind it could never be adopted.

## What it deliberately does not do

- **No resampling.** Every client writes stereo s32 at the rate this
  answers it with; conversion is the library's job, in the client,
  where `usnd` already does it.

## The card's rate

**It decides it, from the card's setting** ([`sndfmt`](sndfmt.md),
`/etc/sound-cards.conf`). A client asks with the rate of what it plays
(`src_rate` in its ring's control page) and writes nothing until
answered (`rate`). Under **match** -- the default -- a client playing
ALONE moves the card to its rate when that is allowed, once the audio
already mixed for the card has played; anyone else playing keeps the
card where it is, and the newcomer resamples. A client with no rate of
its own (a click, a game's effects) leaves the card alone. A fixed rate
always holds. Each move is said in the log: `soundd: hda1 now at 44100
Hz, its deepest width`.
- **No master volume of its own.** `system.volume` is the card's
  master and is applied by the driver below this; what `soundd` applies
  is each application's own gain, from `/etc/sound.conf`.
- **No priority, ducking or routing.** A mix is the sum of the clients
  in 64 bits, clamped once.
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
