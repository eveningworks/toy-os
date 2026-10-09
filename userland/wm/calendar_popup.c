// See calendar_popup.h for what this is and why the panel owns it.
#include "wm_internal.h"
#include <time.h>
#include <langinfo.h>
#include "lib/udate.h"
#include "ui/uui_primitives.h"
#include "calendar_popup.h"
#include "wm_shadow.h"
#include "wm_tray.h"
#include "wm_overlay.h"
#include "ui/uui.h"
#include "ui/utheme.h"
#include "kapi.h"
#include "rt/sys.h"
#include "caltime.h"      // cal_days_in_month/_days_from_civil -- shared with ring 0

int calendar_open = 0;

// THE TIME THE CARD SHOWS, read at the tick and drawn from here. Read
// while drawing, the RTC's second rolls over between the compositor's
// uptime ticks, so a frame damaged for something else drew a newer digit
// than the last one damaged -- a stale or half-new second.
static struct rtc_time g_card_now;
static int g_card_now_ok;
// The "today" cell reads the same instant, or it could cross midnight a
// frame apart from the card's date.

// What is on screen. Reset to today every time the popup OPENS (see
// calendar_open_now()), so a month paged to last week is not still
// showing tomorrow -- the same call every desktop's clock applet makes.
static int view_year = 1970, view_month = 1;

// The LC_TIME locale's first weekday (`locale.week_start`, or the
// region's), adopted by calendar_poll_config().
static int week_start_monday = 1;

static const char *const g_month_names[] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December",
};

// Two letters, Monday-first as stored -- the popup rotates them when
// the locale's week starts on a Sunday. Two rather than three keeps the
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
    w += ugfx_char_w();
    // Never narrower than tall: today's fill is a rounded cell, and a
    // sliver of one reads as a bar.
    int min = ugfx_char_h() + 8;
    return w < min ? min : w;
}

// The clock card: the time at display size, the date written out, the
// zone. Its height is the font's, so a bigger font grows it.
static int card_h_px(void) {
    return cal_pad() + ugfx_font_display()->line_h + 2 * ugfx_char_h() + cal_pad();
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
    g->cell_h = ugfx_char_h() + 8;
    g->header_h = ugfx_char_h() + 12;
    g->week_numbers = nl_langinfo(_TOY_WEEK_NUMBERS)[0] == '1';
    int week_w = g->week_numbers ? ugfx_text_width("53") + ugfx_char_w() : 0;

    // As wide as the grid, or as the longest date the card can show.
    int grid_w = week_w + 7 * g->cell_w;
    int text_w = ugfx_text_width("Wednesday 30 September 2026") + 4 * pad;
    g->w = (grid_w > text_w ? grid_w : text_w) + 2 * pad;

    g->card_h = card_h_px();
    g->link_h = ugfx_char_h() + 12;
    int nav_top = pad + g->card_h + pad / 2;
    int grid_top = nav_top + g->header_h;
    g->h = grid_top + (1 + CAL_WEEK_ROWS) * g->cell_h + pad / 2 + g->link_h;

    // ANCHORED TO THE CLOCK, not to the screen's right edge: the clock
    // is the control that was clicked, and a popup that opens somewhere
    // else has to be explained. wm_popup_place() keeps it on a narrow
    // screen (or with a tray full of app items pushing the clock left).
    int cx, cy, cw, ch;
    int right = screen_w - 8;
    if (tray_clock_rect(&cx, &cy, &cw, &ch)) right = cx + cw;
    wm_popup_place(right - g->w, screen_h - taskbar_h - g->h - 4, g->w, g->h, &g->x, &g->y);

    g->card_x = g->x + pad;
    g->card_y = g->y + pad;
    g->card_w = g->w - 2 * pad;

    // The nav row: `<` and `>` squares at the ends, the title between
    // them, so the click target for "back to today" is the whole middle
    // rather than the glyphs alone.
    int ny = g->y + nav_top;
    int btn = g->header_h - 4;
    g->prev_x = g->x + pad;      g->prev_y = ny + 2; g->prev_w = btn; g->prev_h = btn;
    g->next_x = g->x + g->w - pad - btn; g->next_y = ny + 2; g->next_w = btn; g->next_h = btn;
    g->title_x = g->prev_x + btn; g->title_y = ny;
    g->title_w = g->next_x - g->title_x; g->title_h = g->header_h;

    // The grid centred in the panel, the week column (when shown) to the
    // left of the day columns -- grid_x is always the FIRST DAY column.
    int gx = g->x + (g->w - grid_w) / 2;
    g->week_x = g->week_numbers ? gx : -1;
    g->week_w = week_w;
    g->grid_x = gx + week_w;
    g->grid_y = g->y + grid_top;

    g->link_x = g->x + 1;
    g->link_y = g->y + g->h - g->link_h;
    g->link_w = g->w - 2;

    g->view_year = vy;
    g->view_month = vm;
    g->days = cal_days_in_month(vy, vm);
    g->first_col = column_of(vy, vm, 1);
    g->week_start_monday = week_start_monday;

    int ty, tm, td;
    if (calendar_open && g_card_now_ok) {
        struct rtc_time t = g_card_now;
        tz_localize(&t);
        ty = (int)t.year; tm = (int)t.month; td = (int)t.day;
    } else {
        calendar_today(&ty, &tm, &td);
    }
    if (ty == vy && tm == vm) {
        int idx = g->first_col + td - 1;
        g->today_col = idx % 7;
        g->today_row = idx / 7;
    } else {
        g->today_col = -1;
        g->today_row = -1;
    }
}

