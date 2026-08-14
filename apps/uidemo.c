// UI Demo -- the app that exists to be tested against.
//
// Every other app here exists to do something; this one exists to be a
// KNOWN TARGET. It shows one of each widget in apps/ui/, at fixed
// documented offsets, and reports every interaction as a single
// parseable line in the kernel log. That makes GUI testing an
// assertion on text rather than a reading of a screenshot: click a
// coordinate with `gui click`, then check the log said what you
// expected. It's also the calibration target -- if a click lands on
// the wrong widget, this app says which one it actually hit.
//
// ---------------------------------------------------------------------
// LAYOUT -- all coordinates CONTENT-RELATIVE (0,0 = top-left of the
// content area, i.e. just below the title bar).
// ---------------------------------------------------------------------
//
// Everything is derived from the font (gfx_char_w()/gfx_char_h()), like
// the rest of this GUI, so nothing here is a raw pixel constant that a
// `fontsize` change would invalidate. `gui windows --json` gives the
// content origin; add these offsets to it. The row constants below are
// the single source of truth -- draw and hit-test both read them, so
// they cannot disagree (the mistake the Control Panel's first version
// made, see docs/gui-guidelines.md).
//
//   ROW_BUTTONS   three ui_buttons in a ui_button_group: "One" "Two"
//                 "Three", codes 1/2/3. Hover and press states, commit
//                 on release.
//   ROW_CHECKS    two widget_checkboxes: "Alpha", "Beta". Toggle on
//                 click (act-on-contact is correct for these).
//   ROW_RADIO     a ui_radio_list, 2 columns: "Red" "Green" "Blue" "Grey".
//   ROW_TEXTBOX   a ui_textbox. Click to focus, then keys go to it.
//   ROW_DROPDOWN  a ui_dropdown, 10 items. Click to open the popup;
//                 arrows work while closed too. Its popup deliberately
//                 opens OVER the listbox below it -- that overlap is the
//                 point, it's what proves the popup is drawn last.
//   ROW_LIST      a ui_listbox, 12 items in a 4-row box, so it always
//                 has a scrollbar to exercise.
//   ROW_SCROLL    a ui_scrollback with a scrollbar beside it, prefilled
//                 with numbered lines so scroll position is readable.
//   ROW_STATUS    a live readout of the last event, so a screenshot is
//                 also self-describing.
//
// Keyboard focus is deliberately simple here: while the textbox is
// active it takes every key, and otherwise keys go to the dropdown and
// then the listbox. Click the textbox to focus it, click anywhere else
// to release it.
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
//   uidemo: button <n>              -- a button group button committed
//   uidemo: check <name> <on|off>
//   uidemo: radio <name>
//   uidemo: focus textbox
//   uidemo: key <code> text="<contents>"
//   uidemo: scroll <offset> <wheel|page|thumb>
//   uidemo: list <n> <label>        -- listbox selection committed
//   uidemo: dropdown open
//   uidemo: dropdown close          -- closed with the value unchanged
//   uidemo: dropdown <n> <label>    -- dropdown value CHANGED
//   uidemo: layout <widget> <x> <y> <w> <h>   -- emitted once on open
//   uidemo: layout listbox_row_h <px>
//
// Widget names are stable identifiers, not display labels: btn1, btn2,
// btn3, chk_alpha, chk_beta, radio, textbox, scrollback, dropdown,
// listbox, none.
//
// These go through klog_write(), so they land on the serial console
// alongside the `gui` debug commands (apps/wm/wm_debug.c) and in
// `dmesg`. A test can therefore drive and observe over one wire.
#include "uidemo.h"
#include "wm/wm.h"
#include "ui/ui.h"
#include "theme.h"
#include "kapi.h"

#define PAD        10
#define ROW_GAP    8
#define BTN_COUNT  3
#define CHK_SIZE   (gfx_char_h() - 2)

