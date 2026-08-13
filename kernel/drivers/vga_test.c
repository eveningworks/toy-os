// Cursor-style name/enum plumbing. Small, but it's the exact thing that
// silently rots: VGA_CURSOR_STYLE_NAMES has to stay in lockstep with
// enum vga_cursor_style by hand (the same convention, and the same
// hazard, as debugflags.c's DBGFLAG_NAMES), and both the `cursor`
// command and cursor_config.c read it generically.
//
// Nothing here touches the framebuffer -- the drawing itself needs
// pixels, which is what the QMP screenshots cover.
#include "ktest.h"
#include "kapi.h"

KTEST("vga", "every cursor style has a name, and it parses back") {
    for (int i = 0; i < VGA_CURSOR_STYLE_COUNT; i++) {
        const char *name = VGA_CURSOR_STYLE_NAMES[i];
        KTEST_ASSERT(name != 0);
        KTEST_ASSERT(name[0] != '\0'); // a NULL/empty slot means the table fell behind the enum

        enum vga_cursor_style parsed;
        KTEST_ASSERT(vga_cursor_style_parse(name, &parsed));
        KTEST_ASSERT_EQ((int)parsed, i);
    }
}

KTEST("vga", "an unknown cursor style is rejected, not guessed at") {
    enum vga_cursor_style parsed = VGA_CURSOR_REVERSE;
    KTEST_ASSERT_EQ(vga_cursor_style_parse("nonsense", &parsed), 0);
    KTEST_ASSERT_EQ((int)parsed, (int)VGA_CURSOR_REVERSE); // output untouched on failure
    KTEST_ASSERT_EQ(vga_cursor_style_parse("", &parsed), 0);
    // Case-sensitive, same "reject rather than guess" rule as
    // dbgflag_parse() and the knum parsers.
    KTEST_ASSERT_EQ(vga_cursor_style_parse("Underline", &parsed), 0);
}

KTEST("vga", "setting a style sticks; an out-of-range one is ignored") {
    enum vga_cursor_style original = vga_cursor_style();

    vga_set_cursor_style(VGA_CURSOR_BEAM);
    KTEST_ASSERT_EQ((int)vga_cursor_style(), (int)VGA_CURSOR_BEAM);

    vga_set_cursor_style(VGA_CURSOR_STYLE_COUNT); // out of range
    KTEST_ASSERT_EQ((int)vga_cursor_style(), (int)VGA_CURSOR_BEAM);

    vga_set_cursor_style(original); // tests run in the LIVE kernel -- put it back
    KTEST_ASSERT_EQ((int)vga_cursor_style(), (int)original);
}

// --- text measurement / clipping -------------------------------------
//
// gfx_draw_string() draws past any boundary it's handed, which has now
// caused the same bug twice in different files (see docs/decisions.md).
// gfx_text_width()/gfx_text_fit_chars() are the budgeting half of the
// fix, and they're pure arithmetic over the font metrics -- so unlike
// the drawing itself they can be asserted on directly here rather than
// eyeballed in a screenshot.

KTEST("gfx", "text width measures one row, stopping at a newline") {
    int cw = gfx_char_w();
    KTEST_ASSERT(cw > 0);
    KTEST_ASSERT_EQ(gfx_text_width(""), 0);
    KTEST_ASSERT_EQ(gfx_text_width("abc"), 3 * cw);
    // A multi-line string has no single width; every caller measuring
    // one is measuring a row.
    KTEST_ASSERT_EQ(gfx_text_width("ab\ncdef"), 2 * cw);
}

KTEST("gfx", "fit_chars never returns a partial glyph") {
    int cw = gfx_char_w();

    KTEST_ASSERT_EQ(gfx_text_fit_chars("abcdef", 3 * cw), 3);
    // One pixel short of the fourth glyph -- three, not "three and a
    // bit". Drawing a partial character is what produced the overlap
    // this exists to prevent.
    KTEST_ASSERT_EQ(gfx_text_fit_chars("abcdef", 4 * cw - 1), 3);
    KTEST_ASSERT_EQ(gfx_text_fit_chars("abcdef", 4 * cw), 4);

    // Never more than the string holds, however wide the box.
    KTEST_ASSERT_EQ(gfx_text_fit_chars("ab", 100 * cw), 2);

    // Degenerate widths are 0, not negative and not a wrap-around.
    KTEST_ASSERT_EQ(gfx_text_fit_chars("abc", 0), 0);
    KTEST_ASSERT_EQ(gfx_text_fit_chars("abc", -50), 0);
    KTEST_ASSERT_EQ(gfx_text_fit_chars("abc", cw - 1), 0);

    KTEST_ASSERT_EQ(gfx_text_fit_chars("ab\ncd", 100 * cw), 2); // stops at the newline
}
