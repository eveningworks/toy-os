// System Settings: what Time & Locale shows beyond its settings -- the
// live clock and the Change... dialog on Date & time, and the preview on
// Region & formats.
//
// SETTING THE CLOCK IS AN ACTION, NOT A SETTING: there is no value to
// stage, and a time typed into a staged page goes stale while it waits
// for Apply. So it is a button and a dialog (Windows 11's Date & time),
// it writes at once with SYS_SETTIME, and it is unavailable while
// network time is on, which would undo it (`timedatectl set-time`
// refuses for the same reason). Both extras are found by the SETTING a
// page carries, like the screensaver's Test, so renaming a page in
// /etc/settings.d keeps them.
#include "settings/settings_internal.h"
#include <time.h>
#include <locale.h>
#include <langinfo.h>
#include "caltime.h"
#include "lib/udate.h"
#include "lib/human.h"
#include "lib/unum.h"

#define SET_TZ      "system.timezone"
#define SET_NTP     "system.ntp"
#define SET_REGION  "locale.region"

int g_clock_page, g_region_page;
struct uui_custom g_clock_view;
struct uui_setting_row g_clock_row;
struct uui_button g_clock_btn;
struct uui_label g_region_preview;
static char g_preview_text[160];
static char g_clock_desc[SETTING_ABI_DESC_MAX];
static int g_ntp_on;
static uint64_t g_shown_second;

static int slot_named(const char *name) {
    for (int i = 0; i < g_slot_count; i++)
        if (g_slot[i].setting >= 0 && strcmp(g_name[g_slot[i].setting], name) == 0) return i;
    return -1;
}

static int pad(void) { return ugfx_char_w(); }

// --- the live clock -----------------------------------------------------

// "UTC+3", "UTC-3:30", "UTC" -- the offset in force now, DST included.
static void offset_text(char *out, size_t cap, time_t now) {
    long off = tz_offset_seconds(&now) / 60;
    if (!off) { strlcpy(out, "UTC", cap); return; }
    char sign = off < 0 ? '-' : '+';
    if (off < 0) off = -off;
    if (off % 60) snprintf(out, cap, "UTC%c%ld:%02ld", sign, off / 60, off % 60);
    else snprintf(out, cap, "UTC%c%ld", sign, off / 60);
}

int clock_view_height(void) {
    return pad() + ugfx_font_display()->line_h + ugfx_char_h() * 2 + pad() + 4;
}

static void draw_clock(struct ugfx_surface *s, const struct uui_custom *c) {
    uui_fill_round_rect(s, c->x, c->y, c->w, c->h, 6, UTHEME_SEPARATOR);
    uui_fill_round_rect(s, c->x + 1, c->y + 1, c->w - 2, c->h - 2, 5, UTHEME_WHITE);
    struct rtc_time now;
    if (sys_gettime(&now) != 0) return;
    time_t epoch = (time_t)cal_rtc_to_epoch(&now);
    struct tm tm;
    localtime_r(&epoch, &tm);

    char clock[32], date[64], zone[80], off[16];
    udate_format_tm(clock, sizeof clock, &tm, UDATE_TIME | UDATE_SECONDS);
    udate_format_tm(date, sizeof date, &tm, UDATE_DATE | UDATE_LONG);
    offset_text(off, sizeof off, epoch);
    // "UTC" names its own offset; "UTC, UTC" says it twice.
    if (!strcmp(tzname[0], off)) strlcpy(zone, off, sizeof zone);
    else snprintf(zone, sizeof zone, "%s, %s%s", tzname[0], off,
                  tm.tm_isdst ? " (summer time)" : "");

    int x = c->x + pad() + pad() / 2, w = c->w - 3 * pad(), y = c->y + pad();
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_display());
    int big_h = ugfx_char_h();
    ugfx_draw_string_clipped(s, x, y, w, clock, UTHEME_TEXT, UTHEME_WHITE);
    ugfx_set_font(was);
    y += big_h + 2;
    ugfx_draw_string_clipped(s, x, y, w, date, UTHEME_TEXT, UTHEME_WHITE);
    y += ugfx_char_h();
    ugfx_draw_string_clipped(s, x, y, w, zone,
                             uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED), UTHEME_WHITE);
}

