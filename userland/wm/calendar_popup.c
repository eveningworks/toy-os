// See calendar_popup.h for what this is and why the panel owns it.
#include "wm_internal.h"
#include <time.h>
#include "calendar_popup.h"
#include "wm_shadow.h"
#include "wm_tray.h"
#include "wm_overlay.h"
#include "ui/uui.h"
#include "ui/utheme.h"
#include "kapi.h"
#include "rt/sys.h"
#include "caltime.h"      // cal_days_in_month/_days_from_civil -- shared with ring 0
#include "wm/wm_conf.h"   // struct setting_msg, SETTING_OP_*

int calendar_open = 0;

// What is on screen. Reset to today every time the popup OPENS (see
// calendar_open_now()), so a month paged to last week is not still
// showing tomorrow -- the same call every desktop's clock applet makes.
static int view_year = 1970, view_month = 1;

// `desktop.week_start`, adopted by calendar_poll_config(). Monday is
// the registry's default (ISO 8601); see week_start_config.c.
static int week_start_monday = 1;

static const char *const g_month_names[] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December",
};

// Two letters, Monday-first as stored -- the popup rotates them when
// `desktop.week_start` says Sunday. Two rather than three keeps the
// panel narrow enough to sit over the taskbar's right end without
// covering half the strip, which is what GNOME and Plasma both draw.
static const char *const g_day_names[] = { "Mo", "Tu", "We", "Th", "Fr", "Sa", "Su" };

const char *calendar_month_name(int month) {
    if (month < 1 || month > 12) return "?";
    return g_month_names[month - 1];
}

void calendar_today(int *out_year, int *out_month, int *out_day) {
    struct rtc_time t;
    sys_gettime(&t);   // UTC
    tz_localize(&t);   // ...localised here (userland/libc/tz.c)
    if (out_year) *out_year = (int)t.year;
    if (out_month) *out_month = (int)t.month;
    if (out_day) *out_day = (int)t.day;
}

// Which COLUMN a date falls in, honouring `week_start`. The weekday
// itself comes from cal_days_from_civil() rather than a Zeller variant
// written out here: 1970-01-01 was a Thursday, so days-since-epoch + 3
// is the Monday-based weekday, and that is one line against a leap-rule
// table this repo already has tested in ring 0 AND ring 3 (caltime.h).
// The double modulo is for a date before 1970, which an RTC should
// never report and which would otherwise index the array backwards.
static int column_of(int year, int month, int day) {
    int64_t days = cal_days_from_civil(year, month, day);
    int mon0 = (int)(((days + 3) % 7 + 7) % 7);
    return week_start_monday ? mon0 : (mon0 + 1) % 7;
}

// ---------------------------------------------------------------------
// Geometry. ONE function, asked by the drawing, the hit-testing and the
// debug console alike -- the rule `gui taskbar` was rewritten for after
// reporting button centres eight pixels off the real ones.
//
// Every measurement is font-derived (docs/gui-guidelines.md): the cell
// is sized from the widest thing that goes in one, never from a pixel
// constant, so a larger default font grows the panel instead of
// clipping it.
//
// SIX WEEK ROWS ALWAYS, even for a month that fits in five (and
// February in a non-leap year starting on the first column fits in
// four). A panel that changed height as you paged months would move its
// own `<` and `>` out from under the cursor, which is why Plasma and
// GNOME both reserve the full six.
#define CAL_WEEK_ROWS 6

static int cal_pad(void)  { return ugfx_char_w(); }

static int cell_w_px(void) {
    int w = ugfx_text_width("30");
    for (int i = 0; i < 7; i++) {
        int dw = ugfx_text_width(g_day_names[i]);
        if (dw > w) w = dw;
    }
    return w + ugfx_char_w();
}

