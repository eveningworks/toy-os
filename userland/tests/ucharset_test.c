// lib/ucharset.h: Latin-1 <-> UTF-8, as RFB's cut text needs it.
//
// WHAT A BROKEN VERSION WOULD STILL PASS: ASCII alone passes almost any
// converter, so every byte 0x01-0xFF goes there and back. A decoder that
// replaced per BYTE would pass "one euro sign in, one ? out" only by
// luck of its width, so a four-byte emoji must give ONE '?'. And a
// writer that cut short instead of refusing would pass every check
// whose buffer was big enough; the exact fit and one byte less are both
// asked.
#include <string.h>
#include "lib/ucharset.h"
#include "lib/utest.h"

int main(void) {
    utest_begin("ucharset_test", "Latin-1 <-> UTF-8", 0);
    char all[255], u8[600], back[300];
    for (int i = 0; i < 255; i++) all[i] = (char)(i + 1);
    long n = ucharset_latin1_to_utf8(u8, sizeof u8, all, sizeof all);
    long m = n > 0 ? ucharset_utf8_to_latin1(back, sizeof back, u8, (size_t)n, '?') : -1;
    utest_checkf(n == 127 + 128 * 2 && m == 255 && !memcmp(back, all, 255),
                 "every byte 0x01-0xFF there and back (%ld UTF-8 bytes, %ld back)", n, m);

    char out[32];
    utest_check(ucharset_latin1_to_utf8(out, sizeof out, "\xe4", 1) == 2 && !strcmp(out, "\xc3\xa4"),
                "a-umlaut is C3 A4");
    utest_check(ucharset_utf8_to_latin1(out, sizeof out, "1\xe2\x82\xac", 4, '?') == 2 &&
                !strcmp(out, "1?"), "the euro sign, past Latin-1, is the replacement");
    utest_check(ucharset_utf8_to_latin1(out, sizeof out, "a\xf0\x9f\x98\x80z", 6, '?') == 3 &&
                !strcmp(out, "a?z"), "a four-byte character is ONE replacement");
    utest_check(ucharset_utf8_to_latin1(out, sizeof out, "\x80x\xc3", 3, '?') == 3 &&
                !strcmp(out, "?x?"), "a stray continuation and a truncated sequence are replaced");
    utest_check(ucharset_utf8_to_latin1(out, sizeof out, "\xc0\x80y", 3, '?') == 3 &&
                !strcmp(out, "??y"), "an overlong NUL is not a NUL");
    utest_check(ucharset_latin1_to_utf8(out, sizeof out, "a\0b", 3) == 2 && !strcmp(out, "ab"),
                "a NUL is dropped");

    utest_check(ucharset_latin1_to_utf8(out, 3, "\xe4", 1) == 2 &&
                ucharset_latin1_to_utf8(out, 2, "\xe4", 1) == -1 && out[0] == 0,
                "an exact fit is written; one byte short is refused, not cut");
    utest_check(ucharset_utf8_to_latin1(out, 3, "ab", 2, '?') == 2 &&
                ucharset_utf8_to_latin1(out, 2, "ab", 2, '?') == -1 && out[0] == 0 &&
                ucharset_utf8_to_latin1(out, 0, "ab", 2, '?') == -1,
                "...and the same the other way, a cap of 0 included");
    return utest_end();
}
