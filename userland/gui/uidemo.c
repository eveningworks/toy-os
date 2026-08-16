// UI Demo -- the app that exists to be tested against, in RING 3.
//
// Every other app here exists to do something; this one exists to be a
// KNOWN TARGET. It shows one of each Toykit widget at documented
// offsets and reports every interaction as a single parseable line, so
// a GUI test is an assertion on text rather than a reading of a
// screenshot -- and when a click lands on the wrong thing, this app
// says which widget it actually hit.
//
// It moved out of the kernel in Milestone 41's stage 0
// (docs/wm-ring3-design.md). That is the point of the port rather than
// a side effect: this app is the test target for a widget set, and the
// widget set that survives the milestone is `userland/ui/`. Testing
// apps/ui/ with it was testing the copy that is going away.
//
// WHAT THE PORT CHANGED, AND WHY
// ------------------------------
// The layout, the widget list and the log grammar are carried over
// unchanged. Three things could not be:
//
//   1. **Diagnostics go to stderr** (`sys_eprint`), not `klog_write`.
//      A client's stdout goes to whoever spawned it; stderr reaches the
//      kernel log and `dmesg`, which is what a test reads. Same wire,
//      same "uidemo: " prefix, so tools/uidemo_test.py still asserts on
//      one stream.
//   2. **The listbox and dropdown act on CLICK, not on press-then-
//      release.** That is the ring-3 widgets' existing contract (see
//      uui_listbox.h / uui_dropdown.h), shared with four apps that
//      already ship. The buttons still arm on press and commit on
//      release, so the cancel path -- press, drag off, release, nothing
//      happens -- is still demonstrable, and it is the case
//      docs/gui-guidelines.md's rule actually exists for.
//   3. **Three widgets, not four, are in the focus ring**: the textbox,
//      the dropdown and the listbox. Ring 3's button group has no
//      keyboard activation, so a tab stop on it would be a stop that
//      does nothing.
//
// ---------------------------------------------------------------------
// LAYOUT -- all coordinates CONTENT-RELATIVE, which in ring 3 means
// they are simply the surface's own coordinates: a client draws into
// its own buffer and the server places the window, so there is no
// origin to add.
// ---------------------------------------------------------------------
//
// Everything is derived from the font (ugfx_char_w()/ugfx_char_h()), so
// nothing here is a raw pixel constant a font-size change invalidates.
// `gui windows --json` gives the content origin; add these offsets.
//
//   ROW_BUTTONS   three uui_buttons in a group: "One" "Two" "Three",
//                 codes 1/2/3. Hover and press states, commit on release.
//   ROW_CHECKS    two uui_checkboxes: "Alpha", "Beta".
//   ROW_RADIO     a uui_radio_list, 2 columns: "Red" "Green" "Blue" "Grey".
//   ROW_TEXTBOX   a uui_textbox. Click to focus, then keys go to it.
//   ROW_DROPDOWN  a uui_dropdown, 10 items. Its popup deliberately opens
//                 OVER the listbox below it -- that overlap is the
//                 point, it proves the popup is drawn last.
//   ROW_LIST      a uui_listbox, 12 items in a 4-row box, so it always
//                 has a scrollbar to exercise.
//   ROW_SCROLL    a uui_textview, prefilled with numbered lines so the
//                 scroll position is readable.
//   ROW_STATUS    a live readout of the last event, so a screenshot is
//                 also self-describing.
//
// ---------------------------------------------------------------------
// LOG GRAMMAR -- one line per event, prefix "uidemo: ".
// ---------------------------------------------------------------------
//
//   uidemo: open
//   uidemo: hover <widget>          -- widget name, or "none" on leaving
//   uidemo: press <widget>
//   uidemo: release <widget>        -- only when a press COMMITTED
//   uidemo: cancel <widget>         -- press dragged off, nothing acted
//   uidemo: click <widget>          -- act-on-contact widgets only
//   uidemo: button <n>
//   uidemo: check <name> <on|off>
//   uidemo: radio <name>
//   uidemo: focus <textbox|dropdown|listbox|none>
//   uidemo: key <code> text="<contents>"
//   uidemo: scroll <offset> <wheel|page|thumb|pan>
//   uidemo: list <n> <label>
//   uidemo: dropdown open | close | <n> <label>
//   uidemo: layout <widget> <x> <y> <w> <h>   -- emitted once on open
//   uidemo: layout listbox_row_h <px>
//
// Widget names are stable identifiers, not display labels: btn1, btn2,
// btn3, chk_alpha, chk_beta, radio, textbox, scrollback, dropdown,
// listbox, none.
#include <stdint.h>
#include "rt/sys.h"
#include "ui/ugfx.h"
#include "ui/uui.h"
#include "ui/uapp.h"
#include "ui/utheme.h"
#include "lib/stdio.h"
#include "lib/string.h"