// A repaint when the shown second has moved -- on_tick's question.
int clock_tick(void) {
    if (!g_clock_page) return 0;
    struct rtc_time now;
    if (sys_gettime(&now) != 0) return 0;
    uint64_t sec = cal_rtc_to_epoch(&now);
    if (sec == g_shown_second) return 0;
    g_shown_second = sec;
    return 1;
}

// --- the region preview ---------------------------------------------------

// The formats the page's STAGED choices would give, through a locale
// name with @modifiers (<locale.h>) -- then the system's again.
static void build_preview(void) {
    static const struct { const char *setting, *key; } MODS[] = {
        { "locale.date_format", "date" }, { "locale.time_format", "time" },
        { "locale.number_format", "number" }, { "locale.week_start", "week" },
        { "locale.week_numbers", "weeknum" },
    };
    int r = slot_named(SET_REGION);
    if (r < 0) return;
    char name[96];
    snprintf(name, sizeof name, "%s@", staged_value(&g_slot[r]));
    int first = 1;
    for (unsigned i = 0; i < sizeof MODS / sizeof MODS[0]; i++) {
        int sl = slot_named(MODS[i].setting);
        const char *v = sl >= 0 ? staged_value(&g_slot[sl]) : "region";
        if (!v[0] || !strcmp(v, "region")) continue;
        size_t n = strlen(name);
        snprintf(name + n, sizeof name - n, "%s%s=%s", first ? "" : ",", MODS[i].key, v);
        first = 0;
    }
    if (first) name[strlen(name) - 1] = '\0';   // no modifiers: no '@'

    if (!setlocale(LC_ALL, name)) {
        strlcpy(g_preview_text, "These formats are not in /etc/locales", sizeof g_preview_text);
        setlocale(LC_ALL, "");
        return;
    }
    time_t now = time(0);
    struct tm tm;
    localtime_r(&now, &tm);
    char date[32], longd[48], hm[24], num[32], size[24];
    udate_format_tm(date, sizeof date, &tm, UDATE_DATE);
    udate_format_tm(longd, sizeof longd, &tm, UDATE_DATE | UDATE_LONG);
    udate_format_tm(hm, sizeof hm, &tm, UDATE_TIME);
    strlcpy(num, "1234567.89", sizeof num);
    unum_localize(num, sizeof num, UNUM_GROUP);
    human_size_iec(size, sizeof size, 1310720);   // 1.2 MiB
    // Spaced rather than dotted: the session font has no middle dot.
    snprintf(g_preview_text, sizeof g_preview_text, "%s     %s     %s     %s     %s",
             date, longd, hm, num, size);
    setlocale(LC_ALL, "");
    ulogf("settings: preview %s -> %s\n", name, g_preview_text);
}

void region_preview_refresh(void) {
    if (g_region_page) build_preview();
}

// --- the page hooks -------------------------------------------------------

void clock_page_opened(void) {
    g_clock_page = slot_named(SET_TZ) >= 0;
    g_region_page = slot_named(SET_REGION) >= 0;
    g_shown_second = 0;
    int ntp = slot_named(SET_NTP);
    // The APPLIED value, not the staged one: what would undo a manual
    // set is the ntpd that is running now.
    g_ntp_on = ntp >= 0 && strcmp(g_value[g_slot[ntp].setting], "on") == 0;
    g_clock_btn.disabled = g_ntp_on;
    g_clock_row.disabled = g_ntp_on;
    strlcpy(g_clock_desc, g_ntp_on ? "Unavailable while the time is set automatically"
                                   : "Step the clock to a date and time you enter",
            sizeof g_clock_desc);
    region_preview_refresh();
}

