# lsdisplay

**a `/bin` program.**

**Category:** System information

## Synopsis

    lsdisplay

## Description

The screen and the monitor on it, over `QUERY_DISPLAY`: which display
driver claimed the hardware, the mode it is showing (size, depth, pitch,
how many scanouts a flip has), its capability bits, and -- when the
driver read an EDID -- what the monitor said about itself: its PNP
vendor letters, its name, whether its input is digital, and its native
timing with the refresh rate and pixel clock the EDID encodes.

The last line is the one this exists for. A driver that inherits the
firmware's mode rather than setting one (the Intel driver on the
bare-metal laptop) can show 1920x1080 without knowing whether that is
the panel's native timing; only the EDID says so, and `On screen:`
compares the two.

## What it deliberately does not do

It does not change anything -- the mode is `config set resolution`, and
the setting only offers what the driver lists. And it reads the EDID
the kernel read at boot; a monitor plugged in later is not seen until
hotplug exists (`docs/roadmap.md`).

## Where the EDID comes from

Three sources feed one parser (`kernel/drivers/display/edid.c`): the
Intel driver's eDP AUX channel, virtio-gpu's `GET_EDID`, and the
`-vga std` adapter's EDID BAR. `dmesg` carries the same facts as
`display: EDID ...` lines, plus every detailed timing rather than only
the first. A monitor with no EDID, or one whose block fails its
checksum, reports `no EDID`.

## See also

`lscpu`, `lspci`, `lsusb` for the same shape of question about other
hardware; `config get display.native_width` for one number of it;
`gfxbench` for how fast the framebuffer is.
