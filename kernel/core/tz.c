// Timezone selection and application, on top of the raw UTC time
// rtc_read() (timer.c) returns from the CMOS/RTC hardware clock.
//
// This is toy-os's first config file: the selected city is persisted as
// plain text in /etc/timezone via the ordinary persistent filesystem
// (fs.h) -- no new storage mechanism, just fs_write()/fs_read() on a
// path like any other file `ls`/`cat` can see. /etc is the general
// config-file convention now (kernel.c's kernel_main() creates it right
// after fs_init(), before tz_init() runs) -- any future config file
// should live there too, not invent its own location.
//
// tz_init() also migrates a config file from before /etc existed (a
// bare "timezone" at the filesystem root, from when this was the only
// config file and directories didn't exist yet) -- see LEGACY_TZ_FILE
// below -- so an already-chosen city isn't silently lost across the
// upgrade.
//
// DST is real added complexity for what's otherwise a one-line offset
// add, but was asked for specifically (Finland is UTC+2 in winter, +3
// in summer). Two rules are implemented:
//   - TZ_DST_EU: last Sunday of March 01:00 UTC -> last Sunday of
//     October 01:00 UTC (the actual EU-wide rule, which is also what
//     the UK uses post-Brexit).
//   - TZ_DST_US: second Sunday of March -> first Sunday of November.
// Known simplification: both are checked against the transition *date*
// only (not the exact 01:00/02:00 clock-change instant), so the
// displayed time can be off by up to a few hours right at the
// spring/fall boundary itself. Not worth the extra precision for a
// shell clock.

#include "tz.h"
#include "fs.h"
#include "string.h"

enum dst_rule { TZ_DST_NONE, TZ_DST_EU, TZ_DST_US };

struct tz_city {
    const char *name;          // lowercase, matched by `timezone <name>`
    int base_offset_minutes;   // standard-time UTC offset, before DST
    enum dst_rule dst_rule;
};

static const struct tz_city TZ_CITIES[] = {
    { "utc",         0,    TZ_DST_NONE },
    { "helsinki",  120,    TZ_DST_EU   },
    { "london",      0,    TZ_DST_EU   },
    { "berlin",     60,    TZ_DST_EU   },
    { "newyork",  -300,    TZ_DST_US   },
    { "losangeles", -480,  TZ_DST_US   },
    { "tokyo",     540,    TZ_DST_NONE },
};
#define TZ_CITY_COUNT_INTERNAL ((int)(sizeof(TZ_CITIES) / sizeof(TZ_CITIES[0])))

#define TZ_CONFIG_FILE "/etc/timezone"
#define LEGACY_TZ_FILE "/timezone" // pre-/etc location -- see tz_init()

static int current_index = 0; // UTC until tz_init() loads/sets otherwise

// ---- date math ----