// Row origins, top to bottom. Functions rather than constants because
// each depends on the live font metrics.
static int row_buttons(void) { return PAD; }
static int btn_h(void)       { return gfx_char_h() + 12; }
static int row_checks(void)  { return row_buttons() + btn_h() + ROW_GAP; }
static int row_radio(void)   { return row_checks() + gfx_char_h() + 6 + ROW_GAP; }
static int radio_row_h(void) { return gfx_char_h() + 8; }
static int row_textbox(void) { return row_radio() + 2 * radio_row_h() + ROW_GAP; }
static int tbx_h(void)       { return gfx_char_h() + 10; }
static int row_dropdown(void) { return row_textbox() + tbx_h() + ROW_GAP; }
static int dd_h(void)        { return gfx_char_h() + 10; }
static int row_list(void)    { return row_dropdown() + dd_h() + ROW_GAP; }
// Row height comes from the widget, not from a local constant -- asking
// it is the only way the box height and its rows can't disagree. Safe
// before uidemo_open(): g_state is static, so `row_h` is 0 there and
// ui_listbox_row_h() falls back to the same font-derived default it
// will use after init.
static int list_h(void);
static int row_scroll(void)  { return row_list() + list_h() + ROW_GAP; }
static int scroll_h(void)    { return 5 * gfx_char_h(); }
static int row_status(void)  { return row_scroll() + scroll_h() + ROW_GAP; }

#define BTN_W (6 * gfx_char_w() + 16)
#define TBX_W (24 * gfx_char_w())
#define SCROLL_W (34 * gfx_char_w())
#define SCROLLBAR_W 12
#define VIEW_W (SCROLL_W + SCROLLBAR_W)
#define DD_W (22 * gfx_char_w())
#define LIST_W (22 * gfx_char_w())
#define LIST_ROWS 4

static const char *const BTN_LABELS[BTN_COUNT] = { "One", "Two", "Three" };
static const char *const RADIO_LABELS[] = { "Red", "Green", "Blue", "Grey" };
#define RADIO_COUNT 4

// Deliberately longer than what fits, so both controls always have a
// live scrollbar to exercise -- a scrollbar that never appears is a
// scrollbar that never gets tested (see ui_textview.h on the last time
// that happened in this very app).
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

struct uidemo_state {
    struct ui_button buttons[BTN_COUNT];
    struct ui_button_group group;
    struct ui_radio_list radio;
    struct ui_textbox textbox;
    struct ui_textview view;   // scrollback + scrollbar + all its input handling
    struct ui_dropdown dropdown;
    struct ui_listbox list;
    int checked[2];
    int radio_sel;
    int hover_name;      // index into WIDGET_NAMES, or -1
    int hover_x, hover_y; // last hover point, for widgets whose hover is per-ROW rather than per-widget
    // Keyboard focus, via the shared manager (apps/ui/ui_focus.h).
    // Four widgets here take keys, and without a focus concept the first
    // one in a try-each-in-turn chain swallows every key it recognises
    // -- the dropdown handles arrows even while CLOSED, so the listbox
    // under it could never be arrowed at all. Tab and Shift-Tab cycle;
    // clicking focuses.
    struct ui_focus focus;
    struct ui_focusable focus_items[4];
    int focus_was;       // last focus index logged, so only changes are reported
    int pressing;        // a press is in progress; on_press fires every tick, focus must move only on the first
    // Whether the press currently in progress actually armed a button.
    // ui_button_group_release() returns -1 for BOTH "nothing was ever
    // armed" and "armed then dragged off", and only the second is a
    // cancel -- without this every click on a checkbox also logged
    // "cancel btn", which is exactly the sort of noise a log-asserting
    // test trips over.
    int armed;
    char status[64];
};

static struct uidemo_state g_state;

static int list_h(void) {
    return ui_listbox_height_for_rows(&g_state.list, LIST_ROWS);
}

// Stable identifiers -- deliberately NOT the display labels, so a test
// asserting on the log doesn't break when a label is reworded.
enum widget_id { W_NONE = 0, W_BTN1, W_BTN2, W_BTN3, W_CHK_ALPHA,
                  W_CHK_BETA, W_RADIO, W_TEXTBOX, W_SCROLLBACK,
                  W_DROPDOWN, W_LISTBOX };

static const char *const WIDGET_NAMES[] = {
    "none", "btn1", "btn2", "btn3", "chk_alpha", "chk_beta",
    "radio", "textbox", "scrollback", "dropdown", "listbox",
};

static void logline(const char *fmt_done) {
    klog_write("uidemo: ");
    klog_write(fmt_done);
    klog_write("\n");
}

static void set_status(const char *s) {
    k_strcpy(g_state.status, s);
}

// Names matching focus_items[]'s order, for the log.
static const char *const FOCUS_NAMES[] = { "buttons", "textbox", "dropdown", "listbox" };

