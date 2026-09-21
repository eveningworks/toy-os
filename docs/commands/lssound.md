# lssound

**a `/bin` program.**

**Category:** System information

## Synopsis

```
lssound [-v]
```

## Options

- `-v` -- also print the rates and bit depths each device reports it
  supports.

## Description

Lists the sound devices the kernel has registered, and marks the one
the stream is on.

```
/$ lssound
* hda0       QEMU HD Audio            hda
  ac97       Intel 82801AA AC97       ac97

2 device(s); -v for what each one supports
```

**`-v` is what the card says about itself:**

```
/$ lssound -v
* hda0       QEMU HD Audio            hda
             rates:  16 22.05 32 44.1 48 88.2 96 kHz
             depths: 16-bit
```

**WHAT A CARD SAYS IS NOT WHAT THIS STACK USES.** The shared ring is 48
kHz, 16-bit, stereo everywhere (`SND_RATE` and `SND_CHANNELS` in
`abi/sound_abi.h`), `usnd` resamples every file to it, and nothing asks
a card for anything else. These numbers are read off the hardware and
reported unchanged — they are the facts a per-device format would have
to be built on, gathered before building it rather than after.

**Why it exists at all.** There was no way to *list* the sound devices.
The only view was the `audio_device` setting's choice list, which
answers "what may I pick" — a question about the setting. This answers
"what is this", which is a question about the hardware, and is where a
capability belongs.

**`not reported` is not `nothing`.** A mask of zero means the driver
does not say; a device that genuinely supported no rate could not have
been registered at all. The two are printed differently on purpose.

## What each driver reports

**`hda`** asks the codec: `PARAM_PCM_SUPPORT`, from the DAC when it
carries the format-override capability and from the audio function
group otherwise. That is the real hardware answer, and it differs
between machines — QEMU's controller offers seven rates, a laptop's
codec its own set.

**`ac97`** reports 48 kHz 16-bit, which is **the driver's set, not the
codec's ceiling**. An AC97 codec with Variable Rate Audio can do more,
but VRA is not programmed here, so reporting what the hardware might
manage would describe something nothing can ask for.

**`usb-audio`** reports every alternate setting its descriptor walk
saw, including the ones the driver cannot use — the same list the
refusal line in `dmesg` prints when a device offers no 48 kHz stereo
s16 stream.

## See also

[`aplay`](aplay.md) to play a file, [`lscodec`](lscodec.md) for an HD
Audio codec's own graph, and System Settings' Sound page to choose
which device is active.
