#ifndef WM_TASKBAR_H
#define WM_TASKBAR_H

// The height the strip may take, and what it is with nothing written.
// **THE BOUNDS ARE THE DECLARATION'S TWIN** -- /etc/settings.d/
// desktop.taskbar_height carries the same Min/Max/Step/Default, because
// that is what bounds a value arriving from System Settings or a hand
// edit. These are what the code clamps with; change both together.
//
// A PIXEL CONSTANT, not font-derived: the two tiers' fonts do not share
// a line height, and a formula gave 36 on one side and 40 on the other.
// 48 is Windows 11's; KDE's is 44.
//
// This is the BAR's thickness. `taskbar_h` (wm_internal.h) is the BAND
// the strip reserves, which is the bar plus TASKBAR_FLOAT_GAP while
// `desktop.taskbar_float` is on -- see struct taskbar_geom.
#define TASKBAR_H_MIN  24
#define TASKBAR_H_MAX  96
#define TASKBAR_H_STEP 2
#define TASKBAR_H_DEFAULT 48

// The taskbar strip's LAYOUT, computed in one place and read by both
// wm_render.c (drawing the buttons) and wm_input.c (hit-testing them).
//
// It used to be neither: `win_btn_w()` returned a constant and each of
// the three call sites walked the windows itself, marching `bx` right by
// a fixed step. With enough windows open the last buttons ran off the
// screen edge and under the clock -- the reported bug -- and any fix
// applied to one of the three walks would have silently disagreed with
// the other two. A layout a caller cannot recompute differently is the
// point of this file.
//
// WHAT IT DOES, and it is Windows' behaviour rather than an invention:
// buttons take their natural width while they fit, SHRINK toward a
// font-derived floor as more windows open, and once even the floor will
// not fit, windows of the same application COLLAPSE into one button
// carrying a count. Clicking a collapsed button opens a list of its
// windows (Windows' jump list; KDE Plasma's grouped-task popup).
// `desktop.taskbar_combine` moves that point -- always, when full (the
// above), never -- as Windows 11's "Combine taskbar buttons" does. Past
// it, the buttons that do not fit go behind an OVERFLOW button at the
// end of the row, a count that opens a list of them (Windows 11's
// overflow flyout), rather than being drawn off-screen, which is the
// failure this file exists to end. A second row is the
// obvious next step and is deliberately not taken -- `taskbar_h` is a
// constant that the desktop icon area, the Start menu's anchor, the
// context-menu clamp and every damage rect all derive from -- and now
// a SETTING, `desktop.taskbar_height`, adopted through
// taskbar_poll_config() and relaid through wm_layout_changed().
//
// GROUPING KEY: `struct window.app_id` -- the client's own name for what
// its window IS ("notepad"), which is why every uapp now sets one. A
// window with no app_id (a kernel-space app window, or a client that
// declared none) groups by its client pid instead, so it can never be
// merged with an unrelated window that also said nothing.

#include "wm.h"

// One button on the strip. `first` is the index into windows[] of the
// group's frontmost member -- the one a plain click acts on -- and
// `count` is how many windows the button stands for (1 for an
// ungrouped button).
struct taskbar_button {
    // The icon NAME to draw in this button, or NULL. Borrowed from the
    // app registry, which outlives one frame's layout.
    const char *icon;
    int x, w;
    int first;
    int count;
    int elided;          // the label was cut short to fit
    // The open_seq of the window that STARTS this button (the group's
    // lowest-ranked member) -- stable while the button exists, whichever
    // member is in front. What a drag and a glide follow.
    uint32_t key;
    int dragging;        // this button is under the pointer in a drag; x follows it
    char label[32];
};

// Fills `out` with the strip's current buttons, left to right, and
// returns how many were written. Never writes more than `max`, and
// never places a button that would cross into the tray.
//
// RECOMPUTED ON EVERY CALL rather than cached: it depends on the window
// list, the live font metrics and the tray's width, and a cache would be
// a fourth thing that can disagree with the other three.
int taskbar_layout(struct taskbar_button *out, int max);

// The button window `idx` is shown on -- its own, or its group's -- as a
// screen rect; 0 when the strip has no room for it. What a minimize
// shrinks toward (wm_anim.h).
int taskbar_button_rect_for(int idx, int *x, int *y, int *w, int *h);

// The icon column inside a taskbar button: its edge length in pixels.
// Two callers must agree on it -- wm_render.c draws the icon and this
// file's make_label() has to reserve the same width, or a label is
// truncated for a column that is not there, or worse runs under one
// that is. The same "one geometry calculation per widget" rule
// uui_scrollbar.h states.
//
// WHICH icon is not a taskbar question and no longer lives here: a
// title bar asks it too, so it is wm_window_icon_name() in wm.c.
int taskbar_icon_size(void);
#define TASKBAR_ICON_MAX 32