#define PAD       10
#define ROW_GAP   8
#define BTN_COUNT 3
#define CHK_GAP   20

// Magenta, used for nothing else -- see draw()'s containment note. A
// test asserting "this colour is nowhere on screen" needs a colour no
// legitimate pixel can be.
#define OOB_MARKER ugfx_rgb(255, 0, 255)

// utheme.h carries four colours and no selection tint; this is the one
// uui_listbox picks for its own selected row, named here rather than
// repeated as a literal so the text view's selection matches the
// listbox's.
#define SEL_BG ugfx_rgb(205, 220, 240)
// Likewise the widgets' own border grey.
#define BORDER ugfx_rgb(160, 160, 165)

#define CHK_SIZE  (ugfx_char_h() - 2)
#define BTN_W     (6 * ugfx_char_w() + 16)
#define TBX_W     (24 * ugfx_char_w())
#define SCROLL_W  (34 * ugfx_char_w())
#define VIEW_W    (SCROLL_W + 12)
#define DD_W      (22 * ugfx_char_w())
#define LIST_W    (22 * ugfx_char_w())
#define LIST_ROWS 4

static const char *const BTN_LABELS[BTN_COUNT] = { "One", "Two", "Three" };
static const char *const RADIO_LABELS[] = { "Red", "Green", "Blue", "Grey" };
#define RADIO_COUNT 4

// Deliberately longer than what fits, so both controls always have a
// live scrollbar to exercise -- a scrollbar that never appears is a
// scrollbar that never gets tested.
static const char *const DD_ITEMS[] = {
    "Aardvark", "Badger", "Capybara", "Dormouse", "Echidna",
    "Ferret", "Gerbil", "Hedgehog", "Ibex", "Jackal",
};
#define DD_COUNT 10

static const char *const LIST_ITEMS[] = {
    "alpha", "bravo", "charlie", "delta", "echo", "foxtrot",
    "golf", "hotel", "india", "juliet", "kilo", "lima",
};
#define LIST_COUNT 12

// Stable identifiers -- deliberately NOT the display labels, so a test
// asserting on the log does not break when a label is reworded.
enum widget_id { W_NONE = 0, W_BTN1, W_BTN2, W_BTN3, W_CHK_ALPHA,
                 W_CHK_BETA, W_RADIO, W_TEXTBOX, W_SCROLLBACK,
                 W_DROPDOWN, W_LISTBOX };

static const char *const WIDGET_NAMES[] = {
    "none", "btn1", "btn2", "btn3", "chk_alpha", "chk_beta",
    "radio", "textbox", "scrollback", "dropdown", "listbox",
};

static struct {
    struct uui_button buttons[BTN_COUNT];
    struct uui_button_group group;
    struct uui_radio_list radio;
    struct uui_textbox textbox;
    struct uui_textview view;
    struct uui_dropdown dropdown;
    struct uui_listbox list;
    struct uui_checkbox chk[2];
    int radio_sel;
    int hover_name;
    int hover_x, hover_y;
    struct uui_focus focus;
    struct uui_focusable focus_items[3];
    int focus_was;
    int pressing;
    // Whether the press in progress actually armed a button.
    // uui_button_group_release() returns -1 for BOTH "nothing was armed"
    // and "armed then dragged off", and only the second is a cancel --
    // without this, every click on a checkbox also logged "cancel btn".
    int armed;
    char status[64];
} g;

