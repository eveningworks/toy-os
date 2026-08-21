#ifndef UTHEME_H
#define UTHEME_H

#include "ui/ugfx.h"

// The ring-3 toolkit's THEME: a PALETTE (colour roles, Qt's QPalette /
// GTK's named colours) and METRICS (font-derived sizes and spacing,
// Qt's QStyle). One live theme that every widget reads through, so a
// colour or a control's size is chosen in ONE place instead of per app
// -- and so a future variant (a dark mode, a user accent colour) is a
// swap of this struct rather than an audit of every call site.
//
// **COLOURS are fixed values; METRICS are functions of the current
// font.** A palette does not change when the font size does, but a
// checkbox box or a row's padding must, so the two are split the way
// QPalette and QStyle are: `utheme_current()` hands back the colours,
// the `utheme_*()` metric calls derive from `ugfx_char_h()` live and
// need no rebuild when `font_size` changes.
//
// The default palette's values are apps/theme.h's, kept in step
// deliberately -- a client whose colours drift from the desktop's reads
// as a rendering bug (the same risk the font mapping was built to
// avoid). When a dark mode or accent lands, it starts from the default
// and overrides roles, rather than each app re-picking colours.

struct utheme {
    uint32_t window_bg;    // default window content background
    uint32_t panel_bg;     // window / panel / control-row background
    uint32_t control_bg;   // button / control face
    uint32_t field_bg;     // editable field / display background (white today)
    uint32_t text;         // body text
    uint32_t border;       // window / menu / control border lines
    uint32_t accent;       // selection / highlight / focus / checkmark
    uint32_t accent_text;  // text drawn on `accent`
};

// The live theme. Never NULL: the first read lazily installs the default
// palette, so a UTHEME_* used before uapp_run() calls utheme_init()
// still resolves. utheme_set() replaces it wholesale (a future dark
// mode / accent picker); utheme_default() fills `out` with the defaults
// so a variant can start from them.
const struct utheme *utheme_current(void);
void utheme_init(void);
void utheme_set(const struct utheme *t);
void utheme_default(struct utheme *out);

// --- colour roles, spelled as the call sites already spell them ------
//
// Redefined from fixed `ugfx_rgb()` macros to reads of the live theme,
// so every existing UTHEME_* site becomes theme-driven with no edit.
// Safe because `ugfx_rgb()` is a function -- these were already runtime
// expressions, never usable in a file-scope static initialiser, so
// nothing depended on them folding to a constant.
#define UTHEME_WINDOW_BG  (utheme_current()->window_bg)
#define UTHEME_PANEL_BG   (utheme_current()->panel_bg)
#define UTHEME_BUTTON_BG  (utheme_current()->control_bg)
#define UTHEME_WHITE      (utheme_current()->field_bg)
#define UTHEME_TEXT       (utheme_current()->text)
#define UTHEME_BORDER     (utheme_current()->border)
#define UTHEME_ACCENT     (utheme_current()->accent)
#define UTHEME_ACCENT_TEXT (utheme_current()->accent_text)

// --- metrics (QStyle), FONT-DERIVED and live -------------------------
//
// Every one tracks `ugfx_char_h()`, so raising `font_size` scales the
// chrome with the text -- the same "layout is font-derived, never a
// pixel constant" rule docs/gui-guidelines.md holds apps to, made
// available centrally instead of re-derived per app. All clamp to a
// sane floor when the font is not up yet (char_h == 0 before uapp_run()
// fetches it).
int utheme_pad(void);        // outer padding / margin
int utheme_gap(void);        // spacing between sibling controls
int utheme_border_w(void);   // border stroke width
int utheme_indicator(void);  // checkbox / radio box edge
int utheme_control_h(void);  // a one-line control's height
int utheme_focus_w(void);    // focus-ring inset / width

#endif
