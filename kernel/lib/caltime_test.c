// Tests for caltime.c -- the proleptic Gregorian calendar and nothing
// else. The civil <-> epoch vectors were `tz_test.c`'s until the
// timezone database left ring 3 (api/tz.h); the city lookup and the DST
// rules they sat beside are `/tests/utz_test` now.
#include "ktest.h"
#include "caltime.h"
#include "rtctime.h"

KTEST("caltime", "civil <-> epoch round-trips and matches known vectors") {
    // Vectors cross-checked against Python's calendar.timegm -- pure
    // calendar arithmetic, no timezone involved (see caltime.h on what
    // these epochs mean).
    static const struct { uint16_t y; uint8_t mo, d, h, mi, s; uint64_t epoch; } V[] = {
        { 1970, 1, 1, 0, 0, 0, 0ull },
        { 1999, 12, 31, 23, 59, 59, 946684799ull },
        { 2000, 2, 29, 12, 0, 0, 951825600ull },   // century leap day (400 rule)
        { 2024, 2, 29, 23, 59, 59, 1709251199ull },
        { 2026, 8, 14, 12, 34, 56, 1786710896ull },
        { 2038, 1, 19, 3, 14, 8, 2147483648ull },  // past the 32-bit rollover
    };
    for (unsigned i = 0; i < sizeof(V) / sizeof(V[0]); i++) {
        struct rtc_time t;
        t.year = V[i].y; t.month = V[i].mo; t.day = V[i].d;
        t.hour = V[i].h; t.minute = V[i].mi; t.second = V[i].s;
        KTEST_ASSERT(cal_rtc_to_epoch(&t) == V[i].epoch);

        struct rtc_time back;
        cal_epoch_to_rtc(V[i].epoch, &back);
        KTEST_ASSERT_EQ(back.year, V[i].y);
        KTEST_ASSERT_EQ(back.month, V[i].mo);
        KTEST_ASSERT_EQ(back.day, V[i].d);
        KTEST_ASSERT_EQ(back.hour, V[i].h);
        KTEST_ASSERT_EQ(back.minute, V[i].mi);
        KTEST_ASSERT_EQ(back.second, V[i].s);
    }

    // Ordering sanity: one second before midnight < the next day.
    struct rtc_time a = { 23, 59, 59, 28, 2, 2023 };
    struct rtc_time b = { 0, 0, 0, 1, 3, 2023 };
    KTEST_ASSERT(cal_rtc_to_epoch(&a) + 1 == cal_rtc_to_epoch(&b));
}
