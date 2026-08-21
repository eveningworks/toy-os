#ifndef UUI_LABEL_H
#define UUI_LABEL_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_primitives.h"

// A line of text the LAYOUT knows about.
//
// The missing primitive. Every app here that wanted a caption drew it in
// on_draw and worked out its own coordinates -- which is fine until the
// thing it captions moves, and until the page scrolls. System Settings
// hit both in one day: a page heading painted in on_draw landed on top
// of the first control (on_draw runs AFTER the toolkit paints widgets),
// and reserving space for it by hand would have left content sliding
// underneath it as the page scrolled.
//
// So: a widget with no behaviour and no input, whose entire job is to
// occupy a row the layout has accounted for. It takes no press, no key
// and no focus -- `hit` is absent, so the router never offers it
// anything and a click passes through to whatever is behind.
//
// The text is POINTED AT, not copied: a caption is usually a literal or
// an app-owned buffer, and copying would mean a size limit and a second
// place the string lives. A buffer must outlive the label.

struct uui_label {
    int x, y, w, h;
    const char *text;   // caller-owned; NULL draws nothing
    uint32_t fg, bg;
    // Reserve room for this many rows even when `text` is shorter or
    // NULL, so a caption that changes cannot reflow the page under the
    // user. 0 means one row.
    int rows;

    // WORD-WRAP within `w`, across at most `rows` lines. Off by
    // default, which is Qt's QLabel::setWordWrap and GtkLabel:wrap --
    // most labels are a word or two in a control and wrapping one would
    // be surprising.
    //
    // THE HEIGHT STILL COMES FROM `rows`, NOT FROM THE TEXT, and that
    // is what keeps this cheap. A real toolkit asks "how tall are you
    // at this width?" (Qt's heightForWidth) and needs a second measure
    // pass through the layout; here the caller RESERVES the rows and
    // wrapping fills them. So a wrapping label needs `rows` set to what
    // its longest text will need -- text that does not fit is
    // ellipsised on the last line rather than silently cut, so the
    // shortfall is visible instead of being a sentence that appears to
    // end early.
    //
    // A wrapping label should be UUI_FILL_W: its natural width is
    // deliberately small (it takes whatever it is given rather than
    // demanding the width of its longest line, which is what would push
    // a page wider than the window).
    int wrap;

    // WHICH FONT THIS LABEL IS IN. NULL -- the default -- means the
    // session's regular weight, so every existing label is unchanged.
    // ugfx_font_session(UGFX_FONT_BOLD) makes a heading; a font the app
    // rasterized itself (ugfx_font_load) makes one at any size or face.
    //
    // **IT AFFECTS MEASUREMENT AS WELL AS DRAWING**, and that is the
    // whole reason it lives on the widget rather than being something
    // an app sets around its draw call. natural_size() is asked by the
    // layout, long before and far away from any drawing, so a bold
    // label whose font was only selected at paint time would be
    // measured in regular and laid out too narrow -- text drawn into a
    // box sized for a different font, which is the failure this field
    // exists to prevent.
    const struct ugfx_font *font;

    // RESERVE ROOM FOR THE LAST ROW'S DESCENDERS. Off by default.
    //
    // A glyph bitmap is taller than a line (ugfx.h's struct ugfx_font
    // says why), so a 'g' on the label's final row hangs a couple of
    // pixels below its box. Usually that is harmless -- it lands in the
    // gap a layout leaves between children, and the tail survives. It is
    // only a problem when the next thing along paints an opaque
    // background right there, which erases it.
    //
    // **THIS WAS ONCE ALWAYS-ON AND THAT WAS WRONG.** Two pixels a label
    // is nothing until a page has twenty of them: System Settings' Mouse
    // page grew 42px, which pushed its speed control past the bottom of
    // the scroll view and made it unclickable -- the "a control below the
    // fold is unreachable" trap, caused by trying to save a descender.
    // So the caller decides, and the default is the one that cannot move
    // anything.
    int descender_room;
};

void uui_label_init(struct uui_label *l, const char *text);
void uui_label_set_text(struct uui_label *l, const char *text);
void uui_label_natural_size(const struct uui_label *l, int *out_w, int *out_h);

// Turns word-wrapping on (see `wrap`) and reserves `rows` lines for it.
// One call rather than two fields set by hand, because the two are only
// ever meaningful together -- wrapping into one row is just clipping.
void uui_label_set_wrap(struct uui_label *l, int rows);

// One line of word-wrapping: copies the longest prefix of `src` that
// fits in `max_w` pixels into `out` (at most `cap` bytes, always
// NUL-terminated), breaking at the last SPACE rather than mid-word, and
// returns where the next line starts.
//
// IT ALWAYS CONSUMES AT LEAST ONE CHARACTER, including when a single
// word is wider than the whole line -- such a word is broken where it
// must be. Returning `src` unchanged there would be the natural
// "refuse to break a word" behaviour and would loop forever inside a
// draw call, i.e. hang the compositor rather than draw something ugly.
// `userland/tests/wrap_test.c` asserts exactly that.
//
// Public so it can be tested, and because a future multi-line widget
// wants the same rule rather than a second copy of it.
const char *uui_label_wrap_next(const char *src, int max_w, char *out, int cap);
// CLIPPED to its own width, always: ugfx_draw_string does not clip, and
// a caption longer than its column would otherwise run into whatever is
// beside it -- the identical overlap bug this codebase has shipped
// twice (docs/gui-guidelines.md).
void uui_label_draw(struct ugfx_surface *s, const struct uui_label *l);

struct uui_widget_ops;
extern const struct uui_widget_ops uui_label_ops;

#endif
