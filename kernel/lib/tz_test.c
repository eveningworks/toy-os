// Tests for tz.c. Only the name lookup so far -- the offset/DST math
// runs off the RTC and the loaded database, neither of which a test in
// the live kernel can pin down, so this covers the part that's pure
// string handling and has a caller typing into it (`timezone <city>`).
#include "ktest.h"
#include "string.h"
#include "tz.h"

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
