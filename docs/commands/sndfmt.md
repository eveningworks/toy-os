# sndfmt

**a `/bin` program.**

**Category:** System configuration

## Synopsis

```
sndfmt [CARD] [-r RATE] [-a LIST] [-b BITS]
```

## Options

- `-r`, `--rate RATE` -- `match` to follow what plays, or one rate in Hz
  (`48000`) the card always runs at.
- `-a`, `--allow LIST` -- the rates `match` may switch the card to, in Hz,
  comma-separated: `44100,48000,96000`.
- `-b`, `--bits BITS` -- `auto` for the deepest width the card offers, or
  one it lists: `16`, `24`, `32`.
- `-h`, `--help` -- every option and what it does.
- `CARD` -- a name from [`lssound`](lssound.md); the active card when left
  out.

## Description

Shows or sets a sound card's output format -- the same setting as the
Format card on System Settings' Sound > Output page and the Format
section of a sound device in Device Manager. Each card keeps its own, by
name, in `/etc/sound-cards.conf`.

```
/$ sndfmt
hda1 (Conexant HD Audio)
  rate:    match what plays, from 44.1 kHz, 48 kHz
  bits:    auto
  playing: 24-bit, 44.1 kHz
/$ sndfmt -a 44100,48000,96000 -b 16
...
applies from the next sound that starts
```

**`match` follows what plays.** A file starting on an idle card takes
the card to its own rate when the allowed list holds it -- a 44.1 kHz
FLAC then reaches the card untouched -- and everything else is
resampled to whatever the card runs at. The card never changes rate
under a sound that is playing: a second program at another rate is
converted to the card's, and a switch waits for the card's queue to
drain. A sound with no rate of its own (a click, a game's effects) keeps
the card where it is. PipeWire's `allowed-rates` is the same rule.

**A fixed rate** is Windows' and macOS' model: one rate, everything
resampled to it.

**Nothing is applied at once.** The daemon, or with no daemon the
program playing, reads the setting the next time it decides a rate --
so a change never cuts into what is playing.

A rate or width the card does not report is refused here, never rounded:
`lssound -v` lists what each card takes.

## See also

[`lssound`](lssound.md) for what each card offers and plays now,
[`soundd`](soundd.md) for who applies the setting.
