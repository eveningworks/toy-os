#!/usr/bin/env python3
"""Bakes a real TrueType font into static anti-aliased bitmap fonts for
toy-os, the same "generate at build time, bake into a C array, commit the
result" pattern genfont.py used for the original hand-drawn 8x8 font --
except the source of truth here is an actual .ttf file rendered offline
with FreeType (via Pillow), not hand-authored ASCII art. There is no font
rasterizer on-target: this script never runs on toy-os itself, only on a
developer's machine, and its *output* (plain grayscale-alpha byte arrays)
is what actually ships in the kernel image.

Bakes EIGHT sizes (8/10/12/14/16/18/20/24, named and selected by their
point size -- see the commit for build 347 for why these replaced the
original four tiny/small/medium/large names) into one font_ttf.c/h,
selectable at runtime via gfx_set_font_px() (see gfx.c) -- the shell's
`fontsize` command switches between them by typing the number.

**WHAT THIS IS FOR NOW.** There IS a real runtime TrueType rasterizer
(kernel/lib/ttf.c), so these baked tables are no longer how the machine
gets glyphs -- they are how it gets glyphs when nothing else can: before
the filesystem is mounted, on the panic path, and on an image with no
font files. That makes what this script produces the FALLBACK, and the
reason it is still hand-tuned rather than deleted: a console that could
not draw text until a disk font loaded could not report why the disk
font did not load.

Two consequences worth knowing before editing this. The baked sizes are
what gfx_set_font_px() SNAPS to when no face is loaded, so they are the
whole answer on a font-less image and want to stay a sensible ladder.
And these glyphs are hinted by FreeType offline, which a runtime atlas
is not -- so below about 10px the baked font still looks BETTER than a
rasterized face, and that is a real roadmap item rather than a
measurement error.

Font: JetBrains Mono (Regular), SIL Open Font License 1.1. Chosen for
being a well-hinted, widely used, freely embeddable monospace face -- see
tools/OFL.txt for the license text, which must travel with any
redistribution of the font or (per OFL's usual interpretation)
substantial rendered derivatives of it. Install locally with e.g.
`sudo apt install fonts-jetbrains-mono` (Debian/Ubuntu) or
`sudo pacman -S ttf-jetbrains-mono` (CachyOS/Arch via AUR), or download
from https://www.jetbrains.com/lp/mono/ to regenerate.

Run from the repo root: `python3 tools/genttf.py`
"""
import sys

from PIL import Image, ImageDraw, ImageFont

FONT_PATH = "/usr/share/fonts/truetype/jetbrains-mono/JetBrainsMono-Regular.ttf"

# name, pixel size passed to FreeType, baked cell size, baseline offset
# from the cell's top. `name` doubles as the user-facing point size (the
# shell's `fontsize <n>` command and /etc/fontsize's persisted value are
# just this string) and PIXEL_SIZE is that same number, passed straight
# through to FreeType -- unlike the old tiny/small/medium/large naming,
# there's no separate internal-vs-displayed number to keep in sync.
#
# CELL_W/CELL_H/BASELINE_Y are still hand-picked per size rather than
# read verbatim off the font's own metrics (font.getmetrics()'s raw
# ascent+descent would fit every glyph with no clipping at all, but
# produces a visibly taller, more loosely-spaced cell than a terminal
# font usually has). The formula this table was generated with -- given
# an installed JetBrainsMono-Regular.ttf and (ascent, descent) from
# ImageFont.getmetrics() at PIXEL_SIZE -- is:
#     cell_w      = int(font.getlength("M"))
#     baseline_y  = round(ascent * 0.89)
#     cell_h      = baseline_y + round(descent * 0.6)
# which reproduces the original hand-tuned tiny/small/medium/large
# values almost exactly (see the commit for build 347) with only the
# same minor, deliberate descender clipping those already had -- the
# same compromise any fixed-cell terminal font makes. Re-derive with
# that formula (or re-tune the constants) if you add another size.
SIZES = [
    ("8",   8,  4, 10,  8),
    ("10", 10,  6, 12, 10),
    ("12", 12,  7, 14, 12),
    ("14", 14,  8, 16, 13),
    ("16", 16,  9, 18, 15),
    ("18", 18, 10, 21, 17),
    ("20", 20, 12, 23, 19),
    ("24", 24, 14, 27, 22),
]