// The extras that go after slot `i`'s card. Returns the new item count.
int clock_emit_after(struct uui_item *out, int n, int i,
                     struct uui_focusable *focus, int *nfocus) {
    const char *nm = g_slot[i].setting >= 0 ? g_name[g_slot[i].setting] : "";
    if (g_clock_page && !strcmp(nm, SET_NTP)) {
        g_clock_row.title = "Set the date and time manually";
        g_clock_row.desc = g_clock_desc;
        g_clock_row.control = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_clock_btn,
                                                 .id = ID_CLOCK_CHANGE, .name = "clock_change" };
        out[n++] = (struct uui_item){ .ops = &uui_setting_row_ops, .widget = &g_clock_row,
                                      .flags = UUI_FILL_W };
        if (!g_ntp_on) focus[(*nfocus)++] = (struct uui_focusable){ &g_clock_btn, &uui_button_ops };
    }
    if (g_region_page && !strcmp(nm, SET_REGION))
        out[n++] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_region_preview,
                                      .flags = UUI_FILL_W, .name = "region_preview" };
    return n;
}

// The live clock, at the top of Date & time.
int clock_emit_top(struct uui_item *out, int n) {
    if (!g_clock_page) return n;
    g_clock_view.h = clock_view_height();
    g_clock_view.w = 0;
    out[n++] = (struct uui_item){ .ops = &uui_custom_ops, .widget = &g_clock_view,
                                  .flags = UUI_FILL_W, .name = "clock_view" };
    return n;
}

// --- the Change... dialog -------------------------------------------------

enum { CD_SET = 1, CD_CANCEL, CD_DAY, CD_MONTH, CD_YEAR, CD_HOUR, CD_MIN, CD_SEC };

static struct uapp_window *g_cd_win;
static struct uui_spinbox g_cd_day, g_cd_year, g_cd_hour, g_cd_min, g_cd_sec;
static struct uui_dropdown g_cd_month;
static struct uui_label g_cd_date_l, g_cd_time_l, g_cd_sep1, g_cd_sep2, g_cd_note, g_cd_gap;
static struct uui_button g_cd_set, g_cd_cancel;
static struct uui_item CD_DATE[4], CD_TIME[6], CD_BTNS[3], CD[4];
static struct uui_layout CD_DATE_ROW, CD_TIME_ROW, CD_BTN_ROW, CD_LAYOUT;
static struct uui_focusable CD_FOCUS[8];
static struct uui_focus g_cd_focus;
static char g_cd_note_text[96], g_cd_sep[2];

static const char *const MONTHS[12] = {
    "January", "February", "March", "April", "May", "June", "July",
    "August", "September", "October", "November", "December",
};

// The day's bound follows the month and the year, so the dialog cannot
// offer 31 February -- the parser rule, applied to a control.
static void clamp_day(void) {
    int y = uui_spinbox_value(&g_cd_year), m = uui_dropdown_selected(&g_cd_month) + 1;
    int last = cal_days_in_month(y, m);
    g_cd_day.max = last;
    if (uui_spinbox_value(&g_cd_day) > last) uui_spinbox_set_value(&g_cd_day, last);
}

static void cd_close(struct uapp_window *w) {
    uapp_window_close(w);
    if (g_app) uapp_redraw(g_app);
}

static void cd_set(struct uapp_window *w) {
    struct tm tm = {
        .tm_year = uui_spinbox_value(&g_cd_year) - 1900,
        .tm_mon = uui_dropdown_selected(&g_cd_month),
        .tm_mday = uui_spinbox_value(&g_cd_day),
        .tm_hour = uui_spinbox_value(&g_cd_hour),
        .tm_min = uui_spinbox_value(&g_cd_min),
        .tm_sec = uui_spinbox_value(&g_cd_sec),
    };
    time_t utc = mktime(&tm);
    int rc = utc > 0 ? sys_settime((uint64_t)utc, 0) : -1;
    char when[64];
    udate_format_tm(when, sizeof when, &tm, UDATE_DATE | UDATE_TIME | UDATE_SECONDS);
    if (rc == 0) snprintf(g_status, sizeof g_status, "Clock set to %s", when);
    else snprintf(g_status, sizeof g_status, "The clock refused %s", when);
    ulogf("settings: clock set %lld result %d\n", (long long)utc, rc);
    cd_close(w);
}

