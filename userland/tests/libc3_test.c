// Stage 3 of the C library: strtol, realloc, qsort/bsearch, ctype,
// setjmp, dirent, errno-as-an-lvalue (docs/libc-design.md).
//
// THE CHECKS THAT ARE HARD TO FAKE:
//
//  - **strtol is checked on endptr, not on the value.** Any digit loop
//    returns 42 for "42"; what distinguishes strtol() from k_parse_i64()
//    is that it stops at the first unusable byte and says WHERE -- and
//    that "no conversion" gives back the original pointer, which is the
//    only way to tell "0" from "not a number".
//  - **realloc is checked on the CONTENTS after a grow that must move.**
//    A realloc that allocated and forgot to copy passes every size
//    check and fails this.
//  - **setjmp is checked with a volatile counter across the jump**, so
//    a longjmp that never arrived and one that arrived once are
//    distinguishable. A bare "did we get here" flag cannot tell them
//    apart from the code simply falling through.
//
// Prints one line per check and exits with the number of failures.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <ctype.h>
#include <errno.h>
#include <setjmp.h>
#include <dirent.h>
#include <unistd.h>
#include <limits.h>
#include <fcntl.h>   // O_RDWR, for the refusal check
#include "rt/sys.h"

#include "lib/utest.h"


static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

static jmp_buf g_jb;
// TWO named frames rather than a recursive one, so the jump has to
// restore rsp across more than its immediate caller. Written as two
// functions because a recursive version trips -Winfinite-recursion:
// longjmp() is noreturn, so GCC sees a function that never returns
// normally calling itself and cannot tell the base case terminates it.
static void jump_inner(void) { longjmp(g_jb, 7); }
// Reading rsp is the ONLY way this test can see whether longjmp
// restored the stack pointer. Everything else survives without it:
// longjmp also restores rbp, so main's frame stays addressable and a
// stale (lower) rsp merely leaves dead space below -- which is why a
// control that deleted the rsp restore passed every other check here.
// Called from the same frame both times, so its own prologue cancels.
static unsigned long rsp_now(void) {
    unsigned long v;
    __asm__ volatile("mov %%rsp, %0" : "=r"(v));
    return v;
}
static void jump_outer(void) { jump_inner(); }

