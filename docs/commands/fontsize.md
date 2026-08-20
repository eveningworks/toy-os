# fontsize

**a shell builtin.**

**Category:** Appearance and the console

## Synopsis

    fontsize                 # what size is in effect
    fontsize <points>        # use that size

## Description

Sets the em size everything is measured from. The whole interface is
font-derived -- window sizes from each app's `default_size()`, chrome
from `gfx_char_h()`, the desktop's column pitch from `gfx_char_w()` --
so this reflows the UI rather than clipping it.

**Any size works once a face is loaded.** With a face selected from
`/usr/share/fonts` (see `fontface`) the size is rasterized on demand, so
`fontsize 13` and `fontsize 32` for a HiDPI panel are ordinary requests.
Atlases are cached, so going back to a size already used is free.

**With no face loaded, the size SNAPS.** The kernel image carries eight
baked sizes (8, 10, 12, 14, 16, 18, 20, 24) and cannot produce a
thirteenth out of nothing, so the request goes to the nearest one and
this says which -- rather than reporting 13 while drawing 12. The
persisted value in `/etc/toyos.conf` is what actually took effect.

## What it deliberately does not do

**It does not resize the legacy console.** The 0xB8000 text-mode
fallback has one hardware cell size; this affects the framebuffer
console only.

**It does not clear the screen while the desktop is up.** Recomputing
the console's rows and columns always happens; the clear that goes with
it is skipped when a compositor owns the screen, because that clear ends
in a full-screen blit over whatever the desktop has drawn.

**It does not retype running clients' fonts.** Same as `fontface`: a
window already open keeps the metrics it mapped at startup.

## See also

`fontface`, `config set system.font_size <n>` (the same setting through
the settings registry), `kernel/lib/font_config.c`.
