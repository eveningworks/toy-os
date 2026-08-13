// Tests for knum.c -- the first time this kernel's number formatting
// has had any. That's the point of the library: the nine copies it
// replaced each wrote straight to a screen or a log, and you can't
// assert on something that has already been printed.
#include "ktest.h"
#include "knum.h"
#include "string.h"

static int eq(const char *a, const char *b) { return k_strcmp(a, b) == 0; }

KTEST("knum", "unsigned and signed decimal, including the extremes") {
    char b[32];

    KTEST_ASSERT_EQ(k_utoa(0, b, sizeof b), 1);
    KTEST_ASSERT(eq(b, "0"));
    k_utoa(4096, b, sizeof b);
    KTEST_ASSERT(eq(b, "4096"));
    k_utoa(18446744073709551615ULL, b, sizeof b); // UINT64_MAX
    KTEST_ASSERT(eq(b, "18446744073709551615"));

    k_itoa(-12, b, sizeof b);
    KTEST_ASSERT(eq(b, "-12"));
    k_itoa(0, b, sizeof b);
    KTEST_ASSERT(eq(b, "0"));
    // INT64_MIN: negating it in signed arithmetic overflows, which is
    // exactly the case a hand-rolled copy gets wrong.
    k_itoa(-9223372036854775807LL - 1, b, sizeof b);
    KTEST_ASSERT(eq(b, "-9223372036854775808"));
}

KTEST("knum", "padding adds digits but never truncates") {
    char b[32];

    k_utoa_pad(7, b, sizeof b, 2);
    KTEST_ASSERT(eq(b, "07"));
    k_utoa_pad(12345, b, sizeof b, 2); // longer than the pad width
    KTEST_ASSERT(eq(b, "12345"));
    k_utoa_pad(0, b, sizeof b, 4);
    KTEST_ASSERT(eq(b, "0000"));
}

KTEST("knum", "hex is unprefixed, lowercase, optionally fixed-width") {
    char b[32];

    k_htoa(0x1f, b, sizeof b, 0);
    KTEST_ASSERT(eq(b, "1f"));
    k_htoa(0x1f, b, sizeof b, 4); // a table column, e.g. pci.c's
    KTEST_ASSERT(eq(b, "001f"));
    k_htoa(0, b, sizeof b, 0);
    KTEST_ASSERT(eq(b, "0"));
    k_htoa(0x8000100000ULL, b, sizeof b, 0);
    KTEST_ASSERT(eq(b, "8000100000"));
    k_htoa(0xffffffffffffffffULL, b, sizeof b, 0);
    KTEST_ASSERT(eq(b, "ffffffffffffffff"));
}

KTEST("knum", "a result that doesn't fit produces nothing, not a prefix") {
    char b[4];
    // "12345" needs 6 bytes with its NUL. A truncated number is a WRONG
    // number, so it must not appear at all.
    KTEST_ASSERT_EQ(k_utoa(12345, b, sizeof b), 0);
    KTEST_ASSERT_EQ(b[0], '\0');

    KTEST_ASSERT_EQ(k_htoa(0xabcdef, b, sizeof b, 0), 0);
    KTEST_ASSERT_EQ(b[0], '\0');

    // Exactly fitting is fine: "123" + NUL is 4.
    KTEST_ASSERT_EQ(k_utoa(123, b, sizeof b), 3);
    KTEST_ASSERT(eq(b, "123"));
}

KTEST("knum", "parsers reject rather than guess") {
    uint32_t u32;
    uint64_t u64;
    int64_t i64;

    KTEST_ASSERT(k_parse_u32("4096", &u32));
    KTEST_ASSERT_EQ(u32, 4096);
    KTEST_ASSERT_EQ(k_parse_u32("", &u32), 0);        // empty
    KTEST_ASSERT_EQ(k_parse_u32("12a", &u32), 0);     // trailing junk
    KTEST_ASSERT_EQ(k_parse_u32("a12", &u32), 0);     // leading junk
    KTEST_ASSERT_EQ(k_parse_u32(" 12", &u32), 0);     // no whitespace skipping
    KTEST_ASSERT_EQ(k_parse_u32("-1", &u32), 0);      // unsigned means unsigned
    KTEST_ASSERT_EQ(k_parse_u32("4294967296", &u32), 0); // > UINT32_MAX

    // A rejected parse leaves the output alone.
    u32 = 0xdead;
    KTEST_ASSERT_EQ(k_parse_u32("nope", &u32), 0);
    KTEST_ASSERT_EQ(u32, 0xdead);

    KTEST_ASSERT(k_parse_u64("18446744073709551615", &u64));
    KTEST_ASSERT_EQ(k_parse_u64("18446744073709551616", &u64), 0); // overflow

    KTEST_ASSERT(k_parse_i64("-720", &i64));
    KTEST_ASSERT_EQ(i64, -720);
    KTEST_ASSERT(k_parse_i64("840", &i64));
    KTEST_ASSERT_EQ(i64, 840);
    KTEST_ASSERT_EQ(k_parse_i64("-", &i64), 0);
}

KTEST("knum", "bounded parsing reads a field without copying it out") {
    // tz.c's city rows and keyboard_layout.c's escapes both parse a
    // span inside a bigger buffer -- the whole span must be the number.
    const char *row = "helsinki:120:eu";
    uint64_t v;
    KTEST_ASSERT(k_parse_u64_n(row + 9, 3, &v));
    KTEST_ASSERT_EQ(v, 120);
    KTEST_ASSERT_EQ(k_parse_u64_n(row + 9, 4, &v), 0); // span includes ':'

    int64_t s;
    KTEST_ASSERT(k_parse_i64_n("-300xx", 4, &s));
    KTEST_ASSERT_EQ(s, -300);
}

KTEST("knum", "hex parsing, with or without the 0x prefix") {
    uint64_t v;

    KTEST_ASSERT(k_parse_hex("1f", &v));
    KTEST_ASSERT_EQ(v, 0x1f);
    KTEST_ASSERT(k_parse_hex("0x1F", &v)); // prefix + uppercase
    KTEST_ASSERT_EQ(v, 0x1f);
    KTEST_ASSERT(k_parse_hex("ffffffffffffffff", &v));
    KTEST_ASSERT_EQ((int64_t)v, -1); // all ones
    KTEST_ASSERT_EQ(k_parse_hex("0x", &v), 0);   // a prefix is not a number
    KTEST_ASSERT_EQ(k_parse_hex("0xg", &v), 0);
    KTEST_ASSERT_EQ(k_parse_hex("1ffffffffffffffff", &v), 0); // > 64 bits

    KTEST_ASSERT_EQ(k_hex_digit('0'), 0);
    KTEST_ASSERT_EQ(k_hex_digit('a'), 10);
    KTEST_ASSERT_EQ(k_hex_digit('F'), 15);
    KTEST_ASSERT_EQ(k_hex_digit('g'), -1);
}
