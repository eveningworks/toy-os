# screenshot

**a `/bin` program.**

**Category:** Graphics and the desktop

## Synopsis

    screenshot [-w] [-r x,y,w,h] [-p] [-d seconds] [-f qoi|png] [path]

## Description

Writes a picture of the screen to a file. With no arguments it captures
the whole screen into
`/home/screenshots/shot-<YYYYMMDD-HHMMSS>.qoi` and prints the path it
chose; give it a path and it writes there and says nothing.

It is an ordinary program with no window of its own. It asks the
compositor for the pixels over the client channel, so it works from a
terminal window, from the physical console with the desktop running, and
over `telnet` on a machine whose screen nobody is looking at -- which is
what makes `tools/remote.py screenshot` possible on the bare-metal
laptop.

## Options

- `-w`, `--window` -- capture the topmost window instead of the whole
  screen, chrome and all. "Topmost" rather than "focused" because the
  program asking is usually the focused one. (The Screenshot app lets
  you POINT at the window instead; a command has no pointer.)
- `-r`, `--region x,y,w,h` -- capture that rectangle, clamped to the
  screen. All four numbers are required; a partial rectangle is refused
  rather than completed with guesses.
- `-p`, `--pointer` -- draw the mouse pointer into the capture. Off by
  default, as `PrintScreen` is on Windows and `XGetImage` is under X11.
- `-d`, `--delay seconds` -- wait before capturing, so you can arrange
  the screen first. The wait happens before anything connects.
- `-f`, `--format qoi|png` -- which encoder to use. Otherwise it comes
  from the filename's extension, and from `qoi` for a generated name.

## Which format to use

`qoi` is the default because this system can open it again -- the Image
Viewer decodes QOI, and nothing here decodes PNG. `png` is for a picture
that is LEAVING the machine: a host, a browser, a bug report. Both are
lossless, and on a desktop screenshot both are roughly forty times
smaller than the raw pixels.

## What it deliberately does not do

It does not edit, annotate or scale. It does not put the image on the
clipboard -- the clipboard here carries files and text, so the Screenshot
app's Copy button copies the saved FILE (`docs/conventions/gui.md`).

And it cannot capture while a fullscreen program holds the display: that
program is drawing straight into the display's own buffers and the
compositor's frame is not what is on screen, so the answer is `a
fullscreen program has the display` rather than a picture of whatever
was there before.

## Exit status

0 on success. 1 with a sentence on stdout otherwise -- no desktop
running, a refusal from the compositor, or a file that could not be
written.

## See also

`lsdisplay` for what the screen actually is; the Screenshot overlay
(`/bin/wm/apps/screenshot`, or PrtSc) for a region you drag and resize,
a window you point at, a delay, and a card with Open / Copy / Folder /
Save as afterwards; `docs/conventions/gui.md` for how the
capture reaches a client at all.