// Logs a focus change if one happened. The manager owns the focus state
// itself (including telling the textbox to show its caret), so this only
// reports -- a `focus` line per tick would drown the log a test asserts
// on.
static void log_focus(void) {
    int now = ui_focus_index(&g_state.focus);
    if (now == g_state.focus_was) return;
    g_state.focus_was = now;
    klog_write("uidemo: focus ");
    klog_write(now < 0 ? "none" : FOCUS_NAMES[now]);
    klog_write("\n");
}

static void log_scroll(const char *how) {
    char msg[48];
    k_snprintf(msg, sizeof msg, "scroll %d %s", g_state.view.tb.scroll_offset, how);
    logline(msg);
    set_status(msg);
}

// Which widget is at this content-relative point? One function, used by
// press, click and hover alike, so all three agree by construction.
static enum widget_id widget_at(int cx, int cy) {
    // An OPEN dropdown popup is drawn on top of everything, so it is
    // tested first -- hit-testing has to be the reverse of draw order or
    // the widget underneath claims a click that visibly landed on the
    // list.
    if (ui_dropdown_popup_hit(&g_state.dropdown, cx, cy)) return W_DROPDOWN;
    if (ui_dropdown_hit(&g_state.dropdown, cx, cy)) return W_DROPDOWN;
    if (ui_listbox_hit(&g_state.list, cx, cy)) return W_LISTBOX;

    for (int i = 0; i < BTN_COUNT; i++) {
        if (ui_button_hit(&g_state.buttons[i], cx, cy)) return (enum widget_id)(W_BTN1 + i);
    }
    int cy0 = row_checks();
    if (widget_checkbox_hit(PAD, cy0, CHK_SIZE, "Alpha", cx, cy)) return W_CHK_ALPHA;
    int beta_x = PAD + widget_checkbox_width(CHK_SIZE, "Alpha") + 20;
    if (widget_checkbox_hit(beta_x, cy0, CHK_SIZE, "Beta", cx, cy)) return W_CHK_BETA;

    if (ui_radio_list_hit(&g_state.radio, PAD, row_radio(), cx, cy) >= 0) return W_RADIO;
    if (ui_textbox_hit(&g_state.textbox, cx, cy)) return W_TEXTBOX;

    if (ui_textview_hit(&g_state.view, cx, cy)) return W_SCROLLBACK;
    return W_NONE;
}

// Reports every widget's content-relative rect, one line each.
//
// This app's whole job is to be a KNOWN TARGET, and a test that has to
// re-derive these offsets from the font metrics is re-implementing
// layout() in Python -- which drifts silently the moment a row is
// added, exactly as happened when the dropdown and listbox rows went in
// between the textbox and the scrollback. Emitted on open (and by
// `layout` below on demand), so a test asks rather than assumes, the
// same reason `gui windows` exists instead of measuring a screenshot.
//
//   uidemo: layout <widget> <x> <y> <w> <h>
static void log_layout(void) {
    char m[80];
    struct { const char *name; int x, y, w, h; } rows[] = {
        { "btn1",      g_state.buttons[0].x, g_state.buttons[0].y,
                       g_state.buttons[0].w, g_state.buttons[0].h },
        { "chk_alpha", PAD, row_checks(),
                       widget_checkbox_width(CHK_SIZE, "Alpha"), CHK_SIZE },
        { "radio",     PAD, row_radio(), 2 * g_state.radio.col_w,
                       2 * radio_row_h() },
        { "textbox",   g_state.textbox.x, g_state.textbox.y,
                       g_state.textbox.w, g_state.textbox.h },
        { "dropdown",  g_state.dropdown.x, g_state.dropdown.y,
                       g_state.dropdown.w, g_state.dropdown.h },
        { "listbox",   g_state.list.x, g_state.list.y,
                       g_state.list.w, g_state.list.h },
        { "scrollback", g_state.view.x, g_state.view.y,
                        g_state.view.w, g_state.view.h },
    };
    for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        k_snprintf(m, sizeof m, "layout %s %d %d %d %d",
                   rows[i].name, rows[i].x, rows[i].y, rows[i].w, rows[i].h);
        logline(m);
    }
    // The listbox's row height is what a test needs to click row N, and
    // it is the widget's to report, not the app's to assume.
    k_snprintf(m, sizeof m, "layout listbox_row_h %d", ui_listbox_row_h(&g_state.list));
    logline(m);
}

