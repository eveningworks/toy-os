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

// --- scrolling --------------------------------------------------------
//
// gfx_scroll_up() is the console's hot path and has two implementations:
// shift the back buffer in RAM, or shift the visible framebuffer in
// place. Only the first is used in practice, and the difference is not
// cosmetic -- the second READS the framebuffer, which a write-combined
// surface makes an uncached bus round trip per load, measured at 178.5ms
// versus 0.5ms per text line under `make run-kvm`. See vga.c's
// double-buffering comment.
//
// What can be asserted here is the shift itself. What CANNOT is the cost
// -- the whole reason that regression survived a green suite is that
// plain QEMU's TCG ignores guest memory types, so both paths are equally
// fast here. `gfxbench` on a KVM guest or real hardware is the only
// thing that measures it, which is why it reports which mode is live.

KTEST("gfx", "scroll_up shifts the back buffer up by exactly pixel_rows") {
    if (!gfx_double_buffered()) KTEST_SKIP("no back buffer to shift");

    int w = gfx_width(), h = gfx_height();
    KTEST_ASSERT(w > 16 && h > 16);

    const int rows = 8;
    uint32_t marker = gfx_rgb(200, 100, 50);
    uint32_t fill   = gfx_rgb(10, 20, 30);

    // A marker line at y == rows must land at y == 0, and both ENDS are
    // checked: a per-row copy that got its length wrong still moves the
    // left edge correctly.
    for (int x = 0; x < w; x++) gfx_put_pixel(x, rows, marker);
    gfx_scroll_up(rows, fill);

    KTEST_ASSERT_EQ(gfx_get_pixel(0, 0), marker);
    KTEST_ASSERT_EQ(gfx_get_pixel(w - 1, 0), marker);

    // ...and the band it vacated is the fill colour, not a copy of the
    // last real row.
    KTEST_ASSERT_EQ(gfx_get_pixel(0, h - 1), fill);
    KTEST_ASSERT_EQ(gfx_get_pixel(w - 1, h - rows), fill);
}

KTEST("gfx", "scroll_up of the whole height clears rather than shifting") {
    if (!gfx_double_buffered()) KTEST_SKIP("no back buffer to shift");

    int h = gfx_height();
    uint32_t fill = gfx_rgb(7, 8, 9);
    gfx_put_pixel(0, 0, gfx_rgb(255, 255, 255));

    // pixel_rows >= height has nothing left to shift, so it is a clear.
    // Guarding this is what stops the row loop running with a negative
    // bound.
    gfx_scroll_up(h, fill);
    KTEST_ASSERT_EQ(gfx_get_pixel(0, 0), fill);
    KTEST_ASSERT_EQ(gfx_get_pixel(0, h - 1), fill);
}
