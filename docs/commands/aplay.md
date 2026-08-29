# aplay

**a `/bin` program.**

**Category:** Appearance and the console

## Synopsis

    aplay [-i] [-v pct] <file>

## Description

Play an audio file, or say what one contains. WAV is the only format
today; the codec table it goes through (`userland/lib/usnd.h`) is what
makes a second one a file and a row rather than a second mechanism.

It always prints what the file is before doing anything with it — the
codec's name, the encoding, the rate and the duration — so a refusal
names the file and the reason instead of just failing to make a noise.
`-i` stops there. `-v` sets this program's own gain, 0 to 100, which is
**not** the `volume` setting: that one is the card's master and belongs
to System Settings, and a program turning its own playback down must not
move a system-wide knob.

Rate and channel conversion is the library's, so a 44.1 kHz mono
recording plays on hardware that only does 48 kHz stereo without the
caller knowing either number. `aplay` waits for the file to finish and
then drains what is still queued, which is why a short sound is not cut
off at the end.

## What it deliberately does not do

There is no playlist, no loop and no seek: those are the GUI player's
(`/bin/wm/apps/player`), and a command that grew them would be a worse
version of it. It records nothing — this system has no capture path.

It cannot play two things at once, and neither can anything else: the
kernel hands out **one** exclusive PCM stream and never mixes
(`docs/decisions/drivers.md`). While the Audio Player, Minesweeper or
another `aplay` holds it, this one says so and exits. Mixing several
sounds within *one* program is what `usnd`'s voices are for; mixing
across programs needs a sound daemon, which is a roadmap item.

## Examples

    aplay /usr/share/sounds/chime.wav
    aplay -i /tests/sine1k.wav
    aplay -v 40 /usr/share/sounds/win.wav

## See also

`config set volume` (the card's master level), the Audio Player app --
which browses a directory, seeks, and plays with Space, the arrows and
`+`/`-`.
