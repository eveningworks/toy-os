// lib/uunicode.h against the names and blocks the image ships.
//
// The answers are Unicode's own, looked up by hand: what a name search,
// a code point and a lone character each find, UTF-8 both ways
// (including the forms a decoder must refuse), and which block a code
// point sits in. The data is generated for the fonts' code points, so
// every character asked about here is one the shipped fonts map.
#include <string.h>
#include "lib/utest.h"
#include "lib/uunicode.h"

// Room for every name: a check that looked at the first few would miss
// a match ranked past them.
static uint32_t g_found[8192];

static int finds(const char *q, uint32_t want, int *count) {
    int n = uunicode_search(q, g_found, 8192);
    *count = n;
    for (int i = 0; i < n && i < 8192; i++)
        if (g_found[i] == want) return 1;
    return 0;
}

int main(void) {
    utest_begin("uunicode_test", "lib/uunicode.h", UTEST_VERDICT_FILE);

    utest_checkf(uunicode_named_count() > 1000, "the names table loaded (%d names)", uunicode_named_count());
    const char *n = uunicode_name(0xE9);
    utest_checkf(n && !strcmp(n, "LATIN SMALL LETTER E WITH ACUTE"), "U+00E9's name (%s)", n ? n : "none");
    n = uunicode_name(0x2192);
    utest_checkf(n && !strcmp(n, "RIGHTWARDS ARROW"), "U+2192's name (%s)", n ? n : "none");
    utest_check(!uunicode_name(0xE000), "a private-use code point has no name");
    int ascending = 1;
    for (int i = 1; i < uunicode_named_count(); i++) ascending &= uunicode_named(i - 1) < uunicode_named(i);
    utest_check(ascending, "the named code points are ascending, no repeats");

    int c, ok;
    ok = finds("rightwards arrow", 0x2192, &c);
    utest_checkf(ok, "'rightwards arrow' finds U+2192 (%d found)", c);
    ok = finds("ARROW Rightwards", 0x2192, &c);
    utest_checkf(ok, "word order and case do not matter (%d found)", c);
    ok = finds("U+2192", 0x2192, &c) && c == 1;
    utest_checkf(ok, "'U+2192' is that code point alone (%d)", c);
    ok = finds("u+e9", 0xE9, &c) && c == 1;
    utest_checkf(ok, "'u+e9' is too: U+ allows fewer than four digits (%d)", c);
    ok = finds("00E9", 0xE9, &c) && c == 1;
    utest_checkf(ok, "bare hex of four digits is a code point (%d)", c);
    ok = finds("\xE2\x86\x92", 0x2192, &c) && c == 1;
    utest_checkf(ok, "the arrow given as itself, in UTF-8 (%d)", c);
    ok = finds("\xE9", 0xE9, &c) && c == 1;
    utest_checkf(ok, "e-acute given as its one Latin-1 byte (%d)", c);
    ok = !finds("rightwards zebra", 0x2192, &c) && c == 0;
    utest_checkf(ok, "every word must match (%d found)", c);
    ok = finds("arrow", 0x2190, &c) && finds("arrow", 0x2193, &c) && c >= 4;
    utest_checkf(ok, "'arrow' finds the four (%d)", c);
    // A query word matches the START of a word in the name: "arr" finds
    // the arrows as it is typed, and "ow" -- inside ARROW -- finds none.
    ok = finds("arr", 0x2192, &c);
    utest_checkf(ok, "a prefix finds as it is typed (%d)", c);
    ok = !finds("ow", 0x2192, &c);
    utest_checkf(ok, "the middle of a word does not (%d found)", c);
    uint32_t out[128];
    int k = uunicode_search("e", out, 128), starts = 1;
    for (int i = 0; i < k && i < 128; i++) {
        const char *nm = uunicode_name(out[i]);
        int hit = nm && nm[0] == 'E';
        for (const char *p = nm; p && *p && !hit; p++) hit = (*p == ' ' || *p == '-') && p[1] == 'E';
        starts &= hit;
    }
    utest_checkf(starts && k > 0, "every name 'e' finds has a word starting with E (%d checked)", k);

    char u[5];
    utest_check(uunicode_utf8(0x41, u) == 1 && !strcmp(u, "A"), "U+0041 is one byte");
    utest_check(uunicode_utf8(0xE9, u) == 2 && !strcmp(u, "\xC3\xA9"), "U+00E9 is C3 A9");
    utest_check(uunicode_utf8(0x2192, u) == 3 && !strcmp(u, "\xE2\x86\x92"), "U+2192 is E2 86 92");
    utest_check(uunicode_utf8(0x1F600, u) == 4 && !strcmp(u, "\xF0\x9F\x98\x80"), "U+1F600 is four bytes");
    utest_check(uunicode_utf8(0xD800, u) == 0, "a surrogate encodes to nothing");
    utest_check(uunicode_utf8(0x110000, u) == 0, "past U+10FFFF encodes to nothing");
    int len;
    utest_check(uunicode_utf8_decode("\xE2\x86\x92x", &len) == 0x2192 && len == 3, "E2 86 92 decodes, length 3");
    utest_check(uunicode_utf8_decode("\xE9", &len) == 0xFFFD && len == 1, "a lone lead byte is U+FFFD, length 1");
    utest_check(uunicode_utf8_decode("\xE2\x41", &len) == 0xFFFD && len == 1, "a broken sequence is U+FFFD, length 1");

    int b = uunicode_block_of(0x3B1);
    utest_checkf(b >= 0 && !strcmp(uunicode_block(b)->name, "Greek and Coptic"), "alpha is in Greek and Coptic (%s)",
                 b >= 0 ? uunicode_block(b)->name : "none");
    b = uunicode_block_of(0x41);
    utest_check(b >= 0 && uunicode_block(b)->lo == 0 && uunicode_block(b)->hi == 0x7F, "A is in Basic Latin, 0-7F");
    int sorted = 1;
    for (int i = 1; i < uunicode_block_count(); i++) sorted &= uunicode_block(i - 1)->hi < uunicode_block(i)->lo;
    utest_checkf(sorted && uunicode_block_count() > 10, "%d blocks, ascending and disjoint", uunicode_block_count());

    return utest_end();
}
