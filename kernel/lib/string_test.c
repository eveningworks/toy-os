// Tests for string.c. The six original functions predate the test
// suite entirely and had none; the batch added alongside knum/kfmt/
// kpath gets them from the start, and the originals get them now too.
#include "ktest.h"
#include "string.h"

static int eq(const char *a, const char *b) { return k_strcmp(a, b) == 0; }

KTEST("string", "the original six still do what callers assume") {
    KTEST_ASSERT_EQ(k_strlen(""), 0);
    KTEST_ASSERT_EQ(k_strlen("toy-os"), 6);
    KTEST_ASSERT(k_strcmp("a", "a") == 0);
    KTEST_ASSERT(k_strcmp("a", "b") < 0);
    KTEST_ASSERT(k_strcmp("b", "a") > 0);
    KTEST_ASSERT(k_strncmp("abcXX", "abcYY", 3) == 0);
    KTEST_ASSERT(k_strncmp("abcXX", "abcYY", 4) != 0);

    char buf[8];
    k_memset(buf, 0xAB, sizeof buf);
    KTEST_ASSERT_EQ((uint8_t)buf[0], 0xAB);
    KTEST_ASSERT_EQ((uint8_t)buf[7], 0xAB);

    k_strcpy(buf, "hi");
    KTEST_ASSERT(eq(buf, "hi"));
}

KTEST("string", "k_strlcpy always terminates and reports truncation") {
    char buf[4];

    KTEST_ASSERT_EQ(k_strlcpy(buf, "ab", sizeof buf), 2);
    KTEST_ASSERT(eq(buf, "ab"));

    // "abcdef" doesn't fit: truncated, still terminated, and the return
    // is the length it WANTED -- that's how a caller detects it.
    KTEST_ASSERT_EQ(k_strlcpy(buf, "abcdef", sizeof buf), 6);
    KTEST_ASSERT(eq(buf, "abc"));

    KTEST_ASSERT_EQ(k_strlcpy(buf, "", sizeof buf), 0);
    KTEST_ASSERT_EQ(buf[0], '\0');
}

KTEST("string", "strchr, strrchr and strstr") {
    const char *path = "/docs/notes/todo.txt";

    KTEST_ASSERT(k_strchr(path, '/') == path);          // first
    KTEST_ASSERT(eq(k_strrchr(path, '/'), "/todo.txt")); // last
    KTEST_ASSERT(k_strchr(path, 'Z') == 0);
    // A NUL search finds the terminator, matching C's strchr.
    KTEST_ASSERT(k_strchr(path, '\0') == path + k_strlen(path));

    KTEST_ASSERT(eq(k_strstr(path, "notes"), "notes/todo.txt"));
    KTEST_ASSERT(k_strstr(path, "nope") == 0);
    KTEST_ASSERT(k_strstr(path, "") == path); // empty needle matches at 0
    // A partial match that then diverges must not stop the search --
    // the bug a naive single-pass scan has.
    KTEST_ASSERT(eq(k_strstr("aab", "ab"), "ab"));
}

KTEST("string", "memcmp and overlapping memmove") {
    KTEST_ASSERT_EQ(k_memcmp("abc", "abc", 3), 0);
    KTEST_ASSERT(k_memcmp("abc", "abd", 3) < 0);
    KTEST_ASSERT_EQ(k_memcmp("abc", "abd", 2), 0); // only n bytes matter

    // Overlap in both directions -- the case k_memcpy gets wrong.
    char buf[8];
    k_strcpy(buf, "abcdef");
    k_memmove(buf + 1, buf, 6); // shift right: dst > src
    buf[7] = '\0';
    KTEST_ASSERT(eq(buf, "aabcdef"));

    k_strcpy(buf, "abcdef");
    k_memmove(buf, buf + 1, 6); // shift left: dst < src
    KTEST_ASSERT(eq(buf, "bcdef"));
}

KTEST("string", "character classes") {
    KTEST_ASSERT(k_isdigit('7'));
    KTEST_ASSERT(!k_isdigit('a'));
    KTEST_ASSERT(!k_isdigit('/')); // just below '0' in ASCII
    KTEST_ASSERT(!k_isdigit(':')); // just above '9'
    KTEST_ASSERT(k_isspace(' ') && k_isspace('\t') && k_isspace('\n'));
    KTEST_ASSERT(!k_isspace('x'));
    KTEST_ASSERT(!k_isspace('\0'));
}

KTEST("string", "ASCII case folding") {
    KTEST_ASSERT_EQ(k_tolower('A'), 'a');
    KTEST_ASSERT_EQ(k_tolower('z'), 'z'); // already folded
    KTEST_ASSERT_EQ(k_toupper('a'), 'A');
    KTEST_ASSERT_EQ(k_toupper('Z'), 'Z');

    // The bytes bracketing each range, where an off-by-one would land.
    // '@'/'[' sit either side of A-Z, '`'/'{' either side of a-z.
    KTEST_ASSERT_EQ(k_tolower('@'), '@');
    KTEST_ASSERT_EQ(k_tolower('['), '[');
    KTEST_ASSERT_EQ(k_toupper('`'), '`');
    KTEST_ASSERT_EQ(k_toupper('{'), '{');
    KTEST_ASSERT_EQ(k_tolower('5'), '5');

    // Latin-1 is deliberately NOT folded -- Ä (0xC4) stays Ä, and the
    // signed-char hazard is why these take an int: passing the byte in
    // as a plain char would arrive as -60, and the range checks must
    // leave that alone too rather than wrapping it into a letter.
    KTEST_ASSERT_EQ(k_tolower(0xC4), 0xC4);
    KTEST_ASSERT_EQ(k_toupper(0xE4), 0xE4);
    KTEST_ASSERT_EQ(k_tolower((char)0xC4), (char)0xC4);
}

KTEST("string", "strcasecmp") {
    KTEST_ASSERT_EQ(k_strcasecmp("helsinki", "Helsinki"), 0);
    KTEST_ASSERT_EQ(k_strcasecmp("LOSANGELES", "losangeles"), 0);
    KTEST_ASSERT_EQ(k_strcasecmp("", ""), 0);
    KTEST_ASSERT(k_strcasecmp("abc", "abd") < 0);
    KTEST_ASSERT(k_strcasecmp("ABD", "abc") > 0);

    // A prefix is still shorter, not equal.
    KTEST_ASSERT(k_strcasecmp("abc", "ABCD") < 0);
    KTEST_ASSERT(k_strcasecmp("ABCD", "abc") > 0);

    // Ordering comes from the FOLDED bytes: raw ASCII puts every
    // uppercase letter below every lowercase one, so a raw comparison
    // would call 'Z' < 'a'. Folded, 'z' > 'a'.
    KTEST_ASSERT(k_strcasecmp("Z", "a") > 0);

    // Non-letters aren't folded, so they still compare by raw value --
    // '_' (0x5F) sits between the two letter ranges.
    KTEST_ASSERT(k_strcasecmp("a_b", "a_B") == 0);
    KTEST_ASSERT(k_strcasecmp("_", "a") < 0);

    // Latin-1 compares as an unsigned byte, not a negative signed char,
    // so å (0xE5) sorts after 'a' rather than before everything.
    KTEST_ASSERT(k_strcasecmp("\xE5", "a") > 0);
}