static int is_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static int days_in_month(int year, int month) {
    static const int DAYS[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (month == 2 && is_leap_year(year)) return 29;
    return DAYS[month - 1];
}

// Sakamoto's algorithm. Returns 0=Sunday .. 6=Saturday.
static int day_of_week(int year, int month, int day) {
    static const int T[] = { 0,3,2,5,0,3,5,1,4,6,2,4 };
    if (month < 3) year -= 1;
    return (year + year/4 - year/100 + year/400 + T[month - 1] + day) % 7;
}

// Day-of-month of the nth (1-based) Sunday in `month`.
static int nth_sunday(int year, int month, int n) {
    int first_dow = day_of_week(year, month, 1);
    int first_sunday = 1 + ((7 - first_dow) % 7);
    return first_sunday + (n - 1) * 7;
}

// Day-of-month of the last Sunday in `month`.
static int last_sunday(int year, int month) {
    int last_day = days_in_month(year, month);
    int last_dow = day_of_week(year, month, last_day);
    return last_day - last_dow;
}

static int eu_dst_active(int year, int month, int day) {
    if (month > 3 && month < 10) return 1;
    if (month < 3 || month > 10) return 0;
    if (month == 3) return day >= last_sunday(year, 3);
    return day < last_sunday(year, 10); // month == 10
}

static int us_dst_active(int year, int month, int day) {
    if (month > 3 && month < 11) return 1;
    if (month < 3 || month > 11) return 0;
    if (month == 3) return day >= nth_sunday(year, 3, 2);
    return day < nth_sunday(year, 11, 1); // month == 11
}

// Adds (possibly negative) minutes to *t, rolling day/month/year over
// as needed. General enough for any offset, though in practice every
// TZ_CITIES entry is well within a single day's rollover.
static void rtc_add_minutes(struct rtc_time *t, int delta_minutes) {
    int total = (int)t->hour * 60 + (int)t->minute + delta_minutes;
    int day_delta = 0;
    while (total < 0) { total += 24 * 60; day_delta--; }
    while (total >= 24 * 60) { total -= 24 * 60; day_delta++; }
    t->hour = (uint8_t)(total / 60);
    t->minute = (uint8_t)(total % 60);

    int day = (int)t->day + day_delta;
    int month = t->month;
    int year = t->year;
    while (day < 1) {
        month--;
        if (month < 1) { month = 12; year--; }
        day += days_in_month(year, month);
    }
    while (day > days_in_month(year, month)) {
        day -= days_in_month(year, month);
        month++;
        if (month > 12) { month = 1; year++; }
    }
    t->day = (uint8_t)day;
    t->month = (uint8_t)month;
    t->year = (uint16_t)year;
}

// ---- public API ----

void tz_init(void) {
    current_index = 0; // default: UTC
    uint32_t size = 0;
    // fs_write() always NUL-terminates at data[size] (see fs.c), so
    // `data` below is safe to treat as a plain C string in both branches.
    const char *data = fs_read(TZ_CONFIG_FILE, &size);

    if (!data || size == 0) {
        // No /etc/timezone yet -- check for a pre-/etc config file
        // (see this file's top comment) and migrate it rather than
        // silently falling back to UTC if the user had already chosen
        // something.
        data = fs_read(LEGACY_TZ_FILE, &size);
        if (!data || size == 0) return;

        int idx = tz_find_by_name(data);
        if (idx < 0) return;
        current_index = idx;
        fs_write(TZ_CONFIG_FILE, TZ_CITIES[idx].name, 0);
        fs_delete(LEGACY_TZ_FILE);
        return;
    }

    int idx = tz_find_by_name(data);
    if (idx >= 0) current_index = idx;
}

int tz_city_count(void) {
    return TZ_CITY_COUNT_INTERNAL;
}

const char *tz_city_name(int index) {
    if (index < 0 || index >= TZ_CITY_COUNT_INTERNAL) return 0;
    return TZ_CITIES[index].name;
}

int tz_current_index(void) {
    return current_index;
}

int tz_set_index(int index) {
    if (index < 0 || index >= TZ_CITY_COUNT_INTERNAL) return 0;
    current_index = index;
    fs_write(TZ_CONFIG_FILE, TZ_CITIES[index].name, 0);
    return 1;
}

int tz_find_by_name(const char *name) {
    for (int i = 0; i < TZ_CITY_COUNT_INTERNAL; i++) {
        if (k_strcmp(TZ_CITIES[i].name, name) == 0) return i;
    }
    return -1;
}

void rtc_read_local(struct rtc_time *out) {
    rtc_read(out);

    const struct tz_city *city = &TZ_CITIES[current_index];
    int offset = city->base_offset_minutes;

    int dst = 0;
    if (city->dst_rule == TZ_DST_EU) dst = eu_dst_active(out->year, out->month, out->day);
    else if (city->dst_rule == TZ_DST_US) dst = us_dst_active(out->year, out->month, out->day);
    if (dst) offset += 60;

    rtc_add_minutes(out, offset);
}