// The ISO 8601 week of a day: the week holding that week's Thursday,
// counted in the Thursday's year. Days are days since the epoch.
static int iso_week(int64_t days) {
    int mon0 = (int)(((days + 3) % 7 + 7) % 7);
    int64_t thu = days - mon0 + 3;
    int y, m, d;
    cal_civil_from_days(thu, &y, &m, &d);
    return (int)((thu - cal_days_from_civil(y, 1, 1)) / 7) + 1;
}

// ---------------------------------------------------------------------
// State

static void go_today(void) {
    int d;
    calendar_today(&view_year, &view_month, &d);
}

void calendar_clock_tick(void) {
    g_card_now_ok = sys_gettime(&g_card_now) == 0;
    if (calendar_open) calendar_damage();
}

void calendar_open_now(void) {
    go_today();
    wm_overlay_close_others("calendar");   // the popups are mutually exclusive
    calendar_open = 1;
    // Its own rect, not a full frame (wm_render.c repaints only damage).
    calendar_clock_tick();
}

void calendar_close(void) {
    if (!calendar_open) return;
    calendar_open = 0;   // where it was drawn is damaged by the core (wm_overlay.h)
    redraw_pending = 1;
}

static void page_month(int delta) {
    view_month += delta;
    while (view_month < 1)  { view_month += 12; view_year--; }
    while (view_month > 12) { view_month -= 12; view_year++; }
    calendar_damage();   // six rows or five: the panel's height can change
}

void calendar_poll_config(void) {
    // The locale's answer, which already folds the region and its
    // override together (userland/libc/locale.c). A pointer read: the
    // tray clock's wm_locale_sync() is what re-reads the locale.
    int want = nl_langinfo(_NL_TIME_FIRST_WEEKDAY)[0] != 1;
    if (want == week_start_monday) return;
    week_start_monday = want;
    if (calendar_open) calendar_damage();
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
    if (uui_hit(g.link_x, g.link_y, g.link_w, g.link_h, mx, my)) return 4;
    return 0;
}

int calendar_rect(int *x, int *y, int *w, int *h) {
    struct calendar_geom g;
    calendar_geometry(&g);
    *x = g.x; *y = g.y; *w = g.w; *h = g.h;
    return 1;
}

// The rules -- the shadow, and the rect it last occupied -- are
// the core's now (wm_overlay.h).
void calendar_damage(void) { wm_overlay_damage("calendar"); }

// A chevron pointing left (dir < 0) or right, centred in a box -- the
// command bar's NAV ink, as the File Manager's back and forward are.
static void draw_chevron(int x, int y, int w, int h, int dir, uint32_t ink) {
    int r = ugfx_char_h() / 4 + 1;
    int cx = x + w / 2, cy = y + h / 2;
    for (int t = 0; t < 2; t++) {   // two passes, a pixel apart: a 2px stroke
        int tip = cx + dir * r / 2 + t, back = cx - dir * r / 2 + t;
        ugfx_draw_line(wm_surface(), back, cy - r, tip, cy, ink, GEOM_AA);
        ugfx_draw_line(wm_surface(), tip, cy, back, cy + r, ink, GEOM_AA);
    }
}

