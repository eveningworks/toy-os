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
point size -- see CHANGELOG's build 347 entry for why these replaced the
original four tiny/small/medium/large names) into one font_ttf.c/h,
selectable at runtime via gfx_set_font_size() (see gfx.c) -- the shell's
`fontsize` command switches between them by typing the number. Baking
multiple fixed sizes offline is the tradeoff that avoids needing a real
runtime TrueType rasterizer (see CHANGELOG for why that's a much bigger
undertaking): you get a choice of sizes, not arbitrary ones, but each one
is genuinely anti-aliased at its native resolution rather than scaled
from another baked size.

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
# values almost exactly (see build 347's CHANGELOG entry) with only the
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
OUT_H = "kernel/include/font_ttf.h"
PREVIEW = "font_ttf_preview.png"

GLYPH_COUNT = 95  # ASCII 32-126


def render_glyph(font, ch, cell_w, cell_h, baseline_y):
    img = Image.new("L", (cell_w, cell_h), 0)
    d = ImageDraw.Draw(img)
    d.text((0, baseline_y), ch, font=font, fill=255, anchor="ls")
    return list(img.getdata())


def emit_h():
    lines = []
    lines.append("#ifndef FONT_TTF_H")
    lines.append("#define FONT_TTF_H")
    lines.append("")
    lines.append("// Anti-aliased bitmap fonts baked from a real TrueType face")
    lines.append("// (JetBrains Mono, OFL 1.1 -- see tools/OFL.txt) at build time by")
    lines.append("// tools/genttf.py. Each glyph is a flat grayscale alpha map (0 =")
    lines.append("// background, 255 = fully the ink color), rendered once offline")
    lines.append("// with real font hinting + anti-aliasing, so it looks like an")
    lines.append("// actual font instead of blocky upscaled pixel art -- gfx.c's")
    lines.append("// gfx_draw_char() alpha-blends it straight into the framebuffer,")
    lines.append("// no runtime rasterization involved.")
    lines.append("//")
    lines.append(f"// {len(SIZES)} sizes are baked in; gfx_set_font_size() (gfx.c) picks which")
    lines.append("// one gfx_draw_char()/gfx_char_w()/gfx_char_h() actually use.")
    lines.append("#include <stddef.h>")
    lines.append("")
    lines.append("enum font_size {")
    for name, *_ in SIZES:
        lines.append(f"    FONT_SIZE_{name.upper()},")
    lines.append("    FONT_SIZE_COUNT")
    lines.append("};")
    lines.append("")
    lines.append(f"#define FONT_TTF_GLYPH_COUNT {GLYPH_COUNT}")
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
    lines.append("#endif")
    with open(OUT_H, "w") as f:
        f.write("\n".join(lines) + "\n")


def emit_c(all_glyphs):
    lines = []
    lines.append("// Anti-aliased bitmap fonts baked from JetBrains Mono (OFL 1.1),")
    lines.append("// ASCII 32-126. GENERATED by tools/genttf.py -- see that file and")
    lines.append("// tools/OFL.txt, not this one, to change the fonts or license text.")
    lines.append('#include "font_ttf.h"')
    lines.append("")
    for name, _size, cw, ch, _baseline in SIZES:
        glyphs = all_glyphs[name]
        lines.append(f"static const unsigned char font_ttf_{name}[FONT_TTF_GLYPH_COUNT][{ch}][{cw}] = {{")
        for code in range(32, 32 + GLYPH_COUNT):
            ch_char = chr(code)
            px = glyphs[ch_char]
            label = "'\\''" if ch_char == "'" else ("'\\\\'" if ch_char == "\\" else f"'{ch_char}'")
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


def render_preview():
    samples = [
        "The quick brown fox jumps",
        "over the lazy dog 0123456789",
        "toy-os graphics mode is up.",
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
        "abcdefghijklmnopqrstuvwxyz",
        "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~",
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


def main():
    all_glyphs = {}
    for name, size, cw, ch, baseline in SIZES:
        font = ImageFont.truetype(FONT_PATH, size)
        all_glyphs[name] = {chr(c): render_glyph(font, chr(c), cw, ch, baseline) for c in range(32, 32 + GLYPH_COUNT)}
    emit_h()
    emit_c(all_glyphs)
    render_preview()
    print("ok: preview + kernel/drivers/font_ttf.c + kernel/include/font_ttf.h written")


if __name__ == "__main__":
    main()