// Row origins, top to bottom. Functions rather than constants because
// each depends on the live font metrics.
static int row_buttons(void)  { return PAD; }
static int btn_h(void)        { return ugfx_char_h() + 12; }
static int row_checks(void)   { return row_buttons() + btn_h() + ROW_GAP; }
static int row_radio(void)    { return row_checks() + ugfx_char_h() + 6 + ROW_GAP; }
static int radio_row_h(void)  { return ugfx_char_h() + 8; }
static int row_textbox(void)  { return row_radio() + 2 * radio_row_h() + ROW_GAP; }
static int tbx_h(void)        { return ugfx_char_h() + 10; }
static int row_dropdown(void) { return row_textbox() + tbx_h() + ROW_GAP; }
static int dd_h(void)         { return ugfx_char_h() + 10; }
static int row_list(void)     { return row_dropdown() + dd_h() + ROW_GAP; }
// Row height comes from the widget, not a local constant -- asking it is
// the only way the box height and its rows cannot disagree.
static int list_h(void)       { return LIST_ROWS * uui_listbox_row_h(&g.list) + 2; }
static int row_scroll(void)   { return row_list() + list_h() + ROW_GAP; }
static int scroll_h(void)     { return 5 * ugfx_char_h(); }
static int row_status(void)   { return row_scroll() + scroll_h() + ROW_GAP; }

static void logline(const char *msg) {
    char line[128];
    snprintf(line, sizeof line, "uidemo: %s\n", msg);
    sys_eprint(line);
}

static void set_status(const char *s) {
    strlcpy(g.status, s, sizeof g.status);
}

static void log_and_status(const char *msg) {
    logline(msg);
    set_status(msg);
}

// Names matching focus_items[]'s order, for the log.
static const char *const FOCUS_NAMES[] = { "textbox", "dropdown", "listbox" };

// Reports a focus change if one happened. The manager owns the state
// itself; this only reports, since a `focus` line per tick would drown
// the log a test asserts on.
static void log_focus(void) {
    int now = g.focus.current;
    if (now == g.focus_was) return;
    g.focus_was = now;
    char m[32];
    snprintf(m, sizeof m, "focus %s", now < 0 ? "none" : FOCUS_NAMES[now]);
    logline(m);
}

static void log_scroll(const char *how) {
    char m[48];
    snprintf(m, sizeof m, "scroll %d %s", g.view.tb.scroll_offset, how);
    log_and_status(m);
}

static void layout(void) {
    for (int i = 0; i < BTN_COUNT; i++) {
        uui_button_set_geometry(&g.buttons[i], PAD + i * (BTN_W + ROW_GAP),
                                row_buttons(), BTN_W, btn_h());
    }
    uui_checkbox_set_geometry(&g.chk[0], PAD, row_checks());
    uui_checkbox_set_geometry(&g.chk[1], PAD + g.chk[0].w + CHK_GAP, row_checks());
    uui_radio_list_set_geometry(&g.radio, PAD, row_radio());
    uui_textbox_set_geometry(&g.textbox, PAD, row_textbox(), TBX_W, tbx_h());
    uui_dropdown_set_geometry(&g.dropdown, PAD, row_dropdown(), DD_W, dd_h());
    g.list.x = PAD; g.list.y = row_list();
    g.list.w = LIST_W; g.list.h = list_h();
    uui_textview_set_geometry(&g.view, PAD, row_scroll(), VIEW_W, scroll_h());
}