static void cd_on_action(struct uapp_window *w, int code) {
    if (code == CD_SET) cd_set(w);
    if (code == CD_CANCEL) cd_close(w);
}

static void cd_on_widget(struct uapp_window *w, int id, int reason) {
    // A release or a key, as on the page: a dialog opened under the
    // pointer must not take the hover for a click (set_owner.c).
    if (reason != UUI_REASON_RELEASE && reason != UUI_REASON_KEY) return;
    if (id == CD_MONTH || id == CD_YEAR) clamp_day();
    uapp_window_redraw(w);
}

static void cd_spin(struct uui_spinbox *s, int v, int lo, int hi) {
    uui_spinbox_init(s, v, lo, hi, 1, 0);
}

void clock_open_dialog(struct uapp *a) {
    if (g_ntp_on || (g_cd_win && uapp_window_is_open(g_cd_win))) return;
    struct rtc_time now;
    sys_gettime(&now);
    time_t e = (time_t)cal_rtc_to_epoch(&now);
    struct tm tm;
    localtime_r(&e, &tm);

    cd_spin(&g_cd_day, tm.tm_mday, 1, 31);
    uui_dropdown_init(&g_cd_month, 0, 0, 0, 0, MONTHS, 12);
    uui_dropdown_set_selected(&g_cd_month, tm.tm_mon);
    // The RTC keeps two digits of year and the century as 20xx
    // (kernel/core/timer.c), so this century is what it can hold.
    cd_spin(&g_cd_year, tm.tm_year + 1900, 2000, 2099);
    cd_spin(&g_cd_hour, tm.tm_hour, 0, 23);
    cd_spin(&g_cd_min, tm.tm_min, 0, 59);
    cd_spin(&g_cd_sec, tm.tm_sec, 0, 59);
    clamp_day();

    // The separator the locale writes a time with, so the fields read
    // as the clock does.
    g_cd_sep[0] = strchr(nl_langinfo(T_FMT), '.') ? '.' : ':';
    g_cd_sep[1] = '\0';
    uui_label_init(&g_cd_date_l, "Date");
    uui_label_init(&g_cd_time_l, "Time");
    uui_label_init(&g_cd_sep1, g_cd_sep);
    uui_label_init(&g_cd_sep2, g_cd_sep);
    snprintf(g_cd_note_text, sizeof g_cd_note_text,
             "%s time. The hardware clock is kept in UTC", tzname[0]);
    uui_label_init(&g_cd_note, g_cd_note_text);
    uui_label_init(&g_cd_gap, "");
    uui_button_init(&g_cd_set, 0, 0, 0, 0, "Set", UTHEME_ACCENT, UTHEME_ACCENT_TEXT, CD_SET);
    uui_button_init(&g_cd_cancel, 0, 0, 0, 0, "Cancel", UTHEME_BUTTON_BG, UTHEME_TEXT, CD_CANCEL);
    // The two captions share a width, so the fields after them align.
    int cap_w = ugfx_text_width("Date") > ugfx_text_width("Time")
                ? ugfx_text_width("Date") : ugfx_text_width("Time");

    CD_DATE[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_cd_date_l, .main_size = cap_w + pad() };
    CD_DATE[1] = (struct uui_item){ .ops = &uui_spinbox_ops, .widget = &g_cd_day, .id = CD_DAY, .name = "clock_day" };
    CD_DATE[2] = (struct uui_item){ .ops = &uui_dropdown_ops, .widget = &g_cd_month, .id = CD_MONTH, .name = "clock_month" };
    CD_DATE[3] = (struct uui_item){ .ops = &uui_spinbox_ops, .widget = &g_cd_year, .id = CD_YEAR, .name = "clock_year" };
    CD_TIME[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_cd_time_l, .main_size = cap_w + pad() };
    CD_TIME[1] = (struct uui_item){ .ops = &uui_spinbox_ops, .widget = &g_cd_hour, .id = CD_HOUR, .name = "clock_hour" };
    CD_TIME[2] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_cd_sep1 };
    CD_TIME[3] = (struct uui_item){ .ops = &uui_spinbox_ops, .widget = &g_cd_min, .id = CD_MIN, .name = "clock_minute" };
    CD_TIME[4] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_cd_sep2 };
    CD_TIME[5] = (struct uui_item){ .ops = &uui_spinbox_ops, .widget = &g_cd_sec, .id = CD_SEC, .name = "clock_second" };
    CD_BTNS[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_cd_gap, .flags = UUI_FILL_W };
    CD_BTNS[1] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_cd_cancel, .id = CD_CANCEL, .name = "clock_cancel" };
    CD_BTNS[2] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_cd_set, .id = CD_SET, .name = "clock_set" };
    CD_DATE_ROW = (struct uui_layout){ .dir = UUI_ROW, .items = CD_DATE, .count = 4 };
    CD_TIME_ROW = (struct uui_layout){ .dir = UUI_ROW, .items = CD_TIME, .count = 6 };
    CD_BTN_ROW = (struct uui_layout){ .dir = UUI_ROW, .items = CD_BTNS, .count = 3 };
    CD[0] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &CD_DATE_ROW };
    CD[1] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &CD_TIME_ROW };
    CD[2] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_cd_note, .flags = UUI_FILL_W };
    CD[3] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &CD_BTN_ROW, .flags = UUI_FILL_W };
    CD_LAYOUT = (struct uui_layout){ .dir = UUI_COLUMN, .items = CD, .count = 4 };

    int nf = 0;
    CD_FOCUS[nf++] = (struct uui_focusable){ &g_cd_day, &uui_spinbox_ops };
    CD_FOCUS[nf++] = (struct uui_focusable){ &g_cd_month, &uui_dropdown_ops };
    CD_FOCUS[nf++] = (struct uui_focusable){ &g_cd_year, &uui_spinbox_ops };
    CD_FOCUS[nf++] = (struct uui_focusable){ &g_cd_hour, &uui_spinbox_ops };
    CD_FOCUS[nf++] = (struct uui_focusable){ &g_cd_min, &uui_spinbox_ops };
    CD_FOCUS[nf++] = (struct uui_focusable){ &g_cd_sec, &uui_spinbox_ops };
    CD_FOCUS[nf++] = (struct uui_focusable){ &g_cd_cancel, &uui_button_ops };
    CD_FOCUS[nf++] = (struct uui_focusable){ &g_cd_set, &uui_button_ops };
    uui_focus_init(&g_cd_focus, CD_FOCUS, nf);

    int w = 0, h = 0;
    uui_layout_natural_size(&CD_LAYOUT, &w, &h);
    int note_w = ugfx_text_width(g_cd_note_text) + 2 * pad();
    if (w < note_w) w = note_w;
    g_cd_win = uapp_window_open(a, &(struct uapp_window_desc){
        .title = "Change date and time", .w = w, .h = h, .flags = UAPP_WIN_MODAL,
        .widgets = CD, .widget_count = 4, .layout = &CD_LAYOUT, .focus = &g_cd_focus,
        .on_widget = cd_on_widget, .on_action = cd_on_action, .on_close = cd_close,
        .log_prefix = "settings.clock",
    });
    ulogf("settings: clock dialog %s\n", g_cd_win ? "open" : "FAILED");
}

void clock_init(void) {
    g_clock_view = (struct uui_custom){ .draw = draw_clock };
    uui_button_init(&g_clock_btn, 0, 0, 0, 0, "Change...", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_CLOCK_CHANGE);
    g_clock_row.desc_rows = 1;   // not a slot, so the page's fit never sets it
    uui_label_init(&g_region_preview, g_preview_text);
    uui_label_set_wrap(&g_region_preview, 2);   // a narrow window wraps it
}