// A labelled button's padding either side, and the gap between its icon
// and label. win_btn_w(), make_label() and the renderer all count from
// these, so the drawn text starts where the layout reserved it.
#define TB_PAD      12
#define TB_ICON_GAP 8

// The strip's height with `desktop.taskbar_height` unset -- the
// declaration's twin above, NOT a font formula: the
// two rings' fonts do not share a line height.
int taskbar_default_h(void);

// HOW THE START BUTTON LOOKS: the word, the mark, or both --
// `desktop.start_button`, an enum DECLARED in
// /etc/settings.d/desktop.start_button. XFCE's Whisker Menu offers this
// same three-way (Icon / Title / Icon and title) and KDE's launcher the
// same choice against an icon-only default; a boolean cannot express
// `both`, which is what Windows 95 through 7 shipped.
//
// IT LIVES HERE BECAUSE THE STRIP'S GEOMETRY DEPENDS ON IT. The Start
// button's width is derived from what is inside it (start_btn_w() in
// wm_render.c), and everything else on the strip starts to the right of
// that -- so the mode has to be one answer three files agree on, the
// same rule the icon column above states.
enum start_button_mode {
    START_BUTTON_TEXT,   // "Start" alone
    START_BUTTON_ICON,   // the mark alone, KDE Plasma's -- the DEFAULT
    START_BUTTON_BOTH,   // mark then word, Windows 95's
};
enum start_button_mode taskbar_start_mode(void);

// The mark's edge length inside the Start button, or 0 when this mode
// draws no mark. Same "one geometry calculation" rule as
// taskbar_icon_size(), and the same reason: wm_render.c blits at it and
// start_btn_w() reserves it.
int start_icon_size(void);

// FOUR INDEPENDENT SETTINGS, as Plasma keeps them (Windows has only the
// alignment): what a window button shows -- `desktop.taskbar_buttons`,
// labelled | icons; where the buttons sit -- `desktop.taskbar_align`,
// left | center; where Start sits -- `desktop.start_position`, left |
// center; and whether the panel floats -- `desktop.taskbar_float`. One
// layout function places every combination (taskbar_layout()), and it
// and the draw function ask taskbar_geom() below, so none of them is a
// second taskbar.
enum taskbar_buttons {
    TASKBAR_BUTTONS_LABELLED,  // icon and title -- the DEFAULT
    TASKBAR_BUTTONS_ICONS,     // the icon alone, larger; the title is the tooltip's
};
enum taskbar_buttons taskbar_buttons(void);
int taskbar_align_centered(void);
int taskbar_start_centered(void);  // the Start MENU opens centred when this is
int taskbar_float_on(void);

// The strip's colours -- `desktop.taskbar_theme` (dark | light), apart
// from the window theme because Windows and KDE both let the panel be
// dark over light windows. `accent` follows utheme's, lightened on dark
// so an indicator three pixels tall still reads.
struct taskbar_palette {
    uint32_t bar, edge, hover, focus, focus_edge, text, dim, accent, running;
    // `edge` again as 0xRRGGBB, for `gui taskbar --json`: a test
    // comparing screenshot pixels cannot know the surface's format.
    uint32_t edge_rgb, bar_rgb, text_rgb;
};
const struct taskbar_palette *taskbar_palette(void);
int taskbar_dark(void);

// How far a floating panel stands off the screen's edges.
#define TASKBAR_FLOAT_GAP 8

// WHERE THE STRIP IS DRAWN, inside the band `taskbar_h` reserves.
//
// **THE BAND NEVER CHANGES WITH THE PANEL'S SHAPE**: everything
// derived from `screen_h - taskbar_h` (the work area, maximized
// windows, every popup's clamp) stays put while a floating panel
// deflates to fill the band -- which it does whenever a window is
// maximized, as Plasma 6's does. And every HIT test uses the band, not
// the panel, so the screen's bottom edge still hits the button above
// it (Fitts's law; the gap under a floating panel is not the desktop).
struct taskbar_geom {
    int px, py, pw, ph;   // the panel
    int radius;           // its corners; 0 when it fills the band
    int btn_y, btn_h;     // every button's box, vertically
    int btn_r;            // and its corners
};
void taskbar_geom(struct taskbar_geom *g);
int taskbar_floating(void);   // drawn detached right now (set, and not deflated)
int taskbar_bar_h(void);      // `desktop.taskbar_height`, clamped

// The Start button's box (btn_y/btn_h tall). Its HIT rect is this
// widened to the band's height -- and to the screen's left edge when
// the button is the leftmost thing on the strip.
void taskbar_start_rect(int *x, int *y, int *w, int *h);
void taskbar_start_hit_rect(int *x, int *y, int *w, int *h);

