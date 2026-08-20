# fontface

**a shell builtin.**

**Category:** Appearance and the console

## Synopsis

    fontface                 # list what is available, and what is active
    fontface <name>          # use that face
    fontface builtin         # go back to the glyphs baked into the kernel

## Description

Chooses which font the machine draws with. `fontface` alone lists
`/usr/share/fonts` -- one `.ttf` per face -- and marks the active one.

**A face is named by its filename without the extension**, the same way
a cursor theme is named by its directory. So `dejavu-sans-mono.ttf` is
`dejavu-sans-mono`, adding a face is dropping a file into the directory,
and listing what is available is a directory listing rather than
something that has to open every file and read its internal `name`
table.

Selecting a face parses it (`kernel/lib/ttf.c`) and rasterizes the
101-glyph set the baked font also carries, at the size currently in
effect, into an atlas laid out exactly like a baked one -- which is why
the console, the desktop and every ring-3 client change together. The
choice persists to `/etc/toyos.conf` as `font_face` and is applied on
the next boot BEFORE the size, since an arbitrary size is only
rasterizable once a face is loaded.

Two faces ship: `dejavu-sans-mono` (the classic Linux terminal face) and
`liberation-sans`, which is PROPORTIONAL and is there so that per-glyph
advance widths are observable rather than merely implemented -- pick it
and the desktop's labels visibly narrow.

## What it deliberately does not do

**It does not fall back on failure.** A name that is not there, a file
that is not TrueType (an OpenType/CFF font, a font collection), a file
too large: all of them are refused and the face already in use stays
active. The alternative -- dropping to the baked font on a bad select --
loses the user's font because of a typo.

**It does not tell running clients to re-ask.** A GUI client maps the
font once, at startup (`WIN_REQ_FONT`), so windows already open keep the
old glyphs until they are reopened. The desktop as a whole picks the new
face up when it restarts, which init does for you if you kill it.

**`builtin` is not a face.** It is the absence of one: the glyph tables
`tools/genttf.py` baked into the kernel image, which is what the machine
boots with, panics with, and falls back to when a font file is missing or
malformed. A console that could not draw text until a disk font loaded
could not report why the disk font did not load.

## See also

`fontsize` (the size, which any face can be rasterized at),
`docs/filesystem-layout.md` (what lives in `/usr/share/fonts`),
`kernel/drivers/font_face.c`.
