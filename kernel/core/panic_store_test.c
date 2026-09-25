// Tests for the panic record format and `panic=`. The store itself is a
// fixed range of physical RAM that only a warm reset exercises; that half
// is tools/panic_store_test.py's, which panics a guest and reads the
// record back on the next boot.
#include "ktest.h"
#include "panic_store.h"
#include "panic.h"
#include "kcrc.h"
#include "string.h"

#define CAP 512
static char g_buf[CAP];

static int sealed(const char *text) {
    uint32_t n = (uint32_t)k_strlen(text);
    k_memset(g_buf, 0, sizeof g_buf);
    k_memcpy(PANIC_RECORD_TEXT(g_buf), text, n);
    return panic_record_seal(g_buf, CAP, n, 42000000000ull, "test-build");
}

KTEST("panic_store", "a sealed record checks, and says how long it is") {
    KTEST_ASSERT(sealed("PANIC: Page fault\n"));
    KTEST_ASSERT_EQ(panic_record_check(g_buf, CAP), 18);
}

// The checksum is compared against kcrc32() over the same bytes, so a
// seal and a check that agreed on a WRONG CRC cannot both pass here.
KTEST("panic_store", "the checksum is CRC-32 over header then text") {
    KTEST_ASSERT(sealed("abc"));
    char copy[PANIC_RECORD_HDR + 3];
    k_memcpy(copy, g_buf, sizeof copy);
    uint32_t stored;
    k_memcpy(&stored, copy + 12, 4);
    k_memset(copy + 12, 0, 4);
    KTEST_ASSERT_EQ(stored, kcrc32(copy, sizeof copy));
}

KTEST("panic_store", "one flipped byte of text is rejected") {
    KTEST_ASSERT(sealed("PANIC: General protection fault\n"));
    PANIC_RECORD_TEXT(g_buf)[3] ^= 0x20;
    KTEST_ASSERT_EQ(panic_record_check(g_buf, CAP), -1);
}

KTEST("panic_store", "cold-boot shapes are rejected: zeroes, 0xFF, a bad length") {
    k_memset(g_buf, 0, sizeof g_buf);
    KTEST_ASSERT_EQ(panic_record_check(g_buf, CAP), -1);
    k_memset(g_buf, 0xFF, sizeof g_buf);
    KTEST_ASSERT_EQ(panic_record_check(g_buf, CAP), -1);
    // A length past the buffer must be refused BEFORE the CRC walks it.
    KTEST_ASSERT(sealed("x"));
    uint32_t huge = 0x7FFFFFFF;
    k_memcpy(g_buf + 8, &huge, 4);
    KTEST_ASSERT_EQ(panic_record_check(g_buf, CAP), -1);
}

KTEST("panic_store", "a record that does not fit is not sealed") {
    KTEST_ASSERT(!panic_record_seal(g_buf, CAP, CAP, 0, "b"));
    KTEST_ASSERT(!panic_record_seal(g_buf, PANIC_RECORD_HDR - 1, 0, 0, "b"));
    KTEST_ASSERT(panic_record_seal(g_buf, CAP, CAP - PANIC_RECORD_HDR, 0, "b"));
}

KTEST("panic_store", "panic= parses seconds, and rejects what is not a number") {
    int s = 99;
    KTEST_ASSERT(panic_parse_secs("10", &s));
    KTEST_ASSERT_EQ(s, 10);
    KTEST_ASSERT(panic_parse_secs("0", &s));
    KTEST_ASSERT_EQ(s, 0);
    KTEST_ASSERT(panic_parse_secs("-5", &s));
    KTEST_ASSERT_EQ(s, -1);
    s = 7;
    KTEST_ASSERT(!panic_parse_secs("soon", &s));
    KTEST_ASSERT_EQ(s, 7);
    KTEST_ASSERT(!panic_parse_secs("", &s));
    KTEST_ASSERT(!panic_parse_secs(0, &s));
}