void uidemo_default_size(int *w, int *h) {
    int content_w = 2 * PAD + SCROLL_W + SCROLLBAR_W;
    int radio_w = 2 * (10 * gfx_char_w() + 24);
    if (radio_w + 2 * PAD > content_w) content_w = radio_w + 2 * PAD;
    *w = content_w;
    *h = row_status() + gfx_char_h() + PAD;
}

static void layout(void) {
    for (int i = 0; i < BTN_COUNT; i++) {
        ui_button_set_geometry(&g_state.buttons[i],
                                PAD + i * (BTN_W + ROW_GAP), row_buttons(),
                                BTN_W, btn_h());
    }
    ui_textbox_set_geometry(&g_state.textbox, PAD, row_textbox(), TBX_W, tbx_h());
    ui_dropdown_set_geometry(&g_state.dropdown, PAD, row_dropdown(), DD_W, dd_h());
    ui_listbox_set_geometry(&g_state.list, PAD, row_list(), LIST_W, list_h());
    ui_textview_set_geometry(&g_state.view, PAD, row_scroll(), VIEW_W, scroll_h());
}

void uidemo_open(struct window *win) {
    for (int i = 0; i < BTN_COUNT; i++) {
        ui_button_init(&g_state.buttons[i], 0, 0, 0, 0, BTN_LABELS[i],
                        THEME_BUTTON_BG, THEME_TEXT, i + 1);
    }
    ui_button_group_init(&g_state.group, g_state.buttons, BTN_COUNT);

    g_state.radio.options = RADIO_LABELS;
    g_state.radio.count = RADIO_COUNT;
    g_state.radio.cols = 2;
    g_state.radio.row_h = radio_row_h();
    g_state.radio.col_w = 10 * gfx_char_w() + 24;
    g_state.radio.marker_size = gfx_char_h() - 4;
    g_state.radio_sel = 0;

    ui_textbox_init(&g_state.textbox, 0, 0, 0, 0, "type here",
                     THEME_WHITE, THEME_TEXT, THEME_BORDER);

    ui_dropdown_init(&g_state.dropdown, 0, 0, 0, 0, DD_ITEMS, DD_COUNT, 0,
                      THEME_BUTTON_BG, THEME_TEXT, THEME_BORDER,
                      THEME_SELECTION_BG, THEME_WHITE,
                      THEME_PANEL_BG, THEME_BORDER);
    // Fewer rows than the popup would otherwise want, so the popup has a
    // scrollbar too rather than just showing all ten at once.
    g_state.dropdown.max_rows = 5;

    ui_listbox_init(&g_state.list, 0, 0, 0, 0, LIST_ITEMS, LIST_COUNT,
                     THEME_WHITE, THEME_TEXT, THEME_SELECTION_BG, THEME_WHITE,
                     THEME_PANEL_BG, THEME_BORDER);
    ui_listbox_set_selected(&g_state.list, 0);

    ui_textview_init(&g_state.view, PAD, row_scroll(), VIEW_W, scroll_h(),
                      THEME_WHITE, THEME_PANEL_BG, THEME_BORDER, THEME_SELECTION_BG);
    // Body drags pan this view: UI Demo has no cursor or selection of
    // its own, so there is nothing for a body press to conflict with,
    // and it makes the third input route demonstrable. Notepad and
    // Terminal keep the default (body presses belong to the app).
    g_state.view.body = UI_TEXTVIEW_BODY_PAN;
    widget_scrollback_set_color(&g_state.view.tb, VGA_BLACK);
    // Numbered lines so a test can read the scroll offset straight off
    // a screenshot, and so scrolling is visibly doing something.
    for (int i = 1; i <= 20; i++) {
        char line[32];
        k_snprintf(line, sizeof line, "line %u of 20", (unsigned)i);
        for (const char *c = line; *c; c++) widget_scrollback_putc(&g_state.view.tb, *c);
        widget_scrollback_putc(&g_state.view.tb, '\n');
    }

    g_state.checked[0] = g_state.checked[1] = 0;
    g_state.hover_name = W_NONE;
    g_state.armed = 0;
    // Tab order is array order -- see ui_focus.h on why that's the whole
    // ordering mechanism. The checkbox and radio list are absent on
    // purpose: both are act-on-contact and have no keyboard behaviour of
    // their own, so a tab stop there would be a stop that does nothing.
    g_state.focus_items[0] = (struct ui_focusable){ &g_state.group,    &ui_button_group_focus_ops };
    g_state.focus_items[1] = (struct ui_focusable){ &g_state.textbox,  &ui_textbox_focus_ops };
    g_state.focus_items[2] = (struct ui_focusable){ &g_state.dropdown, &ui_dropdown_focus_ops };
    g_state.focus_items[3] = (struct ui_focusable){ &g_state.list,     &ui_listbox_focus_ops };
    ui_focus_init(&g_state.focus, g_state.focus_items, 4, THEME_SELECTION_BG);
    g_state.focus_was = -1;
    set_status("ready");
    layout();
    window_set_state(win, &g_state);
    logline("open");
    log_layout();
}

