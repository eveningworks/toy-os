// Tests for kfmt.c. The truncation and unrecognised-conversion cases
// matter most: both are places where a formatter can quietly produce
// something wrong rather than failing, and both are the reason this is
// one shared implementation instead of six.
#include "ktest.h"
#include "kfmt.h"
#include "string.h"

static int eq(const char *a, const char *b) { return k_strcmp(a, b) == 0; }

KTEST("kfmt", "a string width pads, and '-' left-justifies") {
    char b[64];

    // The columns a table needs. Before these existed, `%-12s` emitted
    // the specifier itself into the output -- which reads as working
    // right up until you look at the result.
    k_snprintf(b, sizeof b, "[%-6s]", "ab");
    KTEST_ASSERT(eq(b, "[ab    ]"));

    k_snprintf(b, sizeof b, "[%6s]", "ab");
    KTEST_ASSERT(eq(b, "[    ab]"));

    // Exactly the field width: no padding either way.
    k_snprintf(b, sizeof b, "[%-2s][%2s]", "ab", "cd");
    KTEST_ASSERT(eq(b, "[ab][cd]"));

    // OVER the field width is the case worth pinning: the column is
    // pushed, the value is NOT truncated. A formatter that shortened a
    // value to fit would be reporting something other than what it was
    // given, which this toolkit's formatters never do (see kfmt.h).
    k_snprintf(b, sizeof b, "[%-3s]", "abcdef");
    KTEST_ASSERT(eq(b, "[abcdef]"));

    // A null argument still pads, rather than skipping the field and
    // shifting every later column left.
    // Through a volatile, or -Wformat-overflow objects at compile time
    // to the very case being tested at run time.
    const char *volatile nul = 0;
    k_snprintf(b, sizeof b, "[%-8s]", nul);
    KTEST_ASSERT(eq(b, "[(null)  ]"));

    // NUMBERS NOW FOLLOW C, and this assertion changed with them. It
    // read "0007|0007" while a width meant zero-padding and '-' was
    // parsed-then-ignored on a number -- which was fine when the only
    // callers were %02x register dumps, and wrong the moment anything
    // wanted a column. `%5d` printing 00042 is not what C means.
    // Zero padding is still available and still spelled with an
    // explicit '0'; see this file's width/flag test below.
    k_snprintf(b, sizeof b, "%4u|%-4u", 7u, 7u);
    KTEST_ASSERT(eq(b, "   7|7   "));
}

KTEST("kfmt", "the supported conversions") {
    char b[64];

    k_snprintf(b, sizeof b, "plain");
    KTEST_ASSERT(eq(b, "plain"));

    k_snprintf(b, sizeof b, "%d %d %u", -12, 0, 4096u);
    KTEST_ASSERT(eq(b, "-12 0 4096"));

    k_snprintf(b, sizeof b, "%x %lx", 0x1fu, 0x8000100000UL);
    KTEST_ASSERT(eq(b, "1f 8000100000"));

    // The NULL is deliberate -- a diagnostic printing a string that
    // turned out to be NULL should say so, not fault. GCC correctly
    // objects to a literal NULL for %s, which is exactly the check
    // working; silenced only here.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-overflow"
    const char *nothing = 0;
    k_snprintf(b, sizeof b, "[%s] [%s] %c%c", "hi", nothing, 'o', 'k');
#pragma GCC diagnostic pop
    KTEST_ASSERT(eq(b, "[hi] [(null)] ok"));

    k_snprintf(b, sizeof b, "100%%");
    KTEST_ASSERT(eq(b, "100%"));
}

KTEST("kfmt", "zero-pad widths, including after a minus sign") {
    char b[64];

    k_snprintf(b, sizeof b, "%04x", 0x1fu);
    KTEST_ASSERT(eq(b, "001f"));
    k_snprintf(b, sizeof b, "%02u:%02u:%02u", 9u, 5u, 0u); // kernel.c's clock
    KTEST_ASSERT(eq(b, "09:05:00"));
    // The pad goes after the sign, not before it.
    k_snprintf(b, sizeof b, "%04d", -7);
    KTEST_ASSERT(eq(b, "-007"));
    // A value wider than the pad keeps all its digits.
    k_snprintf(b, sizeof b, "%02u", 12345u);
    KTEST_ASSERT(eq(b, "12345"));
}

KTEST("kfmt", "truncation is detectable, and never unterminated") {
    char b[8];
    for (unsigned i = 0; i < sizeof b; i++) b[i] = (char)0x7f;

    // C99 semantics: returns the length it WANTED, so >= cap means the
    // caller can tell it lost bytes.
    size_t want = k_snprintf(b, sizeof b, "%s-%s", "abcdef", "ghijkl");
    KTEST_ASSERT_EQ(want, 13);
    KTEST_ASSERT(eq(b, "abcdef-"));   // exactly cap-1 bytes stored
    KTEST_ASSERT_EQ(b[7], '\0');      // always terminated

    // cap == 0 must write nothing at all, not even a NUL.
    char guard = 0x7f;
    KTEST_ASSERT_EQ(k_snprintf(&guard, 0, "xyz"), 3);
    KTEST_ASSERT_EQ(guard, 0x7f);
}