OUT_C = "kernel/drivers/font_ttf.c"
OUT_H = "kernel/include/api/font_ttf.h"
OUT_SLOTS = "kernel/lib/font_slots.c"  # shared source: both rings link it
PREVIEW = "font_ttf_preview.png"

ASCII_GLYPH_COUNT = 95  # ASCII 32-126

# Nordic letters (Latin-1/ISO-8859-1 single-byte codepoints -- see
# docs/decisions.md's Nordic-keyboard entry for why Latin-1 over UTF-8)
# baked as extra glyphs appended after the contiguous ASCII block,
# rather than widening to the full 0xA0-0xFF Latin-1 Supplement --
# these six are the letters keyboard.c's `se` layout and the
# IS_NORDIC_CHAR() gates across apps/ actually produce; adding more
# later just means appending to this list and re-running this script.
# (codepoint, label-for-comment) pairs, in the order baked.
EXTRA_CHARS = [
    (0xC4, "AE"),  # Ä
    (0xD6, "OE"),  # Ö
    (0xC5, "AA"),  # Å
    (0xE4, "ae"),  # ä
    (0xF6, "oe"),  # ö
    (0xE5, "aa"),  # å
]
GLYPH_COUNT = ASCII_GLYPH_COUNT + len(EXTRA_CHARS)


def render_glyph(font, ch, cell_w, cell_h, baseline_y):
    img = Image.new("L", (cell_w, cell_h), 0)
    d = ImageDraw.Draw(img)
    d.text((0, baseline_y), ch, font=font, fill=255, anchor="ls")
    return list(img.getdata())


# The full ordered list of codepoints baked into every size, in the
# exact order emit_c()/render all iterate -- ASCII 32-126 first (index
# 0..94), then EXTRA_CHARS (index 95..). Index into this list IS the
# glyph index font_ttf_glyph_index() (gfx.c) returns.
ALL_CODEPOINTS = list(range(32, 32 + ASCII_GLYPH_COUNT)) + [cp for cp, _label in EXTRA_CHARS]


