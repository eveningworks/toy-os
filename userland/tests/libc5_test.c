// Stage 5 of the C library: time_t, struct tm, mktime, strftime
// (docs/libc-design.md).
//
// EVERY DATE HERE IS A FIXED, KNOWN ONE -- nothing asserts against the
// current clock, which would make the test pass or fail depending on
// the day it ran. The one check that uses the real time asserts only
// that it is SELF-CONSISTENT (time() -> gmtime() -> mktime() returns
// the same number), which is true on any date.
//
// The load-bearing checks:
//  - **A round trip through a leap day and a century boundary.**
//    2000-02-29 is a leap year by the 400 rule and 1900-02-29 does not
//    exist by the 100 rule. NOTE WHERE THAT RULE ACTUALLY LIVES: not in
//    cal_is_leap(), which nothing on this path calls, but inside
//    Hinnant's era arithmetic in cal_days_from_civil(). A control that
//    broke cal_is_leap() changed nothing here and reddened a KERNEL
//    KTEST instead, because its only callers are tz.c's DST rules --
//    which is the correct division, and worth knowing before hunting
//    for a calendar bug in the wrong function. Dropping the `- yoe/100`
//    term reddens five checks here.
//
//  - **Every expected value was verified against an independent
//    implementation** (the host's Python datetime) rather than against
//    the code being tested. Writing the expectation from the same
//    mental model as the implementation is how a calendar test agrees
//    with a calendar bug.
//  - **mktime NORMALISING out-of-range fields**, which is how C does
//    date arithmetic ("add 40 days, then mktime"). A mktime that
//    rejected them would pass any test that only feeds it valid dates.
//  - **The epoch of a date computed independently.** 2001-09-09
//    01:46:40 UTC is 1000000000, a number checkable by hand.
//
// Prints one line per check and exits with the number of failures.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "rt/sys.h"

#include "lib/utest.h"

static char buf[128];

static void str_is(const char *got, const char *want, const char *what) {
    if (strcmp(got, want) == 0) { utest_check(1, what); return; }
    static char msg[192];
    snprintf(msg, sizeof msg, "%s   -- got \"%s\", wanted \"%s\"", what, got, want);
    utest_check(0, msg);
}

// Builds a tm without relying on mktime, so the two are tested apart.
static struct tm mk(int y, int mo, int d, int h, int mi, int s) {
    struct tm t;
    t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d;
    t.tm_hour = h; t.tm_min = mi; t.tm_sec = s;
    t.tm_wday = 0; t.tm_yday = 0; t.tm_isdst = 0;
    return t;
}

