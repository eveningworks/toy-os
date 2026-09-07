# font

**a `/bin` program.**

**Category:** System information

## Synopsis

    font glyph <char> [--kernel] [--both]

## Options

- `--kernel` -- skip the client half: ring 0's facts and a 1-bit ink
  map, which is the one view that needs no compositor. `--both`
  overrides it and reads the client anyway.
- `--both` -- both maps, both fact blocks and the hash comparison.

Ring 0's facts print in every mode. With neither flag you get the
client's coverage map, both fact blocks and the hash line -- and the
client half needs a compositor, so it falls back to the ink map when
there is none.

## Description

What the machine is actually about to draw, for one character: its
coverage map, its line box, and whether there is any ink in it at all.

    $ font glyph g
    g  U+0067  slot 71
      kernel   a loaded face 14px regular
               cell 8x16  line_h 14  baseline 12  advance 8
               ink  yes, peak 255/255   box x 0..7  y 4..14  (paints below its line)
      client   cell 8x16  line_h 14  advance 8
      hash     kernel 741af8e9   client 741af8e9   agree

      coverage (client, 8x16, 8 bit):
              =##-#.
             +@=-%@:
             @=  =@:
            .@:  :@:
            .@:  :@:
            .@=  -@:
             *%:.%@:
             .*@@+@.
                 -@.
             :-..#*
             -%@@*.
      ramp .:-=+*#%@  (1 -> 255; a space is no ink at all)

## Why it exists

A glyph that rasterised to nothing is pixel-identical on screen to a
space, to a character the font does not carry, and to a font that failed
to load. Nothing else can tell those four apart.

**`ink NONE` is the answer to that**, and `peak` is the harder
half of it — a glyph can have ink and still be far too faint to read,
which no yes/no flag can express:

    $ font glyph 0x20 --kernel
       U+0020  slot 0
      kernel   a loaded face 14px regular
               ink  NONE -- this glyph is entirely blank

## Two views, and the disagreement is the diagnosis

A GUI client draws from its own read-only mapping of the atlas
(`WIN_REQ_FONT`); ring 0 draws from the atlas itself. Those are
different pieces of memory, and a glyph that is present in one and blank
in the other is exactly the case worth catching — so this reads **both**
and compares them.

The comparison is an FNV-1a hash over the coverage bytes rather than two
pictures, because "these two bitmaps are identical" is not a question a
person should answer by eye. `agree`, `DISAGREE`, or `DIFFERENT CELL
SIZE` if the two are not even comparable.

**The two pictures are deliberately different depths.** The client view
prints 8-bit coverage as a grayscale ramp, because how dark the ink is
belongs to whoever draws it — a glyph whose anti-aliasing has collapsed
is present, non-blank and unreadable, and a threshold would hide that.
The kernel view prints a 1-bit ink map, because where the ink is belongs
to ring 0, which is where a glyph either got rasterised or did not, and
a query record is capped at 256 bytes which no cell's coverage bytes fit
in.

## The line box

`cell_h` is the height of a glyph's bitmap; `line_h` is how far apart
two lines of text sit. They are different numbers, and a glyph
legitimately paints below its line — that is what a descender is, and
`font` says so when it happens. Anything that paints an opaque
background over the row below will erase one.

`advance` is this glyph's own, which equals `cell_w` on the baked font
and on any monospace face and does not on a proportional one.

`box` is the ink's extent within the cell, inclusive. It is meaningless
when there is no ink, which is why `peak` and not the box is the test
for blankness.

## A character, or a codepoint

    font glyph g          # a literal character
    font glyph 0x20       # ...or its codepoint, in hex
    font glyph U+00C4     # ...in either spelling
    font glyph 32         # ...or in decimal

The numeric form is not a convenience. **A space cannot be passed as an
argument through any shell here**, and a space is precisely what you
want to compare a suspected-blank glyph against; the six Latin-1 extras
cannot be typed on the layouts this machine ships either. A single
character always wins, so `font glyph 0` is the digit zero.

## What it deliberately does not do

It does not read a font FILE. FreeType's tools, `otfinfo` and `fc-match`
all inspect a file or a configuration, which you can also get by
rerunning the rasteriser offline; here the rasteriser runs in ring 0 and
the interesting bug is about what got *into* the atlas. This is `xfd`
shaped — a live server-side font — rather than `ftdump` shaped.

It shows one glyph. A whole-set summary ("which glyphs are blank") would
be a second feature; `docs/roadmap.md` has it.

It cannot change the font. `fontface` and `fontsize` do that.

## Notes

The client half needs a compositor: `win_server_request()` refuses
everything when no desktop holds the role and no ring-0 presentation
layer is registered. On a `text` boot, and at the kernel `#` prompt
(where the legacy loader has no scheduler slot to own a font mapping),
`font` says so and falls back to the kernel view rather than reporting
nothing.

Ring 0's facts come from `QUERY_FONTGLYPH`, a list with one record per
atlas slot — so they are readable by anything, not just by this command.