// Reports every widget's rect, one line each.
//
// This app's whole job is to be a KNOWN TARGET, and a test that
// re-derives these offsets from font metrics is re-implementing layout()
// in Python -- which drifts silently the moment a row is added.
static void log_layout(void) {
    char m[96];
    struct { const char *name; int x, y, w, h; } rows[] = {
        { "btn1",       g.buttons[0].x, g.buttons[0].y, g.buttons[0].w, g.buttons[0].h },
        { "chk_alpha",  g.chk[0].x, g.chk[0].y, g.chk[0].w, g.chk[0].h },
        { "radio",      g.radio.x, g.radio.y, g.radio.w, g.radio.h },
        { "textbox",    g.textbox.x, g.textbox.y, g.textbox.w, g.textbox.h },
        { "dropdown",   g.dropdown.x, g.dropdown.y, g.dropdown.w, g.dropdown.h },
        { "listbox",    g.list.x, g.list.y, g.list.w, g.list.h },
        { "scrollback", g.view.x, g.view.y, g.view.w, g.view.h },
    };
    for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        snprintf(m, sizeof m, "layout %s %d %d %d %d",
                 rows[i].name, rows[i].x, rows[i].y, rows[i].w, rows[i].h);
        logline(m);
    }
    snprintf(m, sizeof m, "layout listbox_row_h %d", uui_listbox_row_h(&g.list));
    logline(m);
}

// Which widget is at this point? One function, used by press, click and
// hover alike, so all three agree by construction.
static enum widget_id widget_at(int cx, int cy) {
    // An OPEN dropdown popup is drawn on top of everything, so it is
    // tested first -- hit-testing has to be the reverse of draw order,
    // or the widget underneath claims a click that visibly landed on
    // the popup.
    if (g.dropdown.open && uui_listbox_hit(&g.dropdown.list, cx, cy) >= 0) return W_DROPDOWN;
    if (uui_dropdown_hit(&g.dropdown, cx, cy)) return W_DROPDOWN;
    // NOTE: _hit returns a ROW INDEX, not a boolean -- row 0 is falsy.
    if (uui_listbox_hit(&g.list, cx, cy) >= 0) return W_LISTBOX;

    for (int i = 0; i < BTN_COUNT; i++) {
        if (uui_hit(g.buttons[i].x, g.buttons[i].y,
                    g.buttons[i].w, g.buttons[i].h, cx, cy)) {
            return (enum widget_id)(W_BTN1 + i);
        }
    }
    if (uui_checkbox_hit(&g.chk[0], cx, cy)) return W_CHK_ALPHA;
    if (uui_checkbox_hit(&g.chk[1], cx, cy)) return W_CHK_BETA;
    if (uui_radio_list_hit(&g.radio, cx, cy) >= 0) return W_RADIO;
    if (uui_textbox_hit(&g.textbox, cx, cy)) return W_TEXTBOX;
    if (uui_textview_hit(&g.view, cx, cy)) return W_SCROLLBACK;
    return W_NONE;
}

static void on_size(int *w, int *h) {
    int content_w = 2 * PAD + VIEW_W;
    int radio_w = 2 * (10 * ugfx_char_w() + 24);
    if (radio_w + 2 * PAD > content_w) content_w = radio_w + 2 * PAD;
    *w = content_w;
    *h = row_status() + ugfx_char_h() + PAD;
}

