// The ring-3 timezone conversion: the offset, the two DST rules, and
// what happens when the database cannot answer.
//
// These rules were UNTESTED while they lived in ring 0 -- the kernel's
// tz_test.c said so in its first line, because the math ran off the RTC
// and a loaded database that a KTEST in the live kernel could not pin
// down. In ring 3 both are ordinary inputs: the selection is a setting
// this test writes, and the moment is a `time_t` it chooses.
//
// IT RESTORES THE SELECTION, including on a failed check. A test that
// applies a setting changes the machine for every later tool, and this
// one picks cities on the other side of the world.
#include <time.h>
#include <string.h>
#include "lib/utest.h"
#include "lib/usetting.h"

// A UTC moment, built without the thing under test.
static time_t at(int y, int mo, int d, int h, int mi) {
    struct tm t;
    memset(&t, 0, sizeof t);
    t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d;
    t.tm_hour = h; t.tm_min = mi;
    return timegm(&t);
}

static int local_hour(time_t utc) {
    struct tm out;
    localtime_r(&utc, &out);
    return out.tm_hour;
}

static int select_city(const char *name) {
    if (usetting_set("system.timezone", name) == 0) return 0;
    tzset();
    return 1;
}

int main(void) {
    utest_begin("utz_test", "the timezone conversion in ring 3", 0);

    char original[64] = "utc";
    usetting_get("system.timezone", original, sizeof original);

    // --- a fixed offset, no DST rule -----------------------------------
    int picked = select_city("tokyo");
    utest_check(picked, "the timezone can be selected");
    if (picked) {
        // Tokyo is UTC+9 all year, so the same offset in both seasons is
        // the check -- a rule applied to it would show up as a
        // difference between these two.
        utest_checkf(local_hour(at(2026, 1, 15, 3, 0)) == 12,
                     "a fixed +9 zone shifts a winter moment (got %d, wanted 12)",
                     local_hour(at(2026, 1, 15, 3, 0)));
        utest_checkf(local_hour(at(2026, 7, 15, 3, 0)) == 12,
                     "...and a summer one by the SAME amount (got %d, wanted 12)",
                     local_hour(at(2026, 7, 15, 3, 0)));
    }

    // --- the EU rule ----------------------------------------------------
    if (select_city("helsinki")) {
        utest_checkf(local_hour(at(2026, 1, 15, 10, 0)) == 12,
                     "Helsinki is +2 in January (got %d, wanted 12)",
                     local_hour(at(2026, 1, 15, 10, 0)));
        utest_checkf(local_hour(at(2026, 7, 15, 10, 0)) == 13,
                     "...and +3 in July, which is the EU rule (got %d, wanted 13)",
                     local_hour(at(2026, 7, 15, 10, 0)));
        // The transition is the last Sunday of March -- 2026-03-29 --
        // and the day before it is still winter time. A rule that
        // switched on a fixed date would pass the two above and fail
        // this pair.
        utest_checkf(local_hour(at(2026, 3, 28, 10, 0)) == 12,
                     "the day BEFORE the last Sunday of March is still +2 (got %d)",
                     local_hour(at(2026, 3, 28, 10, 0)));
        utest_checkf(local_hour(at(2026, 3, 29, 10, 0)) == 13,
                     "the last Sunday of March itself is +3 (got %d)",
                     local_hour(at(2026, 3, 29, 10, 0)));
        utest_check(localtime(&(time_t){at(2026, 7, 15, 10, 0)})->tm_isdst == 1,
                    "a summer moment reports tm_isdst");
        utest_check(localtime(&(time_t){at(2026, 1, 15, 10, 0)})->tm_isdst == 0,
                    "...and a winter one does not");
    }

    // --- the US rule, which differs from the EU one by weeks ------------
    if (select_city("newyork")) {
        // 2026: US DST starts March 8 (second Sunday), EU on March 29.
        // A moment between them is the check that tells the two rules
        // apart -- one rule applied to both zones would agree here.
        utest_checkf(local_hour(at(2026, 3, 15, 17, 0)) == 13,
                     "New York is already on DST on March 15 (got %d, wanted 13)",
                     local_hour(at(2026, 3, 15, 17, 0)));
        utest_checkf(local_hour(at(2026, 3, 1, 17, 0)) == 12,
                     "...and was not on March 1 (got %d, wanted 12)",
                     local_hour(at(2026, 3, 1, 17, 0)));
        // November 1 is the first Sunday, so DST has ended.
        utest_checkf(local_hour(at(2026, 11, 1, 17, 0)) == 12,
                     "the first Sunday of November is back to standard (got %d)",
                     local_hour(at(2026, 11, 1, 17, 0)));
    }

    // --- what the database cannot answer --------------------------------
    // The registry refuses a city that is not one of the choices, which
    // is the file's own list -- so a typo never reaches libc at all.
    utest_check(usetting_set("system.timezone", "notacity") == 0,
                "the registry refuses a city the database does not list");

    if (select_city("utc")) {
        utest_checkf(local_hour(at(2026, 7, 15, 10, 0)) == 10,
                     "UTC is its own local time (got %d, wanted 10)",
                     local_hour(at(2026, 7, 15, 10, 0)));
        utest_check(strcmp(tz_current_name(), "utc") == 0,
                    "...and the selected name is what was set");
    }

    if (original[0]) usetting_set("system.timezone", original);
    return utest_end();
}