void calendar_geometry(struct calendar_geom *g) {
    // A CLOSED POPUP REPORTS TODAY'S MONTH, not whatever was last paged
    // to -- because that is the month it would show if it opened now
    // (calendar_open_now() resets the view). Read locally rather than
    // written back: a geometry getter that mutates state would be a
    // syscall per frame for an answer nobody is looking at.
    int vy = view_year, vm = view_month;
    if (!calendar_open) calendar_today(&vy, &vm, 0);

    int pad = cal_pad();
    g->cell_w = cell_w_px();
    g->cell_h = ugfx_char_h() + 6;
    g->header_h = ugfx_char_h() + 12;

    g->w = 7 * g->cell_w + 2 * pad;
    g->h = g->header_h + (1 + CAL_WEEK_ROWS) * g->cell_h + pad;

    // ANCHORED TO THE CLOCK, not to the screen's right edge: the clock
    // is the control that was clicked, and a popup that opens somewhere
    // else has to be explained. wm_popup_place() keeps it on a narrow
    // screen (or with a tray full of app items pushing the clock left).
    int cx, cy, cw, ch;
    int right = screen_w - 8;
    if (tray_clock_rect(&cx, &cy, &cw, &ch)) right = cx + cw;
    wm_popup_place(right - g->w, screen_h - taskbar_h - g->h, g->w, g->h, &g->x, &g->y);

    // The header: `<` and `>` are squares at the ends, the title takes
    // everything between them so the click target for "back to today"
    // is the whole middle rather than the glyphs alone.
    int btn = g->header_h - 4;
    g->prev_x = g->x + pad;      g->prev_y = g->y + 2; g->prev_w = btn; g->prev_h = btn;
    g->next_x = g->x + g->w - pad - btn; g->next_y = g->y + 2; g->next_w = btn; g->next_h = btn;
    g->title_x = g->prev_x + btn; g->title_y = g->y;
    g->title_w = g->next_x - g->title_x; g->title_h = g->header_h;

    g->grid_x = g->x + pad;
    g->grid_y = g->y + g->header_h;

    g->view_year = vy;
    g->view_month = vm;
    g->days = cal_days_in_month(vy, vm);
    g->first_col = column_of(vy, vm, 1);
    g->week_start_monday = week_start_monday;

    int ty, tm, td;
    calendar_today(&ty, &tm, &td);
    if (ty == vy && tm == vm) {
        int idx = g->first_col + td - 1;
        g->today_col = idx % 7;
        g->today_row = idx / 7;
    } else {
        g->today_col = -1;
        g->today_row = -1;
    }
}

// ---------------------------------------------------------------------
// State

static void go_today(void) {
    int d;
    calendar_today(&view_year, &view_month, &d);
}

void calendar_open_now(void) {
    go_today();
    wm_overlay_close_others("calendar");   // the popups are mutually exclusive
    calendar_open = 1;
    redraw_pending = 1;
}

void calendar_close(void) {
    if (!calendar_open) return;
    calendar_open = 0;
    redraw_pending = 1;
}

static void page_month(int delta) {
    view_month += delta;
    while (view_month < 1)  { view_month += 12; view_year--; }
    while (view_month > 12) { view_month -= 12; view_year++; }
    redraw_pending = 1;
}

void calendar_poll_config(void) {
    static uint64_t seen_gen;
    static int primed;
    uint64_t gen = sys_fs_generation();
    if (primed && gen == seen_gen) return;
    seen_gen = gen;
    primed = 1;

    // Read through the SETTINGS REGISTRY rather than straight out of
    // /etc/desktop.conf, the note taskbar_poll_config() carries and for
    // the same reason: the registry knows the default and the legal
    // values, so reading the file here would put a second copy of the
    // default in a second place.
    int want = 1;
    struct setting_msg msg;
    for (unsigned i = 0; i < sizeof msg; i++) ((uint8_t *)&msg)[i] = 0;
    msg.op = SETTING_OP_GET;
    k_strlcpy(msg.name, "desktop.week_start", sizeof msg.name);
    if (sys_setting(&msg) == 0 && msg.value[0]) {
        if (k_strcmp(msg.value, "sunday") == 0) want = 0;
    }
    if (want == week_start_monday) return;
    week_start_monday = want;
    if (calendar_open) redraw_pending = 1;
}

// ---------------------------------------------------------------------
// Drawing