def header_text():
    """The exact contents of kernel/include/api/font_ttf.h.

    Split out from emit_h() so --check can compare it against the
    committed file WITHOUT needing the .ttf installed -- see main().
    """
    lines = []
    lines.append("#ifndef FONT_TTF_H")
    lines.append("#define FONT_TTF_H")
    lines.append("")
    # KEEP THIS IN STEP WITH THE COMMITTED font_ttf.h. The header is
    # generated, so a hand-edit there is lost the next time anybody runs
    # this -- and regenerating needs JetBrainsMono-Regular.ttf installed,
    # which most checkouts do not have. So an edit to the header's prose
    # has to be made HERE as well, and the two were last reconciled when
    # the runtime rasterizer landed.
    lines.append("// Anti-aliased bitmap fonts baked from a real TrueType face")
    lines.append("// (JetBrains Mono, OFL 1.1 -- see tools/OFL.txt) at build time by")
    lines.append("// tools/genttf.py. Each glyph is a flat grayscale alpha map (0 =")
    lines.append("// background, 255 = fully the ink color), rendered once offline")
    lines.append("// with real font hinting + anti-aliasing, so it looks like an")
    lines.append("// actual font instead of blocky upscaled pixel art -- gfx.c's")
    lines.append("// gfx_draw_char() alpha-blends it straight into the framebuffer.")
    lines.append("//")
    lines.append("// **THIS IS THE FALLBACK NOW, NOT THE ONLY FONT.** There IS a runtime")
    lines.append("// rasterizer (kernel/lib/ttf.c) and a face loaded from")
    lines.append("// /usr/share/fonts (api/font_face.h) takes precedence when one is")
    lines.append("// selected. These tables remain because they are the only glyphs that")
    lines.append("// need no filesystem, no allocator and no parsing: they draw before")
    lines.append("// the disk is mounted, on the panic path, and whenever a font file is")
    lines.append("// missing or malformed.")
    lines.append("//")
    lines.append(f"// {len(SIZES)} sizes are baked in. gfx_set_font_px() (gfx.c) SNAPS to the nearest")
    lines.append("// of them when no face is loaded, which is why an arbitrary size is")
    lines.append("// answerable only with one; gfx_font_size() reports which it snapped")
    lines.append(f"// to. The {GLYPH_COUNT}-glyph set here is also the set a runtime atlas")
    lines.append("// rasterizes, so the two are interchangeable everywhere.")
    lines.append("#include <stddef.h>")
    lines.append("")
    lines.append("enum font_size {")
    for name, *_ in SIZES:
        lines.append(f"    FONT_SIZE_{name.upper()},")
    lines.append("    FONT_SIZE_COUNT")
    lines.append("};")
    lines.append("")
    lines.append(f"#define FONT_TTF_GLYPH_COUNT {GLYPH_COUNT}")
    lines.append(f"#define FONT_TTF_ASCII_COUNT {ASCII_GLYPH_COUNT} // ASCII 32-126, indices 0..{ASCII_GLYPH_COUNT - 1}")
    lines.append(f"#define FONT_TTF_EXTRA_COUNT {len(EXTRA_CHARS)} // Nordic letters, indices {ASCII_GLYPH_COUNT}..{GLYPH_COUNT - 1}")
    lines.append("")
    lines.append("struct font_ttf_variant {")
    lines.append("    const unsigned char *glyphs; // FONT_TTF_GLYPH_COUNT * h * w bytes,")
    lines.append("                                  // row-major within each w*h glyph")
    lines.append("    int w;")
    lines.append("    int h;")
    lines.append("    const char *name;")
    lines.append("};")
    lines.append("")
    lines.append("extern const struct font_ttf_variant font_ttf_variants[FONT_SIZE_COUNT];")
    lines.append("")
    lines.append("// Latin-1 codepoints of the FONT_TTF_EXTRA_COUNT glyphs baked after")
    lines.append("// the contiguous ASCII block, in baked order -- e.g.")
    lines.append("// font_ttf_extra_codepoints[0] == 0xC4 ('\\xc4', Ä). See")
    lines.append("// font_ttf_glyph_index() (gfx.c) for the codepoint -> glyph-index")
    lines.append("// lookup that uses this.")
    lines.append("extern const unsigned char font_ttf_extra_codepoints[FONT_TTF_EXTRA_COUNT];")
    lines.append("")
    lines.append("#endif")
    return "\n".join(lines) + "\n"


def emit_c(all_glyphs):
    lines = []
    lines.append("// Anti-aliased bitmap fonts baked from JetBrains Mono (OFL 1.1),")
    lines.append("// ASCII 32-126 plus six Nordic letters (Latin-1 Ä/Ö/Å/ä/ö/å) appended")
    lines.append("// after them. GENERATED by tools/genttf.py -- see that file and")
    lines.append("// tools/OFL.txt, not this one, to change the fonts or license text.")
    lines.append('#include "font_ttf.h"')
    lines.append("")
    for name, _size, cw, ch, _baseline in SIZES:
        glyphs = all_glyphs[name]
        lines.append(f"static const unsigned char font_ttf_{name}[FONT_TTF_GLYPH_COUNT][{ch}][{cw}] = {{")
        for code in ALL_CODEPOINTS:
            px = glyphs[code]
            if 32 <= code <= 126:
                ch_char = chr(code)
                label = "'\\''" if ch_char == "'" else ("'\\\\'" if ch_char == "\\" else f"'{ch_char}'")
            else:
                label = f"0x{code:02X}"
            lines.append(f"    // {code} {label}")
            lines.append("    {")
            for row in range(ch):
                rowbytes = px[row * cw:(row + 1) * cw]
                lines.append("        { " + ", ".join(str(b) for b in rowbytes) + " },")
            lines.append("    },")
        lines.append("};")
        lines.append("")
    lines.append("const struct font_ttf_variant font_ttf_variants[FONT_SIZE_COUNT] = {")
    for name, _size, cw, ch, _baseline in SIZES:
        lines.append(f'    {{ &font_ttf_{name}[0][0][0], {cw}, {ch}, "{name}" }},')
    lines.append("};")
    with open(OUT_C, "w") as f:
        f.write("\n".join(lines) + "\n")
    emit_slots()


