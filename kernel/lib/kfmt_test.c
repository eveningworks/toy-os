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

    // Numbers are unaffected: '-' is parsed and ignored there, so no
    // existing zero-padded conversion changes meaning.
    k_snprintf(b, sizeof b, "%4u|%-4u", 7u, 7u);
    KTEST_ASSERT(eq(b, "0007|0007"));
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