static void on_open(struct uapp *a) {
    (void)a;
    for (int i = 0; i < BTN_COUNT; i++) {
        uui_button_init(&g.buttons[i], 0, 0, 0, 0, BTN_LABELS[i],
                        UTHEME_BUTTON_BG, UTHEME_TEXT, i + 1);
    }
    uui_button_group_init(&g.group, g.buttons, BTN_COUNT);

    uui_checkbox_init(&g.chk[0], PAD, 0, CHK_SIZE, "Alpha",
                      UTHEME_PANEL_BG, UTHEME_TEXT);
    uui_checkbox_init(&g.chk[1], PAD, 0, CHK_SIZE, "Beta",
                      UTHEME_PANEL_BG, UTHEME_TEXT);

    g.radio.options = RADIO_LABELS;
    g.radio.count = RADIO_COUNT;
    g.radio.cols = 2;
    g.radio.row_h = radio_row_h();
    g.radio.col_w = 10 * ugfx_char_w() + 24;
    g.radio.marker_size = ugfx_char_h() - 4;
    g.radio_sel = 0;

    uui_textbox_init(&g.textbox, "type here");

    uui_dropdown_init(&g.dropdown, 0, 0, 0, 0, DD_ITEMS, DD_COUNT);
    // Fewer rows than the popup would otherwise want, so the popup has a
    // scrollbar too rather than showing all ten at once.
    g.dropdown.max_rows = 5;

    uui_listbox_init(&g.list, 0, 0, 0, 0, LIST_ITEMS, LIST_COUNT);
    g.list.selected = 0;

    uui_textview_init(&g.view, PAD, row_scroll(), VIEW_W, scroll_h(),
                      UTHEME_TEXT, UTHEME_WHITE, UTHEME_PANEL_BG,
                      UTHEME_BUTTON_BG, SEL_BG);
    // Body drags PAN this view: UI Demo has no cursor or selection of
    // its own, so nothing conflicts, and it makes the third input route
    // demonstrable. Notepad keeps the default (body presses are the
    // app's).
    g.view.body = UUI_TEXTVIEW_BODY_PAN;
    // Numbered lines, so a test can read the scroll offset straight off
    // a screenshot and scrolling is visibly doing something.
    for (int i = 1; i <= 20; i++) {
        char line[32];
        snprintf(line, sizeof line, "line %d of 20", i);
        for (const char *c = line; *c; c++) utext_putc(&g.view.tb, *c);
        utext_putc(&g.view.tb, '\n');
    }

    g.chk[0].checked = g.chk[1].checked = 0;
    g.hover_name = W_NONE;
    g.armed = 0;
    // Tab order is array order. The checkbox and radio list are absent
    // on purpose: both are act-on-contact with no keyboard behaviour, so
    // a tab stop there would be a stop that does nothing.
    g.focus_items[0] = (struct uui_focusable){ &g.textbox,  &uui_textbox_focus_ops };
    g.focus_items[1] = (struct uui_focusable){ &g.dropdown, &uui_dropdown_focus_ops };
    g.focus_items[2] = (struct uui_focusable){ &g.list,     &uui_listbox_focus_ops };
    uui_focus_init(&g.focus, g.focus_items, 3);
    g.focus_was = -1;
    set_status("ready");
    layout();
    logline("open");
    log_layout();
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    (void)a;
    struct ugfx_surface *s = d->surface;
    ugfx_fill(s, UTHEME_PANEL_BG);

    // A DELIBERATE attempt to draw outside this window, every frame.
    //
    // Kernel-side, the WM clips to a window's content area around
    // on_draw(). A client cannot reach outside its own buffer at all --
    // it has no mapping of anything else -- and ugfx clips every write
    // to the surface. That is a STRONGER boundary, and this keeps
    // testing it: tools/uidemo_test.py asserts these pixels never
    // appear on screen, so a clipping regression in ugfx goes red the
    // same day rather than whenever an app next has a layout bug.
    ugfx_fill_rect(s, -40, -40, 30, 30, OOB_MARKER);
    ugfx_fill_rect(s, s->w + 10, s->h + 10, 30, 30, OOB_MARKER);

    layout();

    uui_button_group_draw(&g.group, s);

    g.chk[0].hovered = (g.hover_name == W_CHK_ALPHA);
    g.chk[1].hovered = (g.hover_name == W_CHK_BETA);
    uui_checkbox_draw(s, &g.chk[0]);
    uui_checkbox_draw(s, &g.chk[1]);

    // The hovered ROW, not just "is the list hovered" -- widget_at()
    // reports the widget, so ask the list itself which row that is.
    int radio_hot = (g.hover_name == W_RADIO)
        ? uui_radio_list_hit(&g.radio, g.hover_x, g.hover_y) : -1;
    uui_radio_list_draw(s, &g.radio, g.radio_sel, radio_hot,
                        UTHEME_PANEL_BG, UTHEME_TEXT);

    uui_textbox_draw(s, &g.textbox, UTHEME_WHITE, UTHEME_TEXT, BORDER);
    uui_dropdown_draw(s, &g.dropdown);   // the closed box only
    uui_listbox_draw(s, &g.list);
    uui_textview_draw(s, &g.view);

    ugfx_draw_string_clipped(s, PAD, row_status(), s->w - 2 * PAD, g.status,
                             UTHEME_TEXT, UTHEME_PANEL_BG);

    // LAST, after every other widget. Drawing is immediate-mode, so
    // z-order is call order: a popup drawn any earlier would be painted
    // over by the listbox and the text view below it.
    uui_dropdown_draw_popup(s, &g.dropdown);
}