// Both formats below are malformed ON PURPOSE -- that's the behavior
// under test. GCC's -Wformat catches them at every real call site,
// which is the point of kfmt.h's format attributes; it has to be
// switched off for the two calls that are deliberately wrong.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
KTEST("kfmt", "an unrecognised conversion prints literally and eats no argument") {
    char b[64];

    // %q isn't supported. If it silently consumed an argument, the
    // following %u would read the wrong one -- far worse than a visibly
    // odd line, which is why the fallback is "emit it verbatim".
    k_snprintf(b, sizeof b, "%q %u", 42u);
    KTEST_ASSERT(eq(b, "%q 42"));

    // A format ending mid-conversion must terminate rather than run off
    // the end of the string.
    k_snprintf(b, sizeof b, "trailing %");
    KTEST_ASSERT(eq(b, "trailing %"));
}
#pragma GCC diagnostic pop

// --- the sink form -------------------------------------------------
//
// k_vcbprintf() is what the C library's printf() is built on, so what
// matters is that it agrees with k_snprintf() -- one formatter, two
// entry points -- and that it is NOT bounded by any buffer this file
// chose.
// NOT zero-initialised as a whole: `= {{0},0}` on a 512-byte array
// makes GCC emit a call to memcpy/memset, which a freestanding kernel
// does not have. Set the two fields that matter instead.
struct sinkbuf { char b[512]; size_t n; };
static void sink_reset(struct sinkbuf *sb) { sb->n = 0; sb->b[0] = '\0'; }

static void sink_collect(void *ctx, const char *s, size_t n) {
    struct sinkbuf *sb = (struct sinkbuf *)ctx;
    for (size_t i = 0; i < n && sb->n + 1 < sizeof sb->b; i++) sb->b[sb->n++] = s[i];
    sb->b[sb->n] = '\0';
}

KTEST("kfmt", "the sink form produces exactly what snprintf would") {
    struct sinkbuf sb;
    sink_reset(&sb);
    char want[128];
    size_t nw = k_snprintf(want, sizeof want, "%s=%d [%04x] %-6s|", "id", -42, 255, "ab");
    size_t ns = k_cbprintf(sink_collect, &sb, "%s=%d [%04x] %-6s|", "id", -42, 255, "ab");
    KTEST_ASSERT(eq(sb.b, want));
    KTEST_ASSERT(ns == nw);
}

KTEST("kfmt", "the sink form is not bounded by KFMT_LINE_MAX") {
    // A line longer than the fixed buffer vga_printf()/klog_printf()
    // use. snprintf into that size would report the full length and
    // store only part; a sink stores all of it, which is the whole
    // reason printf() is built on this and not on a scratch buffer.
    //
    // Length comes from STRINGS, not from a pad width: a numeric width
    // is bounded by put_num()'s 24-byte scratch, so `%400u` does not
    // produce 400 characters. That is a pre-existing property of the
    // number path and nothing to do with the sink -- it is written down
    // here because the obvious way to write this test walks into it.
    const char *s60 = "012345678901234567890123456789"
                      "012345678901234567890123456789";
    struct sinkbuf sb;
    sink_reset(&sb);
    size_t n = k_cbprintf(sink_collect, &sb, "%s%s%s%s%s", s60, s60, s60, s60, s60);
    KTEST_ASSERT(n == 300);
    KTEST_ASSERT(n > KFMT_LINE_MAX);
    KTEST_ASSERT(sb.n == 300);
    KTEST_ASSERT(sb.b[0] == '0' && sb.b[299] == '9');
}

// --- the kernel has no floating point, and says so ------------------
//
// The half of the k_fmt_float() split that lives in the KERNEL
// (kfmt_nofloat.c) returns 0, and this is what proves the formatter
// then does the right thing with that. It matters because the wrong
// behaviours are both silent: printing a garbage number read out of an
// integer register, or consuming an argument that was never passed and
// desynchronising every conversion after it.
//
// A ring-3 test cannot check this -- there, %f works.
KTEST("kfmt", "%f in the KERNEL emits literally and consumes no argument") {
    char b[64];
    // The %d after it is the real assertion: if %f had eaten an
    // argument, 42 would vanish and %d would read whatever came next.
    //
    // -Wformat IS RIGHT to complain here and the warning is suppressed
    // rather than fixed, because the mismatch is the point: this passes
    // ONE argument for two conversions on purpose, to prove the first
    // consumes none. Silencing it locally keeps the build clean without
    // weakening the check on every other caller in the tree.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
#pragma GCC diagnostic ignored "-Wformat-extra-args"
    k_snprintf(b, sizeof b, "[%f][%d]", 42);
#pragma GCC diagnostic pop
    KTEST_ASSERT(eq(b, "[%f][42]"));
}

KTEST("kfmt", "a width pads with SPACES, and '0' asks for zeroes") {
    char b[64];
    k_snprintf(b, sizeof b, "[%5u][%05u][%-5u]", 42u, 42u, 42u);
    KTEST_ASSERT(eq(b, "[   42][00042][42   ]"));
    // The signed path pads after the sign, which is the case a naive
    // "prepend zeroes" gets wrong: -0042, not 00-42.
    k_snprintf(b, sizeof b, "[%6d][%06d]", -42, -42);
    KTEST_ASSERT(eq(b, "[   -42][-00042]"));
    // Hex keeps the register-dump behaviour every existing caller
    // relies on.
    k_snprintf(b, sizeof b, "[%04x][%4x]", 0xabu, 0xabu);
    KTEST_ASSERT(eq(b, "[00ab][  ab]"));
}

KTEST("kfmt", "a precision is parsed rather than emitted literally") {
    char b[64];
    // %.3d is not a conversion this formatter honours, but it must be
    // RECOGNISED -- emitting "%.3d" into the output is what happened
    // before precision was parsed, and it looks like a typo in the
    // caller rather than a limit of the formatter.
    k_snprintf(b, sizeof b, "[%.3d]", 5);
    KTEST_ASSERT(eq(b, "[5]"));
}
