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
             plays:  16-bit, 48 kHz
```

**What a card says, and what it plays now.** The rates and depths are
read off the hardware unchanged, and they are what a card's format may be
chosen from ([`sndfmt`](sndfmt.md), or the Format card in System
Settings). `plays:` is the width and rate the card runs at this moment:
the width its setting asks for (the deepest it offers by default), and
the rate the last sound settled on -- which follows what plays unless the
card is set to a fixed one. Below 32 bits the driver rounds every sample
of the 32-bit stream on the way out (QEMU's HDA codec offers only 16; a
laptop's codec usually plays 24).

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

**`ac97`** reports 16-bit at 48 kHz, plus -- on a codec with Variable
Rate Audio -- each standard rate the codec reads back exactly when it is
written: a codec rounds a rate it cannot do, and a rounded one is not on
offer.

**`usb-audio`** reports the widths of the alternate settings it can
play, and their rates: the rates each UAC1 alternate lists, or what a
UAC2 device's clock answers when asked for its range. The ring-3 driver
(`snddrv --usb-id`) offers only the rates with a whole number of frames
per packet -- 48, 96 and 192 kHz, not 44.1.

**`plays:`** is the width the driver plays at -- the deepest HDA format,
the deepest USB alternate, unless the card's setting chose another --
and the rate. The stream above it is always 32-bit, so a 16-bit card
here means the driver rounds every sample to 16 bits on the way out.

## See also

[`aplay`](aplay.md) to play a file, [`sndfmt`](sndfmt.md) to choose a
card's format, [`lscodec`](lscodec.md) for an HD Audio codec's own
graph, and System Settings' Sound page to choose which device is
active.
