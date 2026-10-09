# vplay

**a `/bin` program.**

**Category:** Graphics and the desktop

## Synopsis

    vplay [OPTION]... FILE

## Options

- `-i`, `--info` -- what the file says it is: container, codec, size,
  frame rate, sound, length and bitrate. Decodes nothing; the default
  when no other option is given.
- `-c`, `--check` -- decode every frame and print its number, time,
  picture type (`I`, `P`, `B`) and a CRC-32 of its pixels, then the
  frame count and the decoding speed.
- `-b`, `--bench` -- decode every frame as fast as it goes and print
  frames a second; nothing per frame.
- `-s`, `--sync` -- find every WHITE frame and every BEEP and print how
  far apart each pair is (made for `/usr/share/videos/av-sync.mpg`).
- `-t TIME`, `--at TIME` -- start at TIME: an exact seek, to the frame
  showing then. TIME takes `sleep`'s units (`5`, `0.5`, `90s`, `1.5m`).
- `-n N`, `--frames N` -- stop after N frames.
- `-o FILE`, `--save FILE` -- write the frame at `--at` (or the first)
  as a picture; the extension picks the format (`.png`, `.jpg`, `.qoi`).
- `-h`, `--help` -- this page in one screen.

Exit status: 0 done, 1 a frame failed to decode or `--sync` found a beep
more than 40 ms from its flash, 2 bad usage (`Try 'vplay --help'`).

## Description

The text half of the Video Player, the way [`aplay`](aplay.md) is the
Audio Player's: it decodes through the same `userland/lib/uvid.h`, so
what it says about a file is what the player will do with it, and it
works over the serial console where there is no window. FFmpeg's
`ffprobe` and `ffmpeg -f null -` are this command on Linux.

It reads MPEG-1 video with MP2 sound in an MPEG program stream (`.mpg`)
and Motion JPEG with PCM sound in AVI (`.avi`). A file in another codec
is described and refused by name -- `MPEG-2 (not supported)` -- rather
than called broken.

`--check`'s CRCs are of the decoded pixels, so two builds can be
compared frame by frame; `--sync` measures where the CONTAINER put each
picture and its sound, which is a fact about the file, not about
playback.

```
/$ vplay /usr/share/videos/first-boot.mpg
/usr/share/videos/first-boot.mpg
  container  mpeg-ps
  video      MPEG-1, 640x360, 30.00 fps
  sound      MP2, 44.1 kHz stereo
  length     1:17 (77792 ms, 2333 frames)
  bitrate    864 kbit/s
/$ vplay -c -n 3 /usr/share/videos/countdown.mpg
    0       10 ms I ef4108f4
    1       44 ms B b8e386d1
    2       77 ms B a67c1374
3 frames in 61 ms, 49.18 fps
/$ vplay -t 30 -o /home/frame.png /usr/share/videos/first-boot.mpg
/home/frame.png
```

## What it deliberately does not do

It plays nothing -- no window, no sound: that is the Video Player's
job, and a command that grew it would be a worse player.

## See also

[`aplay`](aplay.md) (a video's sound plays through it too), the Video
Player app, `tools/uvid_hostcheck.py` (the decoders against FFmpeg).