int main(void) {
    utest_begin("libc3_test", "strtol, realloc, qsort, ctype, setjmp, dirent", 0);

    // --- strtol ------------------------------------------------------
    char *end;
    utest_check(strtol("42", &end, 10) == 42 && *end == '\0', "strtol reads a plain decimal");
    utest_check(strtol("  -17rest", &end, 10) == -17 && strcmp(end, "rest") == 0,
          "skips space, takes a sign, and endptr is the first unused byte");
    utest_check(strtol("ff", &end, 16) == 255, "base 16");

    // --- the long long family ----------------------------------------
    //
    // Their EXISTENCE is half the check: C requires them and they were
    // simply absent, so any program calling one failed to link. The
    // values chosen also need 64 bits, so a delegation to the `int`
    // versions would be caught rather than passing by coincidence.
    utest_check(atoll("9007199254740993") == 9007199254740993LL,
                "atoll carries a value past 2^53");
    utest_check(strtoll("-9223372036854775807", &end, 10) == -9223372036854775807LL
                && *end == '\0', "strtoll reaches LLONG_MIN+1");
    // C really does specify that the unsigned parsers negate.
    utest_check(strtoull("-1", &end, 10) == ~0ULL, "strtoull(\"-1\") is ULLONG_MAX");
    utest_check(llabs(-9007199254740993LL) == 9007199254740993LL,
                "llabs does not truncate to 32 bits");
    utest_check(strtoimax("-42", &end, 10) == -42 && imaxabs((intmax_t)-7) == 7,
                "the intmax_t parsers exist and agree");

    // --- div, and the sign rule that makes it worth having ------------
    //
    // C99 pins `/` toward zero and `%` to the dividend's sign. -7/2 is
    // -3 remainder -1, NOT -4 remainder 1 -- which is the answer a
    // floor-division language gives and the one an open-coded pair
    // drifts into.
    div_t dv = div(-7, 2);
    utest_check(dv.quot == -3 && dv.rem == -1, "div truncates toward zero");
    ldiv_t ld = ldiv(7L, -2L);
    utest_check(ld.quot == -3 && ld.rem == 1, "ldiv keeps the dividend's sign");
    lldiv_t lld = lldiv(9007199254740993LL, 2LL);
    utest_check(lld.quot == 4503599627370496LL && lld.rem == 1,
                "lldiv works past 2^53");

    // --- collation, in the only locale there is -----------------------
    utest_check(strcoll("abc", "abd") < 0 && strcoll("abc", "abc") == 0,
                "strcoll orders like strcmp");
    {
        char x[4];
        // The RETURN is the untruncated length -- that is what makes
        // the standard "call once to size, once to fill" idiom work.
        size_t need = strxfrm(x, "abcdef", sizeof x);
        utest_check(need == 6 && strcmp(x, "abc") == 0,
                    "strxfrm truncates but reports the full length");
    }

    // --- tmpnam + getline ---------------------------------------------
    //
    // Two streams rather than one, because this library has no update
    // mode (see <stdio.h>'s note on `+`). The last line deliberately has
    // NO newline: a reader that only returns complete lines drops it,
    // which is the most common getline() bug.
    {
        char path[L_tmpnam];
        utest_check(tmpnam(path) != NULL && path[0] == '/',
                    "tmpnam builds an absolute scratch path");
        FILE *w = fopen(path, "w");
        utest_check(w != NULL, "and a file can be created at it");
        if (w) {
            fputs("alpha\nbeta\nno-newline", w);
            fclose(w);
        }
        FILE *t = fopen(path, "r");
        if (t) {
            fpos_t start;
            utest_check(fgetpos(t, &start) == 0, "fgetpos reads a position");

            char *line = NULL;
            size_t cap = 0;
            ssize_t n1 = getline(&line, &cap, t);
            utest_check(n1 == 6 && strcmp(line, "alpha\n") == 0,
                        "getline returns the line WITH its newline");
            getline(&line, &cap, t);           // beta
            ssize_t n3 = getline(&line, &cap, t);
            utest_check(n3 == 10 && strcmp(line, "no-newline") == 0,
                        "a final line with no newline is still a line");
            utest_check(getline(&line, &cap, t) == -1, "and then EOF is -1");

            utest_check(fsetpos(t, &start) == 0 &&
                        getline(&line, &cap, t) == 6,
                        "fsetpos returns to the saved position");
            free(line);
            fclose(t);
        }
        remove(path);
    }
    utest_check(strtol("0x1f", &end, 16) == 31 && *end == '\0', "and its optional 0x");
    utest_check(strtol("0x20", &end, 0) == 32, "base 0 detects hex");
    utest_check(strtol("017", &end, 0) == 15, "and octal");
    utest_check(strtol("19", &end, 0) == 19, "and decimal");
    const char *nan = "zz";
    utest_check(strtol(nan, &end, 10) == 0 && end == nan,
          "no conversion returns the ORIGINAL pointer, so 0 is distinguishable");
    // "zz" alone CANNOT catch an implementation that hands back the
    // post-sign, post-prefix position instead of the original -- for
    // that input the two are the same pointer, so a control that broke
    // it changed nothing. These two do reach the branch: the sign and
    // the "0x" have already moved the cursor by the time the digit loop
    // finds nothing.
    const char *nan_signed = "-zz";
    utest_check(strtol(nan_signed, &end, 10) == 0 && end == nan_signed,
          "...including when a SIGN was consumed before the failure");
    // **"0xzz" IS A CONVERSION, and this check used to assert it was
    // not.** The '0' is a valid hex digit and IS converted; only the
    // "xzz" is left over, so glibc returns 0 with endptr one past the
    // start. Asserting `end == nan_hex` enshrined the bug where base 0
    // and base 16 stepped over the prefix and then reported that
    // nothing had been parsed -- so strtol("0", &end, 0) consumed
    // nothing either. Corrected against glibc (tools/libc_diff.py); the
    // sign case above is the one that genuinely has no digits.
    const char *nan_hex = "0xzz";
    utest_check(strtol(nan_hex, &end, 16) == 0 && end == nan_hex + 1,
          "...while an 0x prefix CONVERTS its zero and leaves the rest");
    const char *bare_zero = "0";
    utest_check(strtol(bare_zero, &end, 0) == 0 && end == bare_zero + 1,
          "a lone 0 in base 0 is a conversion, not a refusal");
    utest_check(strtol("7", &end, 10) == 7 && end != (char *)0, "a real 0-valued parse differs");
    errno = 0;
    utest_check(strtol("99999999999999999999999", &end, 10) == LONG_MAX && errno == ERANGE,
          "overflow saturates and sets ERANGE");
    errno = 0;
    utest_check(strtoul("-1", &end, 10) == ULONG_MAX && errno == 0,
          "strtoul negates, as C really does specify");
    utest_check(atoi("  123abc") == 123, "atoi is strtol with the errors dropped");

    // errno is an LVALUE now, not just a function call.
    errno = EINVAL;
    utest_check(errno == EINVAL && sys_errno() == EINVAL,
          "errno is assignable and is the same storage as sys_errno()");

    // --- ctype -------------------------------------------------------
    utest_check(isalpha('a') && isalpha('Z') && !isalpha('0') && !isalpha(' '),
          "isalpha");
    utest_check(isalnum('7') && isalnum('q') && !isalnum('-'), "isalnum");
    utest_check(ispunct('-') && !ispunct('a') && !ispunct(' '), "ispunct excludes space");
    utest_check(isprint(' ') && !isgraph(' '), "isprint includes space, isgraph does not");
    utest_check(iscntrl('\n') && !iscntrl('A') && iscntrl(0x7F), "iscntrl covers DEL");
    utest_check(isxdigit('f') && isxdigit('F') && isxdigit('9') && !isxdigit('g'), "isxdigit");
    utest_check(!isalpha(0xC3), "a UTF-8 lead byte is not alphabetic (ASCII only, on purpose)");

    // --- realloc -----------------------------------------------------
    char *p = (char *)malloc(16);
    utest_check(p != 0, "malloc");
    for (int i = 0; i < 16; i++) p[i] = (char)('a' + i % 26);
    // 4 KiB is far past the 16-byte block, so the allocator has to move
    // it -- which is the case a realloc that forgot to copy survives
    // when the block happens to grow in place.
    char *q = (char *)realloc(p, 4096);
    utest_check(q != 0, "realloc to 4096");
    int kept = 1;
    for (int i = 0; i < 16; i++) if (q[i] != (char)('a' + i % 26)) kept = 0;
    utest_check(kept, "and the original 16 bytes survived the move");
    q = (char *)realloc(q, 8);
    utest_check(q != 0 && q[0] == 'a' && q[7] == 'h', "shrinking keeps the prefix");
    utest_check(realloc(q, 0) == 0, "realloc(p, 0) frees and returns NULL");
    utest_check(realloc(0, 8) != 0, "realloc(NULL, n) is malloc");

    // --- qsort / bsearch ---------------------------------------------
    static int a[9] = { 5, -3, 9, 0, 5, 12, -100, 7, 1 };
    qsort(a, 9, sizeof a[0], cmp_int);
    int sorted = 1;
    for (int i = 1; i < 9; i++) if (a[i - 1] > a[i]) sorted = 0;
    utest_check(sorted, "qsort orders ascending");
    utest_check(a[0] == -100 && a[8] == 12, "with the extremes at the ends");
    int key = 7;
    int *hit = (int *)bsearch(&key, a, 9, sizeof a[0], cmp_int);
    utest_check(hit && *hit == 7, "bsearch finds a present key");
    key = 8;
    utest_check(bsearch(&key, a, 9, sizeof a[0], cmp_int) == 0, "and reports an absent one");

    // --- setjmp / longjmp --------------------------------------------
    static volatile int arrivals = 0;
    unsigned long rsp_before = rsp_now();
    int r = setjmp(g_jb);
    if (r == 0) {
        arrivals++;          // the direct call
        jump_outer();
        utest_check(0, "longjmp returned to its caller (it must not)");
    } else {
        arrivals++;          // the jump
    }
    utest_check(r == 7, "longjmp's value comes back out of setjmp");
    utest_check(arrivals == 2, "and the block ran exactly twice, not once or forever");
    utest_check(rsp_now() == rsp_before, "and the STACK POINTER came back with it");

    // --- dirent ------------------------------------------------------
    DIR *d = opendir("/bin");
    utest_check(d != 0, "opendir(/bin)");
    int count = 0, saw_dir_flag = 0;
    struct dirent *e;
    while ((e = readdir(d)) != 0) {
        count++;
        if (e->d_type == DT_DIR) saw_dir_flag = 1;
        if (e->d_name[0] == '\0') { utest_check(0, "an entry had an empty name"); break; }
    }
    utest_check(count > 5, "readdir walked the whole directory");
    utest_check(saw_dir_flag, "and reported /bin/wm as a directory");
    rewinddir(d);
    int again = 0;
    while (readdir(d)) again++;
    utest_check(again == count, "rewinddir restarts the same snapshot");
    utest_check(closedir(d) == 0, "closedir");
    utest_check(opendir("/definitely/not/here") == 0, "opendir refuses a missing path");

    // --- unistd ------------------------------------------------------
    utest_check(isatty(1) == 1, "isatty(stdout) on the console");
    char cwd[64];
    utest_check(getcwd(cwd, sizeof cwd) != 0, "getcwd");

        // --- the 2026-09-13 libc review -----------------------------------
    //
    // Each of these was a REPORTED defect, and each is written so the
    // old behaviour fails it rather than so the new one passes.
    {
        // getline with a NULL buffer and a NON-ZERO *n. The old guard
        // tested only the capacity, so it skipped the allocation and
        // stored through the NULL. A crash is the failure mode, so
        // reaching the next line at all is most of the check.
        FILE *g = fopen("/tests/libc3_getline.tmp", "w");
        utest_check(g != 0, "getline fixture opens");
        if (g) { fputs("alpha\nbeta\n", g); fclose(g); }
        g = fopen("/tests/libc3_getline.tmp", "r");
        if (g) {
            char *line = 0;
            size_t cap = 128;          // a LIE: there is no buffer
            ssize_t n = getline(&line, &cap, g);
            utest_check(n == 6 && line && line[0] == 'a',
                        "getline allocates when *lineptr is NULL whatever *n says");
            free(line);
            fclose(g);
        }
        remove("/tests/libc3_getline.tmp");
    }
    {
        // O_RDWR is refused rather than aliased to O_WRONLY, which used
        // to let O_TRUNC destroy a file before the first read failed.
        errno = 0;
        int fd = open("/tests/libc3_rdwr.tmp", O_RDWR | O_CREAT | O_TRUNC, 0644);
        utest_check(fd < 0 && errno == EINVAL,
                    "O_RDWR is REFUSED, not quietly downgraded to write-only");
        if (fd >= 0) close(fd);
    }
    {
        // strtol's negation used to be undefined at LONG_MIN.
        errno = 0;
        long v = strtol("-9223372036854775808", &end, 10);
        utest_check(v == LONG_MIN && errno != ERANGE,
                    "LONG_MIN parses exactly, without signed overflow");
    }
    {
        // strtod: a zero mantissa with a huge exponent is 0, not NaN,
        // and a subnormal is not flushed to zero by an overflowing
        // intermediate.
        double z = strtod("0e999", 0);
        utest_check(z == 0.0, "0e999 is zero, not NaN");
        double sub = strtod("1e-310", 0);
        utest_check(sub > 0.0 && sub < 1e-300,
                    "a subnormal survives -- the exponent no longer overflows first");
        utest_check(strtod("0x1p2", &end) == 4.0 && *end == 0,
                    "hex floats parse");
    }
    {
        // printf: the formats the differential harness found wrong.
        char b[32];
        snprintf(b, sizeof b, "%hhu", 256);
        utest_check(strcmp(b, "0") == 0, "%hhu NARROWS the promoted value");
        snprintf(b, sizeof b, "%+5d", 1);
        utest_check(strcmp(b, "   +1") == 0, "a sign sits INSIDE the padded field");
        snprintf(b, sizeof b, "%#8x", 42);
        utest_check(strcmp(b, "    0x2a") == 0, "...and so does an 0x prefix");
        snprintf(b, sizeof b, "%.3o", 1);
        utest_check(strcmp(b, "001") == 0, "a precision applies to octal");
        snprintf(b, sizeof b, "%5c", 'a');
        utest_check(strcmp(b, "    a") == 0, "%c honours a width");
        snprintf(b, sizeof b, "%0+d", 42);
        utest_check(strcmp(b, "+42") == 0, "flags parse in ANY order");
    }
    {
        // qsort is no longer quadratic. Not a timing check -- those are
        // flaky -- but a size the old insertion sort would take minutes
        // over, sorted here in well under a second.
        static int big[4000];
        for (int i = 0; i < 4000; i++) big[i] = 4000 - i;   // worst case
        qsort(big, 4000, sizeof big[0], cmp_int);
        int sorted = 1;
        for (int i = 1; i < 4000; i++) if (big[i - 1] > big[i]) sorted = 0;
        utest_check(sorted, "qsort sorts 4000 reversed elements");
    }

        // --- the second review round --------------------------------------
    {
        // A number's DIGIT COUNT is not its magnitude. Both of these
        // used to overflow the mantissa before the exponent was applied.
        static char longnum[400];
        longnum[0] = '1';
        for (int i = 1; i <= 309; i++) longnum[i] = '0';
        strcpy(longnum + 310, "e-309");
        utest_check(strtod(longnum, 0) == 1.0,
                    "1 then 309 zeros then e-309 is ONE, not infinity");

        static char ones[400];
        ones[0] = '0'; ones[1] = '.';
        for (int i = 0; i < 310; i++) ones[2 + i] = '1';
        ones[312] = 0;
        double f = strtod(ones, 0);
        utest_check(f > 0.11 && f < 0.12, "0. then 310 ones is ~0.111, not NaN");

        // A leading zero is not a significant digit -- counting it as
        // one spent the budget before the value began.
        static char tiny[400];
        tiny[0] = '0'; tiny[1] = '.';
        for (int i = 0; i < 320; i++) tiny[2 + i] = '0';
        tiny[322] = '1'; tiny[323] = 0;
        utest_check(strtod(tiny, 0) > 0.0,
                    "a subnormal written with 320 leading zeros is not zero");
    }
    {
        // The spelled-out forms are one token, not three characters.
        utest_check(strtod("infinity", &end) > 1e308 && *end == 0,
                    "\"infinity\" is consumed whole");
        utest_check(strtod("nan(1234)", &end) != strtod("nan(1234)", &end) &&
                    *end == 0, "\"nan(payload)\" is consumed whole");
        errno = 0;
        double hx = strtod("0x1p1024", 0);
        utest_check(hx > 1e308 && errno == ERANGE,
                    "a hex float that overflows reports ERANGE");
    }
    {
        // '#' on octal outranks a precision of zero.
        char b[16];
        snprintf(b, sizeof b, "%#.0o", 0);
        utest_check(strcmp(b, "0") == 0, "%#.0o of zero is \"0\", not empty");
        snprintf(b, sizeof b, "%.0o", 0);
        utest_check(strcmp(b, "") == 0, "...while %.0o of zero is still empty");
    }

    return utest_end();
}
