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
// This is the ONLY palette (apps/theme.h, the kernel-side copy, is
// gone). When a dark mode or accent lands, it starts from the default
// and overrides roles, rather than each app re-picking colours.
//
// THE GREYS ARE A LADDER, AND A WIDGET THAT COPIES ONE AS A LITERAL
// FALLS OFF IT: retuning the ground once left labels as lighter boxes
// and the menu bar invisible, because each had its own 245 or 235.
// Default to UUI_COLOR_UNSET and resolve against a role at draw time.

struct utheme {
    uint32_t window_bg;    // frame fill, tab-strip / scroll-view / dialog ground
    uint32_t panel_bg;     // an app's PAGE: what uapp clears a window to
    uint32_t control_bg;   // button / control face, a step below panel_bg
    uint32_t field_bg;     // editable field / display background (white today)
    uint32_t bar_bg;       // menu bar / status bar: chrome, a step below panel_bg
    uint32_t chrome;       // a redesigned app's menu, command and status bars
                           // (docs/gui-guidelines.md's design language)
    uint32_t text;         // body text
    uint32_t border;       // window / menu / control border lines
    uint32_t accent;       // selection / highlight / focus / checkmark
    uint32_t accent_text;  // text drawn on `accent`
    uint32_t tab_rest;     // a resting tab in a strip: a clear step under control_bg
    // A CONTROL'S OUTLINE IS NOT THE WINDOW'S BORDER. `border` frames a
    // window or a menu and is nearly black; the edge of a text field, a
    // scrollbar thumb or a spinbox is a soft grey, and drawing either
    // one with the other's colour is immediately wrong. Qt separates
    // these too (Mid/Dark against WindowText).
    uint32_t outline;
    // The tint behind SELECTED TEXT, which is not `accent`. Selected
    // rows here are a pale wash with ordinary dark text over them --
    // Explorer's treatment -- while `accent` is the saturated colour a
    // focus ring and an icon selection use. One role each, because a
    // dark mode has to move them independently.
    uint32_t selection_bg;
    // A DECORATIVE rule -- a table's grid lines, a divider between
    // sections. Lighter than `outline`, which edges something you can
    // click, and lighter again than `border`, which frames a window.
    uint32_t separator;
    // ACTION COLOURS, by what a command DOES, for a colour-coded command
    // bar (Image Viewer's, the File Manager's): indexed by enum
    // utheme_action. Ink on light chrome, so dark enough for 3:1 there.
    uint32_t action[8];
    // SEVERITY -- a log line's or a notice's level, indexed by enum
    // utheme_severity. Separate from `action` because they mean different
    // things and a palette may move them apart, even where today's error
    // red is the danger red. Text-dark on white (4.5:1), and ALWAYS
    // drawn with a shape that differs too (a cross, a triangle), so the
    // two are told apart by more than hue.
    uint32_t severity[4];
};

enum utheme_severity {
    UTHEME_SEV_NONE = 0,   // ordinary text
    UTHEME_SEV_ERROR,      // klog 0..3: emerg, alert, crit, err
    UTHEME_SEV_WARNING,    // klog 4
    UTHEME_SEV_COUNT
};
uint32_t utheme_severity(int sev);

// What a command does, as a colour role -- semantic like the close
// button's red, never decoration. A uui_toolbar_item's `tint` may name
// one of these instead of a colour (a static table cannot call
// utheme_current()).
enum utheme_action {
    UTHEME_ACT_NONE = 0,
    UTHEME_ACT_NAV,      // move about: back, forward, up, previous, next
    UTHEME_ACT_VIEW,     // how it is shown: zoom, refresh, sort
    UTHEME_ACT_CREATE,   // make or start: new, play
    UTHEME_ACT_EDIT,     // change it: rename, rotate
    UTHEME_ACT_DANGER,   // lose it: delete
    UTHEME_ACT_ARRANGE,  // the layout: view mode
    UTHEME_ACT_MEDIA,    // the desktop and the picture: wallpaper
    UTHEME_ACT_COUNT
};
uint32_t utheme_action(int role);

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
#define UTHEME_BAR_BG     (utheme_current()->bar_bg)
#define UTHEME_CHROME     (utheme_current()->chrome)
#define UTHEME_TEXT       (utheme_current()->text)
#define UTHEME_BORDER     (utheme_current()->border)
#define UTHEME_ACCENT     (utheme_current()->accent)
#define UTHEME_ACCENT_TEXT (utheme_current()->accent_text)
#define UTHEME_TAB_REST   (utheme_current()->tab_rest)
#define UTHEME_OUTLINE    (utheme_current()->outline)
#define UTHEME_SELECTION  (utheme_current()->selection_bg)
#define UTHEME_SEPARATOR  (utheme_current()->separator)

// --- a widget's own colour, or the theme's ---------------------------
//
// **A WIDGET RESOLVES ITS COLOURS WHEN IT DRAWS, NOT WHEN IT IS
// BUILT.** An init that copied UTHEME_* into the widget would freeze
// whatever palette was live at construction, so a theme change would
// reach only widgets created afterwards -- and every app builds its
// widgets once, at open. Storing "unset" instead keeps the app's
// explicit override working (it is any other value) while leaving the
// default to be looked up per frame, which is how GTK and Qt both
// resolve a style.
//
// The sentinel is outside the 24-bit range ugfx_rgb() produces, so no
// real colour can collide with it.
#define UUI_COLOR_UNSET 0xFF000000u
#define UUI_COLOR(v, role) ((v) == UUI_COLOR_UNSET ? (role) : (v))

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