// What the pointer is over on the strip: a windows[] index (a group's
// front member), TASKBAR_HOVER_START, or -1. Updated on pointer motion
// from wm.c; a change damages the band. Also arms the panel tooltip
// with a window's full title where the button does not show it (the
// icon-only style, or a label cut short).
#define TASKBAR_HOVER_START (-2)

// What a plain click on a window's button does: restore it, recover it
// from off-screen, minimize it if it is focused, else raise it. The
// peek card's click does the same (wm_peek.c).
void taskbar_activate(int i);

// DRAG TO REORDER: a press on a window button ARMS it and a release on
// it activates (Windows acts on release too); moving 4px first -- the
// SM_CXDRAG default -- makes it a drag instead, the button following
// the pointer while the others glide aside, and the release commits the
// order into `task_rank`. For the rest of the session only, as Windows
// keeps an order only for pinned apps. ALSO: while a cross-window drag
// (wm_dnd) rests on a button, that window comes forward, so the drop
// can land in it. Every tick, from wm.c.
void taskbar_update_press(int mx, int my, uint8_t buttons);
int taskbar_drag_state(int *armed, int *dragging);   // `gui taskbar --json`
uint32_t taskbar_armed_key(void);                    // the pressed button's key, or 0

// Where button `b` is DRAWN this frame: its layout x, glided there over
// a short tween when its slot changes (`desktop.animations` off: at
// once). Hit tests use the layout x; only drawing asks this.
int taskbar_draw_x(const struct taskbar_button *b);
// 1 while any button is still gliding -- the strip needs another frame.
int taskbar_gliding(void);
// How many frames have drawn a glide since the desktop started. A 150 ms
// glide can begin and end between two samples of taskbar_gliding(); a
// counter cannot miss one.
uint32_t taskbar_glide_frames(void);
// draw_taskbar() calls this after drawing the buttons: it damages the
// strip for the next frame while a glide is still in flight.
void taskbar_glide_frame_done(void);
void taskbar_update_hover(int mx, int my, uint8_t buttons);
int taskbar_hover(void);

// Re-reads `desktop.taskbar_height`, `_style`, `_theme` and
// `desktop.start_button` if anything on the filesystem has changed, and
// every frame re-asks whether a floating panel should deflate. Called once per frame from wm.c, beside desktop_poll_config()
// and for the same reason -- there is no inotify here, so a generation
// counter is what says "ask again". The idle cost is one compare.
void taskbar_poll_config(void);

// How many windows the last taskbar_layout() could not give a button --
// the ones behind the overflow button. Nonzero only when even the floor
// width (and, when combining, grouping) does not fit them.
int taskbar_hidden(void);

// WHEN WINDOWS OF ONE APP SHARE A BUTTON: `desktop.taskbar_combine`.
enum taskbar_combine {
    TASKBAR_COMBINE_FULL,     // only when the floor width does not fit -- the DEFAULT
    TASKBAR_COMBINE_ALWAYS,   // one button per app from the first window
    TASKBAR_COMBINE_NEVER,    // one per window; past the floor, the overflow button
};
enum taskbar_combine taskbar_combine(void);

// THE OVERFLOW BUTTON, after the last window button while anything is
// hidden: its x and width from the last layout, and how many windows it
// stands for (0 = there is none). Its press opens their list on release.
int taskbar_overflow(int *x, int *w);
// ITS ONE SET OF METRICS, read by the layout (w) and the renderer (where
// the chevron and the count go): font-derived, and `w` holds the widest
// label it draws ("99+").
struct taskbar_ovf_metrics {
    int w;        // the whole button
    int pad;      // either side of the content
    int k, v;     // the chevron's half-width and half-height
    int gap;      // between the chevron and the count
};
void taskbar_overflow_metrics(struct taskbar_ovf_metrics *m);
// The count as the button shows it: "7", or "99+" past 99.
void taskbar_overflow_label(char *buf, int cap, int hidden);
int taskbar_overflow_menu_open(void);
int taskbar_overflow_armed(void);   // pressed, not yet released
#define TASKBAR_HOVER_OVERFLOW (-3)

// Listed windows of windows[idx]'s application, idx included; 0 when
// idx itself has no button (a dialog, a popup). What "Close all N
// windows" counts -- and taskbar_close_app() asks every one of them to
// close (close_batch.h).
int taskbar_app_windows(int idx);
void taskbar_close_app(int idx);

// A left PRESS at (mx, my) inside the taskbar's window-button area.
// Returns 1 if a button claimed it -- which ARMS it: the click itself
// (a grouped button's window list, or raise/minimize) happens on the
// release, unless the pointer moved far enough to make it a drag. See
// taskbar_update_press().
int taskbar_handle_click(int mx, int my);

// Same, for a right click: opens the per-window context menu, the group
// list for a collapsed button, the Start button's tools menu, or the
// empty strip's own menu (taskbar_menu.h). The tray takes none.
int taskbar_handle_right_click(int mx, int my);

#endif
