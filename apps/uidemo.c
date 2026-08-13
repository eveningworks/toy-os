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
//   ROW_SCROLL    a ui_scrollback with a scrollbar beside it, prefilled
//                 with numbered lines so scroll position is readable.
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
//   uidemo: button <n>              -- a button group button committed
//   uidemo: check <name> <on|off>
//   uidemo: radio <name>
//   uidemo: focus textbox
//   uidemo: key <code> text="<contents>"
//   uidemo: scroll <offset>
//
// Widget names are stable identifiers, not display labels: btn1, btn2,
// btn3, chk_alpha, chk_beta, radio, textbox, scrollback, none.
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
static int row_scroll(void)  { return row_textbox() + tbx_h() + ROW_GAP; }
static int scroll_h(void)    { return 5 * gfx_char_h(); }
static int row_status(void)  { return row_scroll() + scroll_h() + ROW_GAP; }

#define BTN_W (6 * gfx_char_w() + 16)
#define TBX_W (24 * gfx_char_w())
#define SCROLL_W (34 * gfx_char_w())
#define SCROLLBAR_W 12

static const char *const BTN_LABELS[BTN_COUNT] = { "One", "Two", "Three" };
static const char *const RADIO_LABELS[] = { "Red", "Green", "Blue", "Grey" };
#define RADIO_COUNT 4

struct uidemo_state {
    struct ui_button buttons[BTN_COUNT];
    struct ui_button_group group;
    struct ui_radio_list radio;
    struct ui_textbox textbox;
    struct text_scrollback log;
    int checked[2];
    int radio_sel;
    int hover_name;      // index into WIDGET_NAMES, or -1
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

// Stable identifiers -- deliberately NOT the display labels, so a test
// asserting on the log doesn't break when a label is reworded.
enum widget_id { W_NONE = 0, W_BTN1, W_BTN2, W_BTN3, W_CHK_ALPHA,
                  W_CHK_BETA, W_RADIO, W_TEXTBOX, W_SCROLLBACK };

static const char *const WIDGET_NAMES[] = {
    "none", "btn1", "btn2", "btn3", "chk_alpha", "chk_beta",
    "radio", "textbox", "scrollback",
};

static void logline(const char *fmt_done) {
    klog_write("uidemo: ");
    klog_write(fmt_done);
    klog_write("\n");
}

static void set_status(const char *s) {
    k_strcpy(g_state.status, s);
}

// Which widget is at this content-relative point? One function, used by
// press, click and hover alike, so all three agree by construction.
static enum widget_id widget_at(int cx, int cy) {
    for (int i = 0; i < BTN_COUNT; i++) {
        if (ui_button_hit(&g_state.buttons[i], cx, cy)) return (enum widget_id)(W_BTN1 + i);
    }
    int cy0 = row_checks();
    if (widget_checkbox_hit(PAD, cy0, CHK_SIZE, "Alpha", cx, cy)) return W_CHK_ALPHA;
    int beta_x = PAD + widget_checkbox_width(CHK_SIZE, "Alpha") + 20;
    if (widget_checkbox_hit(beta_x, cy0, CHK_SIZE, "Beta", cx, cy)) return W_CHK_BETA;

    if (ui_radio_list_hit(&g_state.radio, PAD, row_radio(), cx, cy) >= 0) return W_RADIO;
    if (ui_textbox_hit(&g_state.textbox, cx, cy)) return W_TEXTBOX;

    if (cx >= PAD && cx < PAD + SCROLL_W + SCROLLBAR_W &&
        cy >= row_scroll() && cy < row_scroll() + scroll_h()) return W_SCROLLBACK;
    return W_NONE;
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

    widget_scrollback_init(&g_state.log);
    widget_scrollback_set_color(&g_state.log, VGA_BLACK);
    // Numbered lines so a test can read the scroll offset straight off
    // a screenshot, and so scrolling is visibly doing something.
    for (int i = 1; i <= 20; i++) {
        char line[32];
        k_snprintf(line, sizeof line, "line %u of 20", (unsigned)i);
        for (const char *c = line; *c; c++) widget_scrollback_putc(&g_state.log, *c);
        widget_scrollback_putc(&g_state.log, '\n');
    }

