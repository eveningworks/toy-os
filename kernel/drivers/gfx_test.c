// KTESTs for gfx.c's TEXT CHOKEPOINTS -- gfx_text_width(),
// gfx_text_fit_chars(), gfx_text_next(), gfx_text_prev().
//
// These four exist so that no widget measures or indexes a string with
// gfx_char_w() itself (see gfx.h's "the rest of the text chokepoints"),
// which makes them the whole surface a future UTF-8 or proportional-font
// change has to get right. That is worth pinning down now, while every
// answer is still obvious, so the migration has something that fails
// when it breaks them.
//
// Everything here is written RELATIVE to gfx_char_w() rather than
// against baked pixel numbers: the font size is a runtime setting
// (`fontsize`, /etc/toyos.conf), so a test asserting "width == 40" would
// pass or fail depending on the machine's config rather than on the
// code. That is the same reason the GUI's own layout is font-derived.
#include "gfx.h"
#include "ktest.h"

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

KTEST("gfx", "text_fit_chars agrees with text_width") {
    // The round trip that matters: whatever fits in W pixels must
    // actually measure <= W. A caller windowing a string trusts both,
    // and they are only useful if they cannot disagree.
    const char *s = "the quick brown fox";
    int cw = gfx_char_w();
    for (int room = 0; room < 25 * cw; room += cw / 2 + 1) {
        int n = gfx_text_fit_chars(s, room);
        KTEST_ASSERT(n * cw <= room || n == 0);
    }
}

KTEST("gfx", "text_next/prev step one character and clamp") {
    const char *s = "abc";

    KTEST_ASSERT_EQ(gfx_text_next(s, 0), 1);
    KTEST_ASSERT_EQ(gfx_text_next(s, 2), 3);
    // At the terminator there is nowhere to go -- a cursor sitting at
    // the end of a field must not walk off it.
    KTEST_ASSERT_EQ(gfx_text_next(s, 3), 3);

    KTEST_ASSERT_EQ(gfx_text_prev(s, 3), 2);
    KTEST_ASSERT_EQ(gfx_text_prev(s, 1), 0);
    KTEST_ASSERT_EQ(gfx_text_prev(s, 0), 0);
}

KTEST("gfx", "text_next/prev round trip in the middle of a string") {
    // prev(next(i)) == i everywhere a step is actually possible. Trivial
    // while one character is one byte; the point is that it stays true
    // once it isn't, which is the whole reason these are functions.
    const char *s = "toy-os";
    for (int i = 0; i < 6; i++) {
        KTEST_ASSERT_EQ(gfx_text_prev(s, gfx_text_next(s, i)), i);
    }
}

KTEST("gfx", "text helpers survive a NULL string") {
    // Widgets pass optional labels straight through (a button with no
    // label is legal, see ui_button_draw), so NULL must be an answer,
    // not a fault.
    KTEST_ASSERT_EQ(gfx_text_width(0), 0);
    KTEST_ASSERT_EQ(gfx_text_next(0, 3), 0);
    KTEST_ASSERT_EQ(gfx_text_prev(0, 3), 2);
}