// "Helsinki, UTC+3 (summer time)" -- the zone and the offset in force.
static void zone_text(char *out, int cap, time_t now, int dst) {
    long off = tz_offset_seconds(&now) / 60;
    char o[16];
    if (!off) k_strlcpy(o, "UTC", sizeof o);
    else if (off % 60) k_snprintf(o, sizeof o, "UTC%c%ld:%02ld", off < 0 ? '-' : '+',
                                  (off < 0 ? -off : off) / 60, (off < 0 ? -off : off) % 60);
    else k_snprintf(o, sizeof o, "UTC%c%ld", off < 0 ? '-' : '+', (off < 0 ? -off : off) / 60);
    if (!k_strcmp(tzname[0], o)) k_strlcpy(out, o, cap);   // not "UTC, UTC"
    else k_snprintf(out, cap, "%s, %s%s", tzname[0], o, dst ? " (summer time)" : "");
}

static void draw_card(const struct calendar_geom *g) {
    struct ugfx_surface *s = wm_surface();
    uui_fill_round_rect(s, g->card_x, g->card_y, g->card_w, g->card_h, 6, UTHEME_SEPARATOR);
    uui_fill_round_rect(s, g->card_x + 1, g->card_y + 1, g->card_w - 2, g->card_h - 2, 5,
                        UTHEME_WHITE);
    if (!g_card_now_ok) return;
    time_t e = (time_t)cal_rtc_to_epoch(&g_card_now);
    struct tm tm;
    localtime_r(&e, &tm);
    char clock[32], date[64], zone[80];
    udate_format_tm(clock, sizeof clock, &tm, UDATE_TIME | UDATE_SECONDS);
    udate_format_tm(date, sizeof date, &tm, UDATE_DATE | UDATE_LONG);
    zone_text(zone, sizeof zone, e, tm.tm_isdst);

    int pad = cal_pad();
    int x = g->card_x + pad, w = g->card_w - 2 * pad, y = g->card_y + pad * 3 / 4;
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_display());
    int big_h = ugfx_char_h();
    ugfx_draw_string_clipped(s, x, y, w, clock, UTHEME_TEXT, UTHEME_WHITE);
    ugfx_set_font(was);
    y += big_h + 2;
    ugfx_draw_string_clipped(s, x, y, w, date, UTHEME_TEXT, UTHEME_WHITE);
    y += ugfx_char_h();
    ugfx_draw_string_clipped(s, x, y, w, zone, uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED),
                             UTHEME_WHITE);
}