    g_state.checked[0] = g_state.checked[1] = 0;
    g_state.hover_name = W_NONE;
    g_state.armed = 0;
    set_status("ready");
    layout();
    window_set_state(win, &g_state);
    logline("open");
}

void uidemo_draw(struct window *win) {
    int cx = window_content_x(win), cy = window_content_y(win);
    int cw = window_content_w(win), ch = window_content_h(win);
    gfx_fill_rect(cx, cy, cw, ch, THEME_WINDOW_BG);
    layout();

    ui_button_group_draw(&g_state.group, cx, cy);

    int chk_y = cy + row_checks();
    widget_checkbox_draw(cx + PAD, chk_y, CHK_SIZE, g_state.checked[0], "Alpha",
                          THEME_WINDOW_BG, THEME_TEXT);
    int beta_x = cx + PAD + widget_checkbox_width(CHK_SIZE, "Alpha") + 20;
    widget_checkbox_draw(beta_x, chk_y, CHK_SIZE, g_state.checked[1], "Beta",
                          THEME_WINDOW_BG, THEME_TEXT);

    ui_radio_list_draw(&g_state.radio, cx + PAD, cy + row_radio(), g_state.radio_sel,
                        THEME_WINDOW_BG, THEME_TEXT, THEME_SELECTION_BG);

    ui_textbox_draw(&g_state.textbox, cx, cy);

    int sx = cx + PAD, sy = cy + row_scroll();
    gfx_fill_rect(sx, sy, SCROLL_W, scroll_h(), THEME_WHITE);
    widget_scrollback_draw(&g_state.log, sx, sy, SCROLL_W, scroll_h(),
                            THEME_WHITE, THEME_SELECTION_BG, 0);
    int total = 0, visible = 0;
    widget_scrollback_metrics(&g_state.log, SCROLL_W, scroll_h(), &total, &visible);
    widget_scrollbar_draw(sx + SCROLL_W, sy, SCROLLBAR_W, scroll_h(),
                           total, visible, g_state.log.scroll_offset,
                           THEME_PANEL_BG, THEME_BORDER);

    // Clipped, like everything else in a fixed box -- the status string
    // is short but the rule is the rule (docs/gui-guidelines.md).
    gfx_draw_string_clipped(cx + PAD, cy + row_status(), cw - 2 * PAD,
                             g_state.status, THEME_TEXT, THEME_WINDOW_BG);
}

int uidemo_hover(struct window *win, int cx, int cy) {
    (void)win;
    layout();
    int now = (cx < 0 || cy < 0) ? W_NONE : (int)widget_at(cx, cy);
    int group_changed = ui_button_group_hover(&g_state.group, cx, cy);
    if (now == g_state.hover_name) return group_changed;
    g_state.hover_name = now;
    klog_write("uidemo: hover ");
    klog_write(WIDGET_NAMES[now]);
    klog_write("\n");
    return 1;
}

int uidemo_press(struct window *win, int cx, int cy) {
    (void)win;
    layout();
    int changed = ui_button_group_press(&g_state.group, cx, cy);
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
    int code = ui_button_group_release(&g_state.group);
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
    } else if (w == W_TEXTBOX) {
        ui_textbox_set_active(&g_state.textbox, 1);
        logline("focus textbox");
        set_status("textbox focused");
    } else {
        ui_textbox_set_active(&g_state.textbox, 0);
    }
    window_invalidate(win);
}

void uidemo_key(struct window *win, int key) {
    char msg[96];
    if (g_state.textbox.field.active) {
        ui_textbox_key(&g_state.textbox, key);
        k_snprintf(msg, sizeof msg, "key %u text=\"%s\"",
                   (unsigned)key, g_state.textbox.field.buf);
        logline(msg);
        set_status(msg);
    } else {
        k_snprintf(msg, sizeof msg, "key %u (no focus)", (unsigned)key);
        logline(msg);
        set_status(msg);
    }
    window_invalidate(win);
}