static void on_motion(struct uapp *a, int cx, int cy, unsigned buttons) {
    layout();
    // With a button held this is a DRAG, which belongs to whatever
    // claimed the press.
    if (buttons) {
        if (g.pressing) {
            if (g.view.thumb_grab >= 0 || g.view.panning) {
                uui_textview_drag(&g.view, cx, cy);
                log_scroll(g.view.thumb_grab >= 0 ? "thumb" : "pan");
                uapp_redraw(a);
                return;
            }
            if (uui_button_group_press(&g.group, cx, cy)) uapp_redraw(a);
        }
        return;
    }

    int now = (cx < 0 || cy < 0) ? W_NONE : (int)widget_at(cx, cy);
    int moved = (cx != g.hover_x || cy != g.hover_y);
    g.hover_x = cx; g.hover_y = cy;
    int changed = uui_button_group_hover(&g.group, cx, cy);
    // Row-level hover inside these two is theirs to track: the name
    // logged below only says WHICH widget, and a listbox highlighting a
    // different row is a repaint the app would otherwise miss.
    if (uui_listbox_hover(&g.list, cx, cy)) changed = 1;
    if (now != g.hover_name) {
        g.hover_name = now;
        char m[32];
        snprintf(m, sizeof m, "hover %s", WIDGET_NAMES[now]);
        logline(m);
        changed = 1;
    } else if (moved && now == W_RADIO) {
        changed = 1;   // a move WITHIN the radio list changes the hot row
    }
    if (changed) uapp_redraw(a);
}

static void on_press(struct uapp *a, int cx, int cy, unsigned buttons) {
    (void)buttons;
    layout();
    g.pressing = 1;

    // Focus first, so a widget acting on this very press already has it.
    // uui_focus_click() only moves focus; it never consumes the press.
    if (uui_focus_click(&g.focus, cx, cy)) log_focus();

    enum widget_id w = widget_at(cx, cy);
    char m[64];

    // --- act-on-contact widgets --------------------------------------
    //
    // A checkbox toggle, a radio selection, a listbox row and a dropdown
    // are exactly the cases where acting on contact is correct. The
    // BUTTONS below deliberately do not act here.
    int was_open = g.dropdown.open;
    if (uui_dropdown_click(&g.dropdown, cx, cy)) {
        int sel = uui_dropdown_selected(&g.dropdown);
        if (was_open && !g.dropdown.open && sel >= 0 && w == W_DROPDOWN) {
            snprintf(m, sizeof m, "dropdown %d %s", sel, DD_ITEMS[sel]);
            log_and_status(m);
        } else if (!was_open && g.dropdown.open) {
            log_and_status("dropdown open");
        } else if (was_open && !g.dropdown.open) {
            log_and_status("dropdown close");
        }
        uapp_redraw(a);
        return;
    }
    // A press that merely DISMISSED an open popup is swallowed here, so
    // it cannot also act on whatever is underneath -- the same rule the
    // menu bar follows.
    if (was_open && !g.dropdown.open) {
        log_and_status("dropdown close");
        uapp_redraw(a);
        return;
    }

    if (w == W_LISTBOX) {
        if (uui_listbox_click(&g.list, cx, cy) && g.list.selected >= 0) {
            snprintf(m, sizeof m, "list %d %s", g.list.selected,
                     LIST_ITEMS[g.list.selected]);
            log_and_status(m);
        }
        uapp_redraw(a);
        return;
    }

    if (w == W_CHK_ALPHA || w == W_CHK_BETA) {
        int idx = (w == W_CHK_ALPHA) ? 0 : 1;
        uui_checkbox_toggle(&g.chk[idx]);
        snprintf(m, sizeof m, "check %s %s", idx ? "beta" : "alpha",
                 g.chk[idx].checked ? "on" : "off");
        log_and_status(m);
        uapp_redraw(a);
        return;
    }

    if (w == W_RADIO) {
        int hit = uui_radio_list_hit(&g.radio, cx, cy);
        if (hit >= 0) {
            g.radio_sel = hit;
            snprintf(m, sizeof m, "radio %s", RADIO_LABELS[hit]);
            log_and_status(m);
        }
        uapp_redraw(a);
        return;
    }

    if (w == W_TEXTBOX) {
        set_status("textbox focused");
        uapp_redraw(a);
        return;
    }

    // --- the text view: thumb drag, pan, or page ---------------------
    if (uui_textview_drag_start(&g.view, cx, cy)) { uapp_redraw(a); return; }
    if (uui_textview_click(&g.view, cx, cy)) { log_scroll("page"); uapp_redraw(a); return; }

    // --- buttons: arm now, commit on release -------------------------
    if (uui_button_group_press(&g.group, cx, cy)) uapp_redraw(a);
    for (int i = 0; i < BTN_COUNT; i++) {
        if (g.buttons[i].pressed) { g.armed = 1; break; }
    }
}