# THE SLOT ORDER IS ABI AND BOTH RINGS NEED IT, so it is its own file
# rather than the tail of the 16k-line bitmap blob. A ring-3 font
# rasteriser must produce the SAME slots in the SAME order as ring 0 or
# the atlas it hands over is nonsense; linking one definition is what
# makes that true by construction instead of by two lists agreeing.
def emit_slots():
    lines = [
        "// GENERATED by tools/genttf.py -- do not edit.",
        "//",
        "// The Latin-1 codepoints baked after ASCII, and the ONE definition",
        "// of them: kernel/lib is shared source, so ring 0 and ring 3 link",
        "// the same bytes. See api/font_ttf.h for the counts.",
        '#include "font_ttf.h"',
        "",
        "const unsigned char font_ttf_extra_codepoints[FONT_TTF_EXTRA_COUNT] = {",
        "    " + ", ".join(f"0x{cp:02X}" for cp, _label in EXTRA_CHARS) + ",",
        "};",
    ]
    with open(OUT_SLOTS, "w") as f:
        f.write("\n".join(lines) + "\n")


def render_preview():
    samples = [
        "The quick brown fox jumps",
        "over the lazy dog 0123456789",
        "toy-os graphics mode is up.",
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
        "abcdefghijklmnopqrstuvwxyz",
        "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~",
        "ÄÖÅ äöå  Nordic: ÄÖÅ äöå",
    ]
    blocks = []
    for name, size, cw, ch, baseline in SIZES:
        font = ImageFont.truetype(FONT_PATH, size)
        width = max(len(s) for s in samples) * cw
        height = len(samples) * ch
        img = Image.new("RGB", (width, height), (0, 0, 0))
        d = ImageDraw.Draw(img)
        for row, s in enumerate(samples):
            d.text((0, row * ch + baseline), s, font=font, fill=(230, 230, 230), anchor="ls")
        blocks.append((name, img))

    pad = 12
    total_w = max(b.width for _, b in blocks) + pad * 2
    total_h = sum(b.height for _, b in blocks) + pad * (len(blocks) + 1)
    out = Image.new("RGB", (total_w, total_h), (10, 10, 10))
    y = pad
    for name, img in blocks:
        out.paste(img, (pad, y))
        y += img.height + pad
    out.save(PREVIEW)


def check_header():
    """Does the committed header match what this script would emit?

    **THE DRIFT THIS CATCHES.** font_ttf.h is GENERATED, and regenerating
    it needs JetBrainsMono-Regular.ttf installed -- which most checkouts
    do not have. So the tempting move when its prose goes stale (and it
    did: it claimed "no runtime rasterization involved" for a while after
    kernel/lib/ttf.c landed) is to hand-edit the header, which works
    perfectly until the next person who DOES have the font regenerates
    and silently reverts it.

    Deliberately compares only the parts that need no font: the comment
    block, the enum and the counts. The glyph DATA cannot be checked
    without rendering it, and pretending otherwise would be a check that
    passes for the wrong reason. Exit code 0 if they agree.
    """
    want = header_text()
    try:
        with open(OUT_H, encoding="utf-8") as f:
            have = f.read()
    except OSError as e:
        print(f"genttf --check: cannot read {OUT_H}: {e}")
        return 1
    if have == want:
        print(f"genttf --check: {OUT_H} matches what this script would emit")
        return 0
    print(f"genttf --check: {OUT_H} is NOT what this script would emit.")
    print("  A generated header was hand-edited, or SIZES/EXTRA_CHARS moved.")
    print("  Make the SAME edit in genttf.py's header_text() so a regeneration")
    print("  keeps it, then re-run this. Diff:")
    import difflib
    for line in list(difflib.unified_diff(have.splitlines(), want.splitlines(),
                                           "committed", "would emit", lineterm=""))[:40]:
        print("   " + line)
    return 1


def main():
    if "--check" in sys.argv:
        sys.exit(check_header())
    all_glyphs = {}
    for name, size, cw, ch, baseline in SIZES:
        font = ImageFont.truetype(FONT_PATH, size)
        all_glyphs[name] = {cp: render_glyph(font, chr(cp), cw, ch, baseline) for cp in ALL_CODEPOINTS}
    with open(OUT_H, "w", encoding="utf-8") as f:
        f.write(header_text())
    emit_c(all_glyphs)
    render_preview()
    print("ok: preview + " + OUT_C + " + " + OUT_H + " + " + OUT_SLOTS + " written")


if __name__ == "__main__":
    main()