void calendar_draw(int mx, int my) {
    if (!calendar_open) return;

    struct calendar_geom g;
    calendar_geometry(&g);
    struct ugfx_surface *s = wm_surface();

    uint32_t bg = UTHEME_PANEL_BG, fg = UTHEME_TEXT;
    uint32_t dim = uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED);
    uint32_t nav = utheme_action(UTHEME_ACT_NAV);
    // Derived from the panel's own colour, never hand-picked: on this
    // near-white theme "hover" has to DARKEN, which is the call
    // uui_state_bg() makes from gfx_luminance() rather than one made
    // here (docs/gui-guidelines.md).
    uint32_t hover_bg = uui_state_bg(bg, UUI_STATE_HOVER);
    int radius = ugfx_char_h() / 2;

    wm_shadow_draw(g.x, g.y, g.w, g.h, radius, WM_SHADOW_POPUP);
    uui_fill_round_rect(s, g.x, g.y, g.w, g.h, radius, UTHEME_OUTLINE);
    uui_fill_round_rect(s, g.x + 1, g.y + 1, g.w - 2, g.h - 2, radius - 1, bg);

    draw_card(&g);

    // --- the nav row: < month year > --------------------------------
    int over_prev = uui_hit(g.prev_x, g.prev_y, g.prev_w, g.prev_h, mx, my);
    int over_next = uui_hit(g.next_x, g.next_y, g.next_w, g.next_h, mx, my);
    int over_title = uui_hit(g.title_x, g.title_y, g.title_w, g.title_h, mx, my);
    if (over_prev) uui_fill_round_rect(s, g.prev_x, g.prev_y, g.prev_w, g.prev_h, 4, hover_bg);
    if (over_next) uui_fill_round_rect(s, g.next_x, g.next_y, g.next_w, g.next_h, 4, hover_bg);
    if (over_title) uui_fill_round_rect(s, g.title_x + 2, g.title_y + 2,
                                        g.title_w - 4, g.title_h - 4, 4, hover_bg);
    draw_chevron(g.prev_x, g.prev_y, g.prev_w, g.prev_h, -1, nav);
    draw_chevron(g.next_x, g.next_y, g.next_w, g.next_h, +1, nav);

    char title[32];
    k_snprintf(title, sizeof title, "%s %d",
               calendar_month_name(g.view_month), g.view_year);
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    draw_centred(g.title_x, g.title_y + (g.header_h - ugfx_char_h()) / 2, g.title_w,
                 title, fg, over_title ? hover_bg : bg);
    ugfx_set_font(was);

    // --- weekday header ---------------------------------------------
    int text_dy = (g.cell_h - ugfx_char_h()) / 2;
    for (int c = 0; c < 7; c++) {
        const char *label = g_day_names[week_start_monday ? c : (c + 6) % 7];
        draw_centred(g.grid_x + c * g.cell_w, g.grid_y + text_dy, g.cell_w, label, dim, bg);
    }

    // --- six rows: the neighbouring months greyed, the week numbers
    // beside them when the locale shows them ---------------------------
    int64_t first = cal_days_from_civil(g.view_year, g.view_month, 1);
    for (int row = 0; row < CAL_WEEK_ROWS; row++) {
        int cy = g.grid_y + (row + 1) * g.cell_h;
        int64_t row_start = first - g.first_col + row * 7;
        if (g.week_numbers) {
            // Numbered by the row's MONDAY, which is its first column on
            // a Monday week and its second on a Sunday one.
            char wk[4];
            k_snprintf(wk, sizeof wk, "%d", iso_week(row_start + (week_start_monday ? 0 : 1)));
            draw_centred(g.week_x, cy + text_dy, g.week_w, wk, dim, bg);
        }
        for (int col = 0; col < 7; col++) {
            int64_t day = row_start + col;
            int y, m, d;
            cal_civil_from_days(day, &y, &m, &d);
            int cx = g.grid_x + col * g.cell_w;
            uint32_t cell_bg = bg, cell_fg = (m == g.view_month) ? fg : dim;
            if (row == g.today_row && col == g.today_col) {
                // TODAY is the accent, not a bespoke colour -- the same
                // accent every selected row in this desktop uses.
                cell_bg = UTHEME_ACCENT;
                cell_fg = UTHEME_ACCENT_TEXT;
                uui_fill_round_rect(s, cx + 2, cy + 2, g.cell_w - 4, g.cell_h - 4, 6, cell_bg);
            }
            char num[3];
            k_snprintf(num, sizeof num, "%d", d);
            if (cell_bg != bg) was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
            draw_centred(cx, cy + text_dy, g.cell_w, num, cell_fg, cell_bg);
            if (cell_bg != bg) ugfx_set_font(was);
        }
    }

    // --- the link to Settings ---------------------------------------
    int over_link = uui_hit(g.link_x, g.link_y, g.link_w, g.link_h, mx, my);
    ugfx_fill_rect(s, g.x + 1, g.link_y - 1, g.w - 2, 1, UTHEME_SEPARATOR);
    uint32_t link_bg = over_link ? uui_state_bg(UTHEME_CHROME, UUI_STATE_HOVER) : UTHEME_CHROME;
    uui_fill_round_rect(s, g.link_x, g.link_y, g.link_w, g.link_h - 1, radius - 1, link_bg);
    ugfx_fill_rect(s, g.link_x, g.link_y, g.link_w, radius, link_bg);   // square top corners
    ugfx_draw_string_clipped(s, g.link_x + cal_pad(), g.link_y + (g.link_h - ugfx_char_h()) / 2,
                             g.link_w - 2 * cal_pad(), "Date & time settings...", nav, link_bg);
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
        calendar_damage();
        return 1;
    }
    // The link opens System Settings on the page that carries the
    // timezone -- named by its SETTING, which a renamed page keeps.
    if (uui_hit(g.link_x, g.link_y, g.link_w, g.link_h, mx, my)) {
        calendar_close();
        { int pid_ = sys_spawn("/bin/wm/system/settings", "system.timezone", -1); if (pid_ > 0) wm_track_launched(pid_); }   // reaped by the poll
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
