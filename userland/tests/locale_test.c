// The regional formats: setlocale() over /etc/locales and the locale.*
// settings, the locale's strftime conversions, localeconv(), and the two
// helpers every program formats through (lib/udate.h, lib/unum.h).
//
// THE EXPECTED STRINGS ARE WRITTEN OUT, not derived from the tables
// under test -- a test that built "1.10.2026" from D_FMT would agree
// with a wrong D_FMT.
//
// IT RESTORES THE SETTINGS IT CHANGES, including on a failed check: a
// region left set changes every later tool's output.
#include <locale.h>
#include <langinfo.h>
#include <string.h>
#include <time.h>
#include "lib/utest.h"
#include "lib/usetting.h"
#include "lib/udate.h"
#include "lib/unum.h"

static struct tm moment(void) {
    // Thursday 2026-10-01 09:05:07 -- single-digit day, month and hour,
    // so a missing `-` flag shows as "01.10." or "09.05".
    struct tm t = { .tm_year = 126, .tm_mon = 9, .tm_mday = 1, .tm_hour = 9,
                    .tm_min = 5, .tm_sec = 7 };
    timegm(&t);
    return t;
}

static void str_is(const char *got, const char *want, const char *what) {
    char detail[160];
    snprintf(detail, sizeof detail, "got \"%s\", wanted \"%s\"", got, want);
    utest_check_detail(strcmp(got, want) == 0, what, detail);
}

static void fmt_is(const char *fmt, const char *want, const char *what) {
    struct tm t = moment();
    char buf[96];
    strftime(buf, sizeof buf, fmt, &t);
    str_is(buf, want, what);
}

static void num_is(const char *in, int flags, const char *want, const char *what) {
    char buf[48];
    strlcpy(buf, in, sizeof buf);
    unum_localize(buf, sizeof buf, flags);
    str_is(buf, want, what);
}

int main(void) {
    utest_begin("locale_test", "regions, their formats, and the setting", 0);

    // --- C is C ---------------------------------------------------------
    utest_check(setlocale(LC_ALL, "C") != 0, "C is a locale");
    fmt_is("%x %X", "10/01/26 09:05:07", "C's %x and %X are the standard's");
    num_is("1234.5", UNUM_GROUP, "1234.5", "C changes no number");
    struct tm t = moment();
    char buf[96];
    udate_format_tm(buf, sizeof buf, &t, UDATE_DATE | UDATE_TIME);
    str_is(buf, "2026-10-01 09:05", "udate in C is the ISO every caller printed before");

    // --- refused, never guessed ----------------------------------------
    utest_check(setlocale(LC_ALL, "de") == 0, "a region /etc/locales lacks is refused");
    utest_check(setlocale(LC_ALL, "fi@time=25hour") == 0, "...as is an unknown override word");
    utest_check(setlocale(LC_ALL, "fi@colour=red") == 0, "...and an unknown override key");
    utest_check(strcmp(setlocale(LC_ALL, 0), "C") == 0, "...and a refusal changed nothing");

    // --- Finland ---------------------------------------------------------
    if (setlocale(LC_ALL, "fi")) {
        fmt_is("%x", "1.10.2026", "fi's short date drops the zeroes");
        fmt_is("%X", "9.05.07", "fi's time is dotted and the hour unpadded");
        fmt_is("%c", "1.10.2026 9.05.07", "%c is the date then the time");
        str_is(localeconv()->decimal_point, ",", "fi's decimal mark is a comma");
        num_is("1234567.89", UNUM_GROUP, "1 234 567,89", "...grouped by spaces when asked");
        num_is("1007.6M", 0, "1007,6M", "...and a size is not grouped");
        num_is("12.5%", 0, "12,5%", "a suffix passes through");
        utest_check(nl_langinfo(_NL_TIME_FIRST_WEEKDAY)[0] == 2, "fi's week starts on Monday");
        utest_check(nl_langinfo(_TOY_WEEK_NUMBERS)[0] == '1', "...and shows week numbers");
        udate_format_tm(buf, sizeof buf, &t, UDATE_DATE | UDATE_LONG);
        str_is(buf, "Thursday 1 October 2026", "fi's long date, in English");
    } else utest_check(0, "fi is a locale");

    // --- the United States ----------------------------------------------
    if (setlocale(LC_ALL, "us")) {
        fmt_is("%x %X", "10/1/2026 9:05:07 AM", "us is month first and 12-hour");
        num_is("1234567.89", UNUM_GROUP, "1,234,567.89", "us groups with commas");
        utest_check(nl_langinfo(_NL_TIME_FIRST_WEEKDAY)[0] == 1, "us's week starts on Sunday");
        udate_format_tm(buf, sizeof buf, &t, UDATE_DATE | UDATE_LONG);
        str_is(buf, "Thursday, October 1, 2026", "us's long date");
    } else utest_check(0, "us is a locale");

    // --- @modifiers override one field and keep the rest ---------------
    if (setlocale(LC_ALL, "fi@time=24colon,week=sunday,weeknum=off")) {
        fmt_is("%x %X", "1.10.2026 09:05:07", "fi@time=24colon keeps fi's date");
        utest_check(nl_langinfo(_NL_TIME_FIRST_WEEKDAY)[0] == 1, "...and takes week=sunday");
        utest_check(nl_langinfo(_TOY_WEEK_NUMBERS)[0] == '0', "...and weeknum=off");
        str_is(localeconv()->decimal_point, ",", "...and keeps fi's numbers");
    } else utest_check(0, "a locale with @modifiers is accepted");

    // --- LC_TIME and LC_NUMERIC are separate ---------------------------
    setlocale(LC_ALL, "C");
    setlocale(LC_NUMERIC, "fi");
    fmt_is("%x", "10/01/26", "LC_NUMERIC=fi leaves the dates C");
    str_is(localeconv()->decimal_point, ",", "...and changes the numbers");

    // --- "" follows the system setting ---------------------------------
    char region[32] = "", tf[32] = "";
    usetting_get("locale.region", region, sizeof region);
    usetting_get("locale.time_format", tf, sizeof tf);
    if (usetting_set("locale.region", "fi") && usetting_set("locale.time_format", "12")) {
        utest_check(setlocale(LC_ALL, "") != 0, "\"\" takes the system's locale");
        fmt_is("%x %X", "1.10.2026 9:05:07 AM",
               "...the region's date with the time_format override");
    } else utest_check(0, "the locale settings can be set");
    utest_check(usetting_set("locale.region", "xx") == 0,
                "the registry refuses a region it does not list");
    usetting_set("locale.region", region[0] ? region : "iso");
    usetting_set("locale.time_format", tf[0] ? tf : "region");
    return utest_end();
}
