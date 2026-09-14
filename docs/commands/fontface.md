# fontface

**a shell builtin.**

**Category:** Appearance and the console

## Synopsis

    fontface                 # list what is available, and what is active
    fontface <name>          # use that face
    fontface builtin         # go back to the glyphs baked into the kernel

## Description

Chooses which font the INTERFACE draws with -- menus, labels, titles,
buttons. `fontface` alone lists `/usr/share/fonts` -- one `.ttf` per
face -- and marks what each family is using.

**THERE ARE TWO FAMILIES, AND THIS SETS ONE OF THEM.** Terminals and
code views draw with a separate monospace face, because a grid of cells
in a proportional face does not line up; the listing marks it
`<- monospace`. This command sets only the interface face, and the
other one is

    config set system.font_mono <name>

rather than a second verb here -- the setting registry already does
this, and two ways to change one thing is how they drift. Both default
to `dejavu-sans-mono`, so an unconfigured machine has them the same.

**A face is named by its filename without the extension**, the same way
a cursor theme is named by its directory. So `dejavu-sans-mono.ttf` is
`dejavu-sans-mono`, adding a face is dropping a file into the directory,
and listing what is available is a directory listing rather than
something that has to open every file and read its internal `name`
table.

**A `-bold` file is a WEIGHT, not a face.** `dejavu-sans-mono.ttf` and
`dejavu-sans-mono-bold.ttf` are one family listed once, and both are
loaded and rasterized when you select it -- so an app can draw bold
text beside regular without a second `fontface`. A family with no
`-bold` file still has a bold: its regular outlines are thickened
(`ttf_embolden`), which is what GDI does when a family has no bold face
and what Cairo and DirectWrite fall back to. `vera-mono` ships without
one on purpose, so that path is exercised rather than merely written.

Pairing by FILENAME rather than by reading each font's own `name` and
`OS/2` tables is the same call the face name itself makes: the
directory is small and seeded by the build, so a suffix rule keeps the
whole registry a directory listing. Real systems do read the metadata
(fontconfig indexes every file; DirectWrite builds a family tree) --
they have to cope with whatever fonts a user has, and toy-os does
not.

Selecting a face parses it (`kernel/lib/ttf.c`) and rasterizes the
101-glyph set the baked font also carries, at the size currently in
effect, into an atlas laid out exactly like a baked one -- which is why
the console, the desktop and every ring-3 client change together. The
choice persists to `/etc/toyos.conf` as `font_face` and is applied on
the next boot BEFORE the size, since an arbitrary size is only
rasterizable once a face is loaded.

Three faces ship, and each is there to make one thing observable rather
than merely implemented:

| Face | Why it ships |
|---|---|
| `dejavu-sans-mono` | the classic Linux terminal face; the default |
| `liberation-sans` | PROPORTIONAL, so per-glyph advance widths are visible -- pick it and the desktop's labels narrow. It is also the only shipped face with a `kern` table, so it is where kerning can be seen |
| `vera-mono` | ships with NO `-bold` companion, so it is the only face on the image whose bold is SYNTHESIZED |

The first two carry a real `-bold` file; `vera-mono` deliberately does
not.

## What it deliberately does not do

**It does not fall back on failure.** A name that is not there, a file
that is not TrueType (an OpenType/CFF font, a font collection), a file
too large: all of them are refused and the face already in use stays
active. The alternative -- dropping to the baked font on a bad select --
loses the user's font because of a typo.

**It does not do kerning it cannot see.** Kerning comes from the
legacy format-0 `kern` table; a face that carries its kerning only in
GPOS (which is most modern OpenType fonts) renders unkerned. Doing that
properly means a shaping engine -- HarfBuzz exists for this -- and is a
deliberate stopping point, not an oversight.

**It does not give you more than the 101 baked glyph slots.** A loaded
face has thousands of glyphs and the atlas rasterizes the same set the
baked font carries. Widening that is the UTF-8 migration, not a
constant.

**`builtin` is not a face.** It is the absence of one: the glyph tables
`tools/genttf.py` baked into the kernel image, which is what the machine
boots with, panics with, and falls back to when a font file is missing or
malformed. A console that could not draw text until a disk font loaded
could not report why the disk font did not load.

## See also

`fontsize` (the size, which any face can be rasterized at),
`docs/filesystem-layout.md` (what lives in `/usr/share/fonts`),
`kernel/drivers/font_face.c`.