static void draw_centred(int x, int y, int w, const char *s, uint32_t fg, uint32_t bg) {
    int tw = ugfx_text_width(s);
    int tx = x + (w - tw) / 2;
    if (tx < x) tx = x;
    // Clipped to the box it was centred in -- an unclipped string in a
    // fixed box is the bug docs/gui-guidelines.md says has shipped
    // twice.
    ugfx_draw_string_clipped(wm_surface(), tx, y, x + w - tx, s, fg, bg);
}

// The registry's two ops (wm_overlay.h). The calendar used to force a
// FULL-SCREEN repaint on every mouse move while it was open -- the
// arrangement the Start menu was rewritten out of, measured at 60 ms a
// move on a 1280x720 TCG guest -- because its `<`/`>` hover was derived
// from the live pointer inside the draw and nothing tracked it. This is
// the three steps docs/roadmap.md asked for.
int calendar_hover_at(int mx, int my) {
    if (!calendar_open) return 0;
    struct calendar_geom g;
    calendar_geometry(&g);
    if (uui_hit(g.prev_x, g.prev_y, g.prev_w, g.prev_h, mx, my)) return 1;
    if (uui_hit(g.next_x, g.next_y, g.next_w, g.next_h, mx, my)) return 2;
    if (uui_hit(g.title_x, g.title_y, g.title_w, g.title_h, mx, my)) return 3;
    return 0;
}

void calendar_damage(void) {
    struct calendar_geom g;
    calendar_geometry(&g);
    wm_damage_window_rect(g.x, g.y, g.w, g.h);   // plus its shadow (wm_shadow.h)
    redraw_pending = 1;
}

void calendar_draw(int mx, int my) {
    if (!calendar_open) return;

    struct calendar_geom g;
    calendar_geometry(&g);

    uint32_t bg = UTHEME_PANEL_BG, border = UTHEME_BORDER, fg = UTHEME_TEXT;
    // Derived from the panel's own colour, never hand-picked: on this
    // near-white theme "hover" has to DARKEN, which is the call
    // uui_state_bg() makes from gfx_luminance() rather than one made
    // here (docs/gui-guidelines.md).
    uint32_t hover_bg = uui_state_bg(bg, UUI_STATE_HOVER);

    wm_shadow_draw(g.x, g.y, g.w, g.h, 0, WM_SHADOW_POPUP);
    ugfx_fill_rect(wm_surface(), g.x, g.y, g.w, g.h, bg);

    // --- header: < month year > -------------------------------------
    int over_prev = uui_hit(g.prev_x, g.prev_y, g.prev_w, g.prev_h, mx, my);
    int over_next = uui_hit(g.next_x, g.next_y, g.next_w, g.next_h, mx, my);
    int over_title = uui_hit(g.title_x, g.title_y, g.title_w, g.title_h, mx, my);
    if (over_prev) ugfx_fill_rect(wm_surface(), g.prev_x, g.prev_y, g.prev_w, g.prev_h, hover_bg);
    if (over_next) ugfx_fill_rect(wm_surface(), g.next_x, g.next_y, g.next_w, g.next_h, hover_bg);
    if (over_title) ugfx_fill_rect(wm_surface(), g.title_x, g.title_y + 2,
                                   g.title_w, g.title_h - 4, hover_bg);

    int glyph_y = g.prev_y + (g.prev_h - ugfx_char_h()) / 2;
    draw_centred(g.prev_x, glyph_y, g.prev_w, "<", fg, over_prev ? hover_bg : bg);
    draw_centred(g.next_x, glyph_y, g.next_w, ">", fg, over_next ? hover_bg : bg);

    char title[32];
    k_snprintf(title, sizeof title, "%s %d",
               calendar_month_name(g.view_month), g.view_year);
    draw_centred(g.title_x, g.y + (g.header_h - ugfx_char_h()) / 2, g.title_w,
                 title, fg, over_title ? hover_bg : bg);

    // --- weekday header, then a rule --------------------------------
    for (int c = 0; c < 7; c++) {
        const char *label = g_day_names[week_start_monday ? c : (c + 6) % 7];
        draw_centred(g.grid_x + c * g.cell_w,
                     g.grid_y + (g.cell_h - ugfx_char_h()) / 2, g.cell_w,
                     label, border, bg);
    }
    ugfx_fill_rect(wm_surface(), g.grid_x, g.grid_y + g.cell_h - 1,
                   7 * g.cell_w, 1, border);

    // --- the days ----------------------------------------------------
    for (int day = 1; day <= g.days; day++) {
        int idx = g.first_col + day - 1;
        int col = idx % 7, row = idx / 7;
        if (row >= CAL_WEEK_ROWS) break; // cannot happen: 6 rows hold any month
        int cx = g.grid_x + col * g.cell_w;
        int cy = g.grid_y + (row + 1) * g.cell_h;

        uint32_t cell_bg = bg, cell_fg = fg;
        if (row == g.today_row && col == g.today_col) {
            // TODAY is the accent, not a bespoke colour -- the same
            // accent every selected row in this desktop uses, so the
            // theme moves it and nothing here has to.
            cell_bg = UTHEME_ACCENT;
            cell_fg = UTHEME_ACCENT_TEXT;
            ugfx_fill_rect(wm_surface(), cx + 1, cy + 1, g.cell_w - 2, g.cell_h - 2, cell_bg);
        }
        char num[3];
        k_snprintf(num, sizeof num, "%d", day);
        draw_centred(cx, cy + (g.cell_h - ugfx_char_h()) / 2, g.cell_w,
                     num, cell_fg, cell_bg);
    }

    // Border last, after every fill -- a hover band spans the columns
    // the border's edges sit on, so drawing it first would have it
    // overpainted (the same lesson start_menu_draw() carries).
    ugfx_draw_rect(wm_surface(), g.x, g.y, g.w, g.h, border);
}