int main(void) {
    utest_begin("libc5_test", "time_t, struct tm, mktime, strftime", 0);

    // --- known epochs -------------------------------------------------
    struct tm t = mk(1970, 1, 1, 0, 0, 0);
    utest_check(mktime(&t) == 0, "1970-01-01 00:00:00 is epoch 0");
    t = mk(2001, 9, 9, 1, 46, 40);
    utest_check(mktime(&t) == 1000000000, "2001-09-09 01:46:40 is exactly 1e9");
    t = mk(2024, 2, 29, 12, 0, 0);
    time_t leap = mktime(&t);
    utest_check(leap == 1709208000, "a leap day converts to its known epoch");

    // --- the leap rules ----------------------------------------------
    // 2000 is a leap year (the 400 rule) and 1900 is not (the 100
    // rule). A calendar that only knows "every four years" gets both of
    // these wrong and everything else in this file right.
    t = mk(2000, 2, 29, 0, 0, 0);
    time_t feb29_2000 = mktime(&t);
    utest_check(t.tm_mon == 1 && t.tm_mday == 29,
          "2000-02-29 exists and is not normalised away");
    t = mk(1900, 2, 29, 0, 0, 0);
    mktime(&t);
    utest_check(t.tm_mon == 2 && t.tm_mday == 1,
          "1900-02-29 does NOT exist and rolls into March 1st");

    // --- round trips --------------------------------------------------
    struct tm back;
    gmtime_r(&feb29_2000, &back);
    utest_check(back.tm_year == 100 && back.tm_mon == 1 && back.tm_mday == 29,
          "epoch -> tm -> the same leap day");
    utest_check(back.tm_wday == 2, "and 2000-02-29 was a Tuesday");
    utest_check(back.tm_yday == 59, "with the right day of the year");

    time_t v = 1709208000;
    gmtime_r(&v, &back);
    utest_check(mktime(&back) == v, "tm -> epoch -> tm -> epoch is stable");

    // --- normalisation ------------------------------------------------
    // How C does date arithmetic. A mktime that REJECTED these would
    // pass every check above.
    t = mk(2024, 1, 31, 0, 0, 0);
    t.tm_mday += 40;                      // "31 January + 40 days"
    mktime(&t);
    utest_check(t.tm_year == 124 && t.tm_mon == 2 && t.tm_mday == 11,
          "mday + 40 carries into March (through a leap February)");
    t = mk(2023, 12, 31, 23, 59, 59);
    t.tm_sec += 1;
    mktime(&t);
    utest_check(t.tm_year == 124 && t.tm_mon == 0 && t.tm_mday == 1 &&
          t.tm_hour == 0 && t.tm_min == 0 && t.tm_sec == 0,
          "one second past new year's eve carries the whole way up");
    t = mk(2024, 1, 1, 0, 0, 0);
    t.tm_mon -= 1;                        // month 0 - 1 = the previous December
    mktime(&t);
    utest_check(t.tm_year == 123 && t.tm_mon == 11,
          "a NEGATIVE month borrows from the year");

    // --- strftime -----------------------------------------------------
    v = 1709208000; // 2024-02-29 12:00:00
    gmtime_r(&v, &back);
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &back);
    str_is(buf, "2024-02-29 12:00:00", "strftime's basic conversions");
    strftime(buf, sizeof buf, "%F %T", &back);
    str_is(buf, "2024-02-29 12:00:00", "%F and %T are the same thing");
    strftime(buf, sizeof buf, "%A %d %B %Y", &back);
    str_is(buf, "Thursday 29 February 2024", "names in full");
    strftime(buf, sizeof buf, "%a %b %e", &back);
    str_is(buf, "Thu Feb 29", "abbreviated, and %e space-pads");
    strftime(buf, sizeof buf, "%I:%M %p", &back);
    str_is(buf, "12:00 PM", "noon is 12 PM, not 00 PM");
    v = 1709164800; // the same day at 00:00
    gmtime_r(&v, &back);
    strftime(buf, sizeof buf, "%I:%M %p", &back);
    str_is(buf, "12:00 AM", "and midnight is 12 AM, not 00 AM");
    strftime(buf, sizeof buf, "%y %j %R %D", &back);
    str_is(buf, "24 060 00:00 02/29/24", "%y %j %R and %D");
    strftime(buf, sizeof buf, "100%% [%q]", &back);
    str_is(buf, "100% [%q]", "%% is a literal and an unknown conversion is copied through");
    utest_check(strftime(buf, 8, "%Y-%m-%d", &back) == 0,
          "a result that does not fit returns 0");

    // --- asctime ------------------------------------------------------
    v = 1709208000;
    str_is(asctime(gmtime(&v)), "Thu Feb 29 12:00:00 2024\n",
           "asctime's fixed 25-character form");
    v = 1709164800 + 5 * 3600; // the 29th at 05:00, so the day is single-digit... no: 29
    gmtime_r(&v, &back);
    struct tm single = mk(2024, 3, 5, 9, 8, 7);
    mktime(&single);
    str_is(asctime(&single), "Tue Mar  5 09:08:07 2024\n",
           "and SPACE-pads a single-digit day, where a numeric width would zero-pad");

    // --- the real clock, asserted only for self-consistency -----------
    time_t now = time(0);
    utest_check(now > 1000000000, "time() is past 2001, so the RTC was actually read");
    time_t stored = 0;
    utest_check(time(&stored) == stored, "time(&t) stores what it returns");
    gmtime_r(&now, &back);
    utest_check(mktime(&back) == now, "time() -> gmtime() -> mktime() round-trips");
    utest_check(difftime(now + 60, now) == 60.0, "difftime");

    return utest_end();
}