void uidemo_draw(struct window *win) {
    int cx = window_content_x(win), cy = window_content_y(win);
    int cw = window_content_w(win), ch = window_content_h(win);
    gfx_fill_rect(cx, cy, cw, ch, THEME_WINDOW_BG);
    layout();

    ui_button_group_draw(&g_state.group, cx, cy);

    int chk_y = cy + row_checks();
    widget_checkbox_draw(cx + PAD, chk_y, CHK_SIZE, g_state.checked[0],
                          g_state.hover_name == W_CHK_ALPHA, "Alpha",
                          THEME_WINDOW_BG, THEME_TEXT);
    int beta_x = cx + PAD + widget_checkbox_width(CHK_SIZE, "Alpha") + 20;
    widget_checkbox_draw(beta_x, chk_y, CHK_SIZE, g_state.checked[1],
                          g_state.hover_name == W_CHK_BETA, "Beta",
                          THEME_WINDOW_BG, THEME_TEXT);

    // The hovered ROW, not just "is the list hovered" -- widget_at()
    // reports the widget, so ask the list itself which row that is.
    int radio_hot = (g_state.hover_name == W_RADIO)
        ? ui_radio_list_hit(&g_state.radio, PAD, row_radio(),
                             g_state.hover_x, g_state.hover_y)
        : -1;
    ui_radio_list_draw(&g_state.radio, cx + PAD, cy + row_radio(), g_state.radio_sel,
                        radio_hot, THEME_WINDOW_BG, THEME_TEXT, THEME_SELECTION_BG);

    ui_textbox_draw(&g_state.textbox, cx, cy);

    ui_dropdown_draw(&g_state.dropdown, cx, cy);   // the closed box only
    ui_listbox_draw(&g_state.list, cx, cy);

    ui_textview_draw(&g_state.view, cx, cy);

    // Clipped, like everything else in a fixed box -- the status string
    // is short but the rule is the rule (docs/gui-guidelines.md).
    gfx_draw_string_clipped(cx + PAD, cy + row_status(), cw - 2 * PAD,
                             g_state.status, THEME_TEXT, THEME_WINDOW_BG);

    // LAST, after every other widget. Drawing here is immediate-mode, so
    // z-order is call order and a popup drawn any earlier would be
    // painted over by the listbox and scrollback below it. A no-op while
    // the dropdown is closed -- see ui_dropdown.h.
    ui_dropdown_draw_popup(&g_state.dropdown, cx, cy);

    // The focus ring goes last for the same reason the popup does --
    // anything drawn after it would paint over it.
    ui_focus_draw_ring(&g_state.focus, cx, cy);
}

int uidemo_hover(struct window *win, int cx, int cy) {
    (void)win;
    layout();
    int now = (cx < 0 || cy < 0) ? W_NONE : (int)widget_at(cx, cy);
    // Kept for the radio list, whose hover is per-row: widget_at() only
    // answers WHICH widget, and the row has to be asked of the list.
    int moved = (cx != g_state.hover_x || cy != g_state.hover_y);
    g_state.hover_x = cx; g_state.hover_y = cy;
    int group_changed = ui_button_group_hover(&g_state.group, cx, cy);
    // Row-level hover inside these two is theirs to track -- the name
    // logged below only says WHICH widget, and a listbox highlighting a
    // different row is a repaint the app would otherwise miss.
    if (ui_dropdown_hover(&g_state.dropdown, cx, cy)) group_changed = 1;
    if (ui_listbox_hover(&g_state.list, cx, cy)) group_changed = 1;
    // A move WITHIN the radio list can change the hovered row without
    // changing the hovered widget, so that has to repaint too.
    if (now == g_state.hover_name) {
        return group_changed || (moved && now == W_RADIO);
    }
    g_state.hover_name = now;
    klog_write("uidemo: hover ");
    klog_write(WIDGET_NAMES[now]);
    klog_write("\n");
    return 1;
}

