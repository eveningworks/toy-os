// The ring-3 half of the toolkit: lib/string.h's C names and
// lib/stdio.h's snprintf, which is kfmt's formatter linked into a
// userland ELF for the first time.
//
// WHY THIS EXISTS RATHER THAN A KTEST. The k_* functions underneath all
// have KTESTs already, and those cover the LOGIC -- they would go on
// passing whether or not any of this were reachable from ring 3. What is
// new here is not logic, it is the link: kfmt.c became freestanding so
// build/userland/shared/kfmt.o could join libuapp.a, and the four mem*
// symbols in userland/lib/cmem.c exist only in userland. A KTEST runs
// inside the kernel and cannot see any of that. So the assertions below
// are mostly about C's contract (memset takes an int, all three mem*
// return their destination) and about the formatter producing the same
// bytes it produces in the kernel -- not about re-testing k_strlen.
//
// Prints one line per check and exits with the number of FAILURES, so
// `run libc_test` reports 0 when everything holds. `strace libc_test`
// shows nothing interesting; the output is the point.
#include <stdint.h>
#include "rt/sys.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>

#include "lib/utest.h"

// A NULL the compiler cannot fold back into a literal -- see its one
// use, in the snprintf section below.
static const char *volatile g_nullstr;

int main(void) {
    utest_begin("libc_test", "lib/string.h + lib/stdio.h in ring 3", 0);

    // --- the four real symbols, on C's contract, not k_*'s ------------
    // Each returns its destination, which the k_* originals do not, and
    // memset takes an int rather than a uint8_t. Those signature
    // differences are the entire reason cmem.c is not just an alias.
    char buf[32];
    utest_check(memset(buf, 'x', 4) == buf, "memset returns dst");
    utest_check(buf[0] == 'x' && buf[3] == 'x', "memset filled");
    // 'A' + 256 truncates to 'A' -- C says the int is converted to
    // unsigned char, so this must NOT write 0x141 or clamp.
    memset(buf, 'A' + 256, 2);
    utest_check(buf[0] == 'A' && buf[1] == 'A', "memset truncates its int to a byte");

    utest_check(memcpy(buf, "hello", 6) == buf, "memcpy returns dst");
    utest_check(strcmp(buf, "hello") == 0, "memcpy copied");

    // Overlapping, forwards -- the case that separates memmove from
    // memcpy. "hello" shifted one right is "hhello".
    utest_check(memmove(buf + 1, buf, 5) == buf + 1, "memmove returns dst");
    buf[6] = '\0';
    utest_check(strcmp(buf, "hhello") == 0, "memmove handles overlap");

    utest_check(memcmp("abc", "abd", 3) < 0, "memcmp orders");
    utest_check(memcmp("abc", "abc", 3) == 0, "memcmp equal");

    // --- the inline wrappers -----------------------------------------
    utest_check(strlen("") == 0 && strlen("abc") == 3, "strlen");
    utest_check(strcmp("a", "a") == 0 && strcmp("a", "b") < 0, "strcmp");
    utest_check(strncmp("abcd", "abzz", 2) == 0, "strncmp");
    utest_check(strchr("a/b", '/') != 0 && strrchr("a/b/c", '/')[1] == 'c', "strchr/strrchr");
    utest_check(strstr("haystack", "sta") != 0 && strstr("abc", "zz") == 0, "strstr");
    utest_check(strcasecmp("AbC", "aBc") == 0, "strcasecmp");
    utest_check(isdigit('7') && !isdigit('x') && isspace(' ') && !isspace('x'), "isdigit/isspace");
    utest_check(tolower('Q') == 'q' && toupper('q') == 'Q', "tolower/toupper");

    // strlcpy's contract is the reason it is here instead of strncpy:
    // always NUL-terminates, and returns strlen(src) so a caller can
    // see truncation.
    char small[4];
    size_t want = strlcpy(small, "abcdef", sizeof small);
    utest_check(want == 6, "strlcpy returns the source length");
    utest_check(strcmp(small, "abc") == 0, "strlcpy truncates and terminates");

    // --- snprintf: the capability this change actually added ---------
    // Every one of these is a byte-for-byte expectation, because the
    // point is that a ring-3 caller gets the SAME formatter the kernel
    // uses, not merely one that produces something plausible.
    size_t n = snprintf(buf, sizeof buf, "%s=%d %u %x", "n", -42, 7u, 255u);
    utest_check(strcmp(buf, "n=-42 7 ff") == 0, "snprintf %s %d %u %x");
    utest_check(n == 10, "snprintf returns the length");

    utest_check(snprintf(buf, sizeof buf, "%04x|%02u", 0xabu, 5u) == 7 &&
           strcmp(buf, "00ab|05") == 0, "snprintf zero-pad widths");

    // Through g_nullstr, not a literal 0: GCC diagnoses a literal NULL
    // for %s at compile time (which is -Wformat doing its job through
    // the macro -- see lib/stdio.h on why snprintf is not an inline),
    // and the behaviour under test is the runtime one. It is `volatile`
    // and not derived from anything: the first version computed it from
    // the failure count to hide the NULL from the optimiser, which made this check
    // fail whenever an EARLIER check had -- a test reporting a failure
    // it did not find. The positive control is what exposed that.
    utest_check(snprintf(buf, sizeof buf, "%c%%%s", 'z', g_nullstr) == 8 &&
           strcmp(buf, "z%(null)") == 0, "snprintf %c, %%, NULL %s");

    // C99 truncation semantics: the return is what it WOULD have been,
    // and the buffer is still terminated. This is the half a caller
    // needs in order to detect that it lost bytes.
    char tiny[5];
    size_t big = snprintf(tiny, sizeof tiny, "0123456789");
    utest_check(big == 10, "snprintf returns the untruncated length");
    utest_check(strcmp(tiny, "0123") == 0, "snprintf truncates and terminates");

    // 64-bit through the length modifier. Without the `l` this would
    // read only the low half -- see kfmt.h on why that matters.
    utest_check(snprintf(buf, sizeof buf, "%lx", 0x1122334455UL) == 10 &&
           strcmp(buf, "1122334455") == 0, "snprintf %lx is 64-bit");

    return utest_end();
}
