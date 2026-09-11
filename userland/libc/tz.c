// THE TIMEZONE, IN RING 3. The city database, the DST rules and the
// UTC -> local conversion, which the kernel carried until they moved
// here (api/tz.h). This is where a C library keeps them: glibc reads
// /usr/share/zoneinfo, and the kernel it runs on knows only UTC.
//
// THE SELECTION COMES FROM THE REGISTRY, THE DATABASE FROM A FILE.
// `system.timezone` is a registered setting, so `SYS_SETTING` answers
// what city is chosen -- including the default when nothing was ever
// set. `/etc/timezones` is the database: one
// "name,offset_minutes,dst,Display Name" row per city, shipped in
// data/etc and staged by `make iso`.
//
// NOT `/etc/toyos.conf` DIRECTLY, though that is where the registry
// persists it. A fresh disk has no such file until something writes a
// setting, and an `fopen` of a missing file makes the kernel log a
// "open() rejected" line -- once per process, which put a kernel log
// line in the middle of every `ls -l` and broke the console-parsing
// tools reading it. Asking the registry costs one syscall and cannot
// miss.
//
// READ ONCE, on the first conversion, and again on tzset() -- C's own
// contract, and the reason tzset() exists at all. A long-running
// program that wants to follow a change made in System Settings calls
// tzset() again; nothing polls a file from inside localtime().
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <caltime.h>
#include "rt/sys.h"
#include "setting_abi.h"

#define DB_FILE  "/etc/timezones"
#define TZ_SETTING "system.timezone"

enum dst_rule { DST_NONE, DST_EU, DST_US };

// The SELECTED city only. The whole database is read to find it and
// then forgotten: nothing in ring 3 lists cities any more -- System
// Settings gets its dropdown from the registry, which enumerates the
// same file (api/setting.h's `choice_file`).
static char g_name[32] = "utc";
static char g_label[40] = "UTC";
static int g_offset_minutes;
static int g_rule = DST_NONE;
static int g_loaded;

// tzname/timezone/daylight, which POSIX programs read directly.
char *tzname[2] = { (char *)"UTC", (char *)"UTC" };
long timezone;      // SECONDS WEST of UTC, which is POSIX's sign, the
int daylight;       // opposite of the database's minutes EAST

// Fills the globals from the database row for `want`. Leaves them at
// UTC when the file is missing or names no such city -- a wrong clock
// by a known amount beats refusing to tell the time.
static void load_city(const char *want) {
    FILE *f = fopen(DB_FILE, "r");
    if (!f) return;
    char line[160];
    while (fgets(line, sizeof line, f)) {
        char *name = line, *off = NULL, *dst = NULL, *label = NULL;
        for (char *p = line; *p; p++) {
            if (*p != ',') continue;
            *p = '\0';
            if (!off) off = p + 1;
            else if (!dst) dst = p + 1;
            else if (!label) label = p + 1;
        }
        if (!off || !dst) continue;
        if (strcasecmp(name, want) != 0) continue;
        size_t n = strlen(label ? label : dst);
        char *end = (label ? label : dst) + n;
        while (end > (label ? label : dst) && (end[-1] == '\n' || end[-1] == '\r')) *--end = '\0';
        strlcpy(g_name, name, sizeof g_name);
        strlcpy(g_label, label && *label ? label : name, sizeof g_label);
        g_offset_minutes = atoi(off);
        g_rule = strcmp(dst, "eu") == 0 ? DST_EU
               : strcmp(dst, "us") == 0 ? DST_US : DST_NONE;
        break;
    }
    fclose(f);
}

// The nth Sunday of a month, and the last one -- the two shapes both
// implemented rules are written in.
static int nth_sunday(int year, int month, int n) {
    int first_dow = cal_day_of_week(year, month, 1);
    int first_sunday = 1 + ((7 - first_dow) % 7);
    return first_sunday + (n - 1) * 7;
}

static int last_sunday(int year, int month) {
    int last_day = cal_days_in_month(year, month);
    return last_day - cal_day_of_week(year, month, last_day);
}

// KNOWN SIMPLIFICATION, carried over from the kernel: both rules are
// checked against the transition DATE, not the 01:00/02:00 instant, so
// the answer can be an hour out on the changeover day itself.
static int eu_dst_active(int year, int month, int day) {
    if (month > 3 && month < 10) return 1;
    if (month < 3 || month > 10) return 0;
    if (month == 3) return day >= last_sunday(year, 3);
    return day < last_sunday(year, 10);
}

static int us_dst_active(int year, int month, int day) {
    if (month > 3 && month < 11) return 1;
    if (month < 3 || month > 11) return 0;
    if (month == 3) return day >= nth_sunday(year, 3, 2);
    return day < nth_sunday(year, 11, 1);
}

void tzset(void) {
    char want[32] = "utc";
    struct setting_msg m;
    memset(&m, 0, sizeof m);
    m.op = SETTING_OP_GET;
    strlcpy(m.name, TZ_SETTING, sizeof m.name);
    if (sys_setting(&m) == 0 && m.value[0]) strlcpy(want, m.value, sizeof want);
    g_offset_minutes = 0;
    g_rule = DST_NONE;
    strlcpy(g_name, want, sizeof g_name);
    strlcpy(g_label, want, sizeof g_label);
    load_city(want);
    tzname[0] = tzname[1] = g_label;
    timezone = -(long)g_offset_minutes * 60;   // POSIX counts west
    daylight = g_rule != DST_NONE;
    g_loaded = 1;
}

// Minutes EAST of UTC for a moment, DST included. The rules are keyed
// on the UTC date, which is what the kernel's clock hands out.
static int offset_at(const time_t *t) {
    if (!g_loaded) tzset();
    int off = g_offset_minutes;
    if (g_rule == DST_NONE) return off;
    struct tm utc;
    if (!gmtime_r(t, &utc)) return off;
    int y = utc.tm_year + 1900, m = utc.tm_mon + 1, d = utc.tm_mday;
    int on = g_rule == DST_EU ? eu_dst_active(y, m, d) : us_dst_active(y, m, d);
    return off + (on ? 60 : 0);
}

struct tm *localtime_r(const time_t *t, struct tm *out) {
    if (!t || !out) return 0;
    int off = offset_at(t);
    time_t shifted = *t + (time_t)off * 60;
    if (!gmtime_r(&shifted, out)) return 0;
    // tm_isdst says whether the offset above INCLUDED an hour, which is
    // the one thing a caller cannot recover from the fields alone.
    out->tm_isdst = (off != g_offset_minutes);
    return out;
}

static struct tm g_local_tm;
struct tm *localtime(const time_t *t) { return localtime_r(t, &g_local_tm); }

const char *tz_current_name(void) {
    if (!g_loaded) tzset();
    return g_name;
}

// THE SAME CONVERSION FOR A BROKEN-DOWN TIME, which is the shape
// SYS_GETTIME hands back -- so a caller reading the clock does not have
// to round-trip through an epoch to display it. In place.
void tz_localize(struct rtc_time *t) {
    if (!t) return;
    time_t e = (time_t)cal_rtc_to_epoch(t);
    int off = offset_at(&e);
    e += (time_t)off * 60;
    cal_epoch_to_rtc((uint64_t)e, t);
}

// Seconds EAST of UTC at `t`, DST included -- what mktime() subtracts
// and what a caller wanting the raw offset for a moment asks for.
// `timezone` is the standard-time offset and counts the other way.
long tz_offset_seconds(const time_t *t) {
    if (!t) return 0;
    return (long)offset_at(t) * 60;
}