int uidemo_press(struct window *win, int cx, int cy) {
    (void)win;
    layout();

    // The dropdown gets first refusal, because its popup is drawn on top
    // of everything -- input order is the reverse of draw order. Note it
    // deliberately does NOT consume a press that merely dismisses an
    // open popup, so that click still reaches whatever it landed on;
    // the repaint is still needed either way, hence `was_open`.
    // Focus first, so a widget that acts on this very press already has
    // it -- ui_focus_click() only moves focus, it never consumes the
    // press (see ui_focus.h). Only on the FIRST tick of a press: it is
    // called every tick while held, and re-running it mid-drag would
    // move focus to whatever the cursor had wandered over.
    int changed = 0;
    if (!g_state.pressing) {
        g_state.pressing = 1;
        if (ui_focus_click(&g_state.focus, cx, cy)) { changed = 1; log_focus(); }
    }

    int was_open = g_state.dropdown.open;
    if (ui_dropdown_press(&g_state.dropdown, cx, cy)) return 1;
    if (was_open != g_state.dropdown.open) changed = 1;

    if (ui_listbox_press(&g_state.list, cx, cy)) return 1;

    if (ui_button_group_press(&g_state.group, cx, cy)) changed = 1;
    for (int i = 0; i < BTN_COUNT; i++) {
        if (g_state.buttons[i].pressed) { g_state.armed = 1; break; }
    }
    return changed;
}

// Commit on release, per docs/gui-guidelines.md -- and the log says
// explicitly whether a press committed or was cancelled, which is the
// distinction a cancel-path test needs to see.
void uidemo_release(struct window *win) {
    layout();
    char m[64];

    // Both of these are safe to call unconditionally -- each returns -1
    // when it had nothing armed, which is exactly the "did the user pick
    // something" question an app wants answered.
    int was_open = g_state.dropdown.open;
    int picked = ui_dropdown_release(&g_state.dropdown);
    if (picked >= 0) {
        k_snprintf(m, sizeof m, "dropdown %u %s", (unsigned)picked, DD_ITEMS[picked]);
        logline(m);
        set_status(m);
    } else if (!was_open && g_state.dropdown.open) {
        logline("dropdown open");
        set_status("dropdown open");
    } else if (was_open && !g_state.dropdown.open) {
        logline("dropdown close");
        set_status("dropdown closed");
    }

    int row = ui_listbox_release(&g_state.list);
    if (row >= 0) {
        k_snprintf(m, sizeof m, "list %u %s", (unsigned)row, LIST_ITEMS[row]);
        logline(m);
        set_status(m);
    }

    // A keyboard activation (Space/Enter on the focused button) arrives
    // through the same path as a mouse release, so the logging below
    // handles both with no second branch -- see ui_button_group.h.
    g_state.pressing = 0;
    int code = ui_button_group_release(&g_state.group);
    if (code < 0) code = ui_button_group_take_activated(&g_state.group);
    if (code > 0) {
        char msg[32];
        k_snprintf(msg, sizeof msg, "button %u", (unsigned)code);
        logline(msg);
        k_snprintf(msg, sizeof msg, "release btn%u", (unsigned)code);
        logline(msg);
        set_status(msg);
    } else if (g_state.armed) {
        logline("cancel btn");
        set_status("press cancelled");
    }
    g_state.armed = 0;
    ui_textview_drag_end(&g_state.view);
    window_invalidate(win);
}

// Act-on-contact widgets only: a checkbox toggle, a radio selection and
// placing a text cursor are exactly the cases gui_apps.h's on_click is
// for. The buttons above deliberately do NOT act here.
void uidemo_click(struct window *win, int cx, int cy) {
    layout();
    enum widget_id w = widget_at(cx, cy);
    char msg[64];

    if (w == W_CHK_ALPHA || w == W_CHK_BETA) {
        int idx = (w == W_CHK_ALPHA) ? 0 : 1;
        g_state.checked[idx] = !g_state.checked[idx];
        k_snprintf(msg, sizeof msg, "check %s %s",
                   idx ? "beta" : "alpha", g_state.checked[idx] ? "on" : "off");
        logline(msg);
        set_status(msg);
    } else if (w == W_RADIO) {
        int hit = ui_radio_list_hit(&g_state.radio, PAD, row_radio(), cx, cy);
        if (hit >= 0) {
            g_state.radio_sel = hit;
            k_snprintf(msg, sizeof msg, "radio %s", RADIO_LABELS[hit]);
            logline(msg);
            set_status(msg);
        }
    } else if (w == W_SCROLLBACK) {
        // Track paging. A thumb press was claimed by drag_start() and
        // never reaches on_click; a body press pans, likewise.
        if (ui_textview_click(&g_state.view, cx, cy)) log_scroll("page");
    } else if (w == W_TEXTBOX) {
        // Focus itself was already moved by ui_focus_click() in on_press
        // (which also activated the caret via the textbox's set_focused);
        // this only reports it.
        set_status("textbox focused");
    }
    window_invalidate(win);
}