static void on_release(struct uapp *a, int cx, int cy, unsigned buttons) {
    (void)cx; (void)cy; (void)buttons;
    layout();
    g.pressing = 0;
    uui_textview_drag_end(&g.view);

    int code = uui_button_group_release(&g.group);
    if (code > 0) {
        char m[32];
        snprintf(m, sizeof m, "button %d", code);
        logline(m);
        snprintf(m, sizeof m, "release btn%d", code);
        log_and_status(m);
    } else if (g.armed) {
        log_and_status("cancel btn");
    }
    g.armed = 0;
    uapp_redraw(a);
}

static void on_wheel(struct uapp *a, int notches) {
    layout();
    // Same top-down order as press: an open popup is on top, so it takes
    // the wheel first.
    if (g.dropdown.open && uui_listbox_wheel(&g.dropdown.list, notches)) {
        uapp_redraw(a);
        return;
    }
    if (uui_listbox_wheel(&g.list, notches)) { uapp_redraw(a); return; }
    if (!uui_textview_wheel(&g.view, notches)) return;
    log_scroll("wheel");
    uapp_redraw(a);
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    char m[96];
    layout();
    // ONE call: the focus manager handles Tab/Shift-Tab itself and
    // routes everything else to the focused widget. Trying each widget
    // in turn is what this replaced -- the dropdown handles arrows even
    // while closed, so the listbox never saw one.
    int before = g.focus.current;
    int was_open = g.dropdown.open;
    if (uui_focus_key(&g.focus, key, mods)) {
        log_focus();
        int now = g.focus.current;
        if (now != before) {
            // A Tab: the focus line above already said what happened.
        } else if (now == 0) {
            snprintf(m, sizeof m, "key %d text=\"%s\"", key, g.textbox.buf);
            log_and_status(m);
        } else if (now == 1) {
            // A key that OPENED or CLOSED the popup is reported as that,
            // exactly as the mouse path reports it -- one grammar for
            // one event, whichever device caused it. Reporting the value
            // instead made "Down opens it" indistinguishable from "Down
            // changed the value".
            if (was_open != g.dropdown.open) {
                log_and_status(g.dropdown.open ? "dropdown open" : "dropdown close");
                uapp_redraw(a);
                return;
            }
            int sel = uui_dropdown_selected(&g.dropdown);
            if (sel >= 0) {
                snprintf(m, sizeof m, "dropdown %d %s", sel, DD_ITEMS[sel]);
                log_and_status(m);
            }
        } else if (now == 2) {
            if (g.list.selected >= 0) {
                snprintf(m, sizeof m, "list %d %s", g.list.selected,
                         LIST_ITEMS[g.list.selected]);
                log_and_status(m);
            }
        }
    } else {
        snprintf(m, sizeof m, "key %d (no focus)", key);
        log_and_status(m);
    }
    uapp_redraw(a);
}

int main(void) {
    struct uapp_desc desc = {
        .title      = "UI Demo",
        .on_size    = on_size,
        .on_open    = on_open,
        .on_draw    = on_draw,
        .on_press   = on_press,
        .on_release = on_release,
        .on_motion  = on_motion,
        .on_wheel   = on_wheel,
        .on_key     = on_key,
        .flags      = UAPP_RESIZABLE,
    };
    return uapp_run(&desc);
}
