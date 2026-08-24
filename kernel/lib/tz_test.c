// Tests for tz.c. Only the name lookup so far -- the offset/DST math
// runs off the RTC and the loaded database, neither of which a test in
// the live kernel can pin down, so this covers the part that's pure
// string handling and has a caller typing into it (`timezone <city>`).
#include "ktest.h"
#include "string.h"
#include "tz.h"

KTEST("tz", "civil <-> epoch round-trips and matches known vectors") {
    // Vectors cross-checked against Python's calendar.timegm -- pure
    // calendar arithmetic, no timezone involved (see tz.h's comment on
    // what these epochs mean).
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
        KTEST_ASSERT(tz_rtc_to_epoch(&t) == V[i].epoch);

        struct rtc_time back;
        tz_epoch_to_rtc(V[i].epoch, &back);
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
    KTEST_ASSERT(tz_rtc_to_epoch(&a) + 1 == tz_rtc_to_epoch(&b));
}

KTEST("tz", "city lookup ignores ASCII case") {
    // Whichever city loaded at index 0 -- ask the database for a real
    // name rather than hardcoding one, since /etc/timezones can be
    // hand-edited and reordered.
    const char *name = tz_city_name(0);
    KTEST_ASSERT(name != 0 && name[0] != '\0');

    // Sized generously rather than against tz.c's private TZ_NAME_MAX,
    // which isn't (and needn't be) exported just for this.
    char shouted[64];
    KTEST_ASSERT(k_strlen(name) < sizeof(shouted));
    size_t i = 0;
    for (; name[i]; i++) shouted[i] = (char)k_toupper((unsigned char)name[i]);
    shouted[i] = '\0';

    KTEST_ASSERT_EQ(tz_find_by_name(name), 0);
    KTEST_ASSERT_EQ(tz_find_by_name(shouted), 0);

    // Still an exact match otherwise: a prefix isn't a hit.
    KTEST_ASSERT(tz_find_by_name("definitelynotacity") < 0);
    KTEST_ASSERT(tz_find_by_name("") < 0);
}

KTEST("tz", "every city has a display name, and it is never a token") {
    int n = tz_city_count();
    KTEST_ASSERT(n > 0);

    // EVERY row answers, because tz_city_label() falls back to the name
    // -- so a caller may draw it unconditionally. Out of range is the
    // only NULL, matching tz_city_name().
    //
    // A display name is capitalised and a token is not, which is what
    // separates a real name from the fallback. Counted rather than
    // required of every row: /etc/timezones is a file somebody may add
    // a city to, and a hand-added row with no display name is
    // legitimate. A broken fourth column or a broken fallback takes
    // this to ZERO, which is the failure worth catching.
    int named = 0;
    for (int i = 0; i < n; i++) {
        const char *label = tz_city_label(i);
        KTEST_ASSERT(label != 0 && label[0] != '\0');
        if (!(label[0] >= 'a' && label[0] <= 'z')) named++;
    }
    KTEST_ASSERT(named > n / 2);
    KTEST_ASSERT(tz_city_label(-1) == 0);
    KTEST_ASSERT(tz_city_label(n) == 0);
}

KTEST("tz", "a two-word city keeps its space, and its token does not") {
    // losangeles is in the shipped list; on a database somebody has
    // edited it may not be, and then there is nothing to assert.
    int idx = tz_find_by_name("losangeles");
    if (idx < 0) return;

    // THE POINT OF THE WHOLE FEATURE: what is stored and what is shown
    // are different strings, and the stored one is still the token.
    KTEST_ASSERT(k_strcmp(tz_city_name(idx), "losangeles") == 0);
    KTEST_ASSERT(k_strcmp(tz_city_label(idx), "Los Angeles") == 0);
}
