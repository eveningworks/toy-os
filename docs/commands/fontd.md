# fontd

**a `/bin` program.**

**Category:** Services and the system

## Synopsis

    fontd

## Description

The session font. It parses the selected TrueType face, rasterizes the
glyph atlas, and publishes it in shared memory for every graphical
client to draw from.

It is a **service**, not something to type: `data/etc/services.d/fontd`
starts it at boot and init restarts it if it crashes. Run it by hand
only to see what it says.

It takes no options.

It reads three settings and republishes whenever one moves:

| setting | meaning |
|---|---|
| `system.font_face` | the UI face (menus, labels, titles): the filename without the extension, or `builtin` |
| `system.font_mono` | the monospace face (terminals, code), the same way |
| `system.font_size` | the em size in pixels, shared by both |

Ask it what it is doing with `diag font` at the serial console:

    # diag font
    size 14px, generation 4
    ui: liberation-sans
      regular  14x15 cell, line 13, 101 glyphs, proportional
      bold     14x15 cell, line 13, 101 glyphs, proportional
    mono: dejavu-sans-mono
      regular  8x16 cell, line 14, 101 glyphs, monospace
      bold     8x16 cell, line 14, 101 glyphs, monospace

The generation is bumped once every weight of a republish is in place
(and when a family goes `builtin`); a client re-maps when it sees it
move, so it is what anything waiting on a font change should wait on.

## Why it is a program and not part of the kernel

A `.ttf` is untrusted input. It arrives from the filesystem, it is
parsed by code with a lot of offsets in it, and until this existed that
parsing happened in ring 0 — which is precisely the surface Windows
spent a decade of GDI font CVEs on before Windows 10 moved it out to
`fontdrvhost`, a sandboxed user-mode process. A malformed font here now
crashes a restartable service instead of the machine.

The atlas is one shared object rather than a copy per client for two
reasons, and the second is the one that bites. It is tens to hundreds of
kilobytes, so a copy each is large — and it is free to **drift**: a
client carrying its own would go on rendering the old face after
`fontface` changed it. One mapping keeps every client's text identical
to the desktop's by construction.

## What it deliberately does not do

**It does not serve the kernel.** Ring 0 draws its console, its shell
and its panic reports from the bitmap tables compiled into the kernel
image, and it has to: those must work before any process exists, and a
console that cannot draw until a service has started is a console that
cannot report why that service did not start. That is the same split
Windows makes — its bugcheck screen uses a built-in font, not the font
host.

So the two rings genuinely differ in what they draw with, and that is
the design rather than an omission. The text console is the boot font;
the desktop is the session font.

**It publishes nothing for `builtin`.** That face name is the sentinel
meaning "use the baked tables", so there is nothing to rasterize and
every client falls back on its own.

**A failure leaves the previous atlas in place.** A client drawing
yesterday's font is better than one drawing none, so a face that will
not parse or will not fit is reported and otherwise ignored.

## See also

`fontface`, `fontsize`, `diag`, `guictl`