int calendar_handle_click(int mx, int my) {
    if (!calendar_open) return 0;

    struct calendar_geom g;
    calendar_geometry(&g);

    // Paging acts on the button-DOWN edge, which is what `on_click`
    // means here (docs/gui-guidelines.md). Deliberate rather than
    // overlooked: the guideline's arm-on-press/commit-on-release rule
    // protects an action that cannot be taken back, and paging a month
    // is undone by clicking the other arrow -- the same reasoning that
    // lets a scrollbar's arrows repeat while held.
    if (uui_hit(g.prev_x, g.prev_y, g.prev_w, g.prev_h, mx, my)) { page_month(-1); return 1; }
    if (uui_hit(g.next_x, g.next_y, g.next_w, g.next_h, mx, my)) { page_month(+1); return 1; }
    if (uui_hit(g.title_x, g.title_y, g.title_w, g.title_h, mx, my)) {
        go_today();
        redraw_pending = 1;
        return 1;
    }
    // A click anywhere else inside the panel is swallowed, not passed
    // through: the days are not controls (see the header), and letting
    // a click fall to whatever is underneath would raise a window the
    // popup is covering.
    if (uui_hit(g.x, g.y, g.w, g.h, mx, my)) return 1;

    // Outside the panel. What happens to the click itself depends on
    // WHERE, and the three cases are what every real panel does:
    //
    //   - the CLOCK: swallowed, which is what makes a second click on
    //     it close the popup rather than reopen it (wm_input.c asks us
    //     first, so its tray hit-test never runs);
    //   - anywhere else on the TASKBAR: dismissed and PASSED THROUGH,
    //     so the Start button and the window buttons act on the click
    //     that dismissed the popup. Windows and Plasma both behave this
    //     way -- a panel button that needs two clicks because something
    //     else was open reads as a dropped click;
    //   - anywhere else: dismissed and swallowed, the ordinary
    //     click-outside-a-menu grab, so dismissing a popup cannot also
    //     raise a window or launch a desktop icon.
    calendar_close();
    int cx, cy, cw, ch;
    if (tray_clock_rect(&cx, &cy, &cw, &ch) && uui_hit(cx, cy, cw, ch, mx, my))
        return 1;
    return my >= screen_h - taskbar_h ? 0 : 1;
}