// --- scrolling -------------------------------------------------------
//
// All three routes a real scrollbar has, because a scrollbar that only
// draws is a decoration: the wheel, clicking the track to page, and
// dragging the thumb. The first version of this app had none of them --
// it drew the bar and left it inert, while this file's own log grammar
// advertised a `scroll` event it never emitted.

void uidemo_wheel(struct window *win, int delta) {
    layout();
    // Same top-down order as press: an open popup is on top, so it takes
    // the wheel first. A CLOSED dropdown ignores it on purpose (see
    // ui_dropdown.h), so this falls through to the listbox as it should.
    if (ui_dropdown_wheel(&g_state.dropdown, delta)) { window_invalidate(win); return; }
    if (ui_listbox_wheel(&g_state.list, delta)) { window_invalidate(win); return; }
    if (!ui_textview_wheel(&g_state.view, delta)) return;
    log_scroll("wheel");
    window_invalidate(win);
}

// Only a thumb hit claims the drag. A track click is act-on-contact
// paging and is handled in uidemo_click() instead -- the same split
// notepad.c uses, and the reason on_drag_start returns 0 there.
int uidemo_drag_start(struct window *win, int cx, int cy) {
    (void)win;
    layout();
    return ui_textview_drag_start(&g_state.view, cx, cy);
}

void uidemo_drag(struct window *win, int cx, int cy) {
    layout();
    ui_textview_drag(&g_state.view, cx, cy);
    log_scroll(g_state.view.thumb_grab >= 0 ? "thumb" : "pan");
    window_invalidate(win);
}

void uidemo_key(struct window *win, int key, uint8_t mods) {
    char msg[96];
    layout();
    // ONE call: the focus manager handles Tab/Shift-Tab itself and routes
    // everything else to the focused widget. Dispatching by trying each
    // widget in turn is what this replaced -- the dropdown handles arrows
    // even while closed, so the listbox never saw one.
    int before = ui_focus_index(&g_state.focus);
    if (ui_focus_key(&g_state.focus, key, mods)) {
        log_focus();
        // A keyboard activation (Space/Enter on the focused button) is
        // RECORDED by the group rather than acted on inside its key
        // handler, so it has to be collected here too -- on_release only
        // fires for a mouse press, and a key path that never collected it
        // meant Space lit nothing at all. Same call, same handling as the
        // mouse path in on_release; see ui_button_group.h.
        int act = ui_button_group_take_activated(&g_state.group);
        if (act > 0) {
            k_snprintf(msg, sizeof msg, "button %u", (unsigned)act);
            logline(msg);
            set_status(msg);
        }
        int now = ui_focus_index(&g_state.focus);
        if (now != before) {
            // A Tab: the focus line above already said what happened.
        } else if (now == 1) {
            k_snprintf(msg, sizeof msg, "key %u text=\"%s\"",
                       (unsigned)key, g_state.textbox.field.buf);
            logline(msg);
            set_status(msg);
        } else if (now == 2) {
            int sel = ui_dropdown_selected(&g_state.dropdown);
            if (sel >= 0) {
                k_snprintf(msg, sizeof msg, "dropdown %u %s", (unsigned)sel, DD_ITEMS[sel]);
                logline(msg);
                set_status(msg);
            }
        } else if (now == 3) {
            int sel = g_state.list.selected;
            if (sel >= 0) {
                k_snprintf(msg, sizeof msg, "list %u %s", (unsigned)sel, LIST_ITEMS[sel]);
                logline(msg);
                set_status(msg);
            }
        }
    } else {
        k_snprintf(msg, sizeof msg, "key %u (no focus)", (unsigned)key);
        logline(msg);
        set_status(msg);
    }
    window_invalidate(win);
}
