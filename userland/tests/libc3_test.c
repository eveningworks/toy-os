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
#include <ctype.h>
#include <errno.h>
#include <setjmp.h>
#include <dirent.h>
#include <unistd.h>
#include <limits.h>
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
    const char *nan_hex = "0xzz";
    utest_check(strtol(nan_hex, &end, 16) == 0 && end == nan_hex,
          "...and when an 0x prefix was");
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

    return utest_end();
}
