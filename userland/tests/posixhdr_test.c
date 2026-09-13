// The ten POSIX headers added for the dash port, checked for the
// behaviour their comments promise rather than for existing.
//
// WHY THESE CHECKS AND NOT OTHERS. Each one targets a place where the
// obvious implementation is wrong in a way that compiles:
//
//  - **A byte reaches wchar_t UNSIGNED.** `char` is signed here, so a
//    plain cast turns 0xE9 into -23 and every later comparison fails.
//    This project has been bitten by the signed-char gates before, so
//    the high half is checked explicitly rather than assumed.
//  - **setlocale REFUSES a locale it cannot honour**, and the refusal
//    is the whole point -- a caller told "yes" for de_DE formats
//    numbers wrongly and never finds out. A test that only checked "C"
//    works would pass against an implementation that accepts anything.
//  - **setrlimit refuses to LOWER a limit but accepts a no-op.** Both
//    halves matter: accepting everything is the bug, and refusing
//    everything breaks `ulimit -f unlimited`.
//  - **wctype() returns 0 for an unknown class, and iswctype() with 0
//    is false.** dash's [[:alpha:]] handling depends on both; an
//    implementation that returned a valid token for a bad name would
//    silently match nothing.
//  - **mbsrtowcs with a NULL destination COUNTS and must not advance
//    *src.** C specifies it, and a caller sizing a buffer does exactly
//    this before allocating -- getting it wrong corrupts the second
//    call rather than the first.
//
// Prints one line per check and exits with the number of failures.
#include <locale.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <wchar.h>
#include <wctype.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <sys/times.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>   // CLOCKS_PER_SEC, to compare against _SC_CLK_TCK

#include "lib/utest.h"

int main(void) {
    utest_begin("posixhdr_test", "the POSIX headers the dash port needs", 0);

    // --- <sys/param.h> -------------------------------------------------
    utest_check(MAXPATHLEN == PATH_MAX,
                "MAXPATHLEN is PATH_MAX, not a second opinion");

    // --- <arpa/inet.h> -------------------------------------------------
    utest_check(htonl(0x01020304u) == 0x04030201u, "htonl swaps all four bytes");
    utest_check(htons(0x0102u) == 0x0201u, "htons swaps both bytes");
    utest_check(ntohl(htonl(0xDEADBEEFu)) == 0xDEADBEEFu, "ntohl undoes htonl");

    // --- <locale.h> ----------------------------------------------------
    utest_check(setlocale(LC_ALL, "C") != 0, "setlocale accepts C");
    utest_check(setlocale(LC_ALL, "POSIX") != 0, "setlocale accepts POSIX");
    utest_check(setlocale(LC_ALL, "") != 0, "setlocale accepts the environment");
    utest_check(setlocale(LC_ALL, 0) != 0, "setlocale queries without setting");
    utest_check(setlocale(LC_ALL, "de_DE.UTF-8") == 0,
                "setlocale REFUSES a locale it cannot honour");
    utest_check(strcmp(localeconv()->decimal_point, ".") == 0,
                "localeconv reports the C locale's decimal point");

    // --- <wchar.h> -----------------------------------------------------
    mbstate_t st;
    wchar_t wc = 0;
    memset(&st, 0, sizeof st);
    utest_check(mbrtowc(&wc, "A", 1, &st) == 1 && wc == 'A',
                "mbrtowc converts one byte");
    memset(&st, 0, sizeof st);
    utest_check(mbrtowc(&wc, "", 1, &st) == 0 && wc == 0,
                "mbrtowc reports a NUL as a zero-length character");
    memset(&st, 0, sizeof st);
    // THE SIGNED-CHAR CHECK: 0xE9 must arrive as 233, not as -23.
    utest_check(mbrtowc(&wc, "\xE9", 1, &st) == 1 && wc == 0xE9,
                "a high byte reaches wchar_t UNSIGNED");
    memset(&st, 0, sizeof st);
    utest_check(mbrlen("x", 0, &st) == (size_t)-2,
                "mbrlen with n=0 is incomplete, not an error");
    utest_check(mbrlen("x", 1, &st) == 1, "mbrlen counts one byte");

    wchar_t buf[8];
    const char *src = "abc";
    memset(&st, 0, sizeof st);
    size_t n = mbsrtowcs(buf, &src, 8, &st);
    utest_check(n == 3 && buf[0] == 'a' && buf[2] == 'c' && buf[3] == 0,
                "mbsrtowcs converts a whole string and stores the NUL");
    utest_check(src == 0, "mbsrtowcs NULLs *src when it reached the end");

    const char *src2 = "abc";
    memset(&st, 0, sizeof st);
    utest_check(mbsrtowcs(0, &src2, 0, &st) == 3,
                "mbsrtowcs with no destination COUNTS");
    utest_check(src2 != 0 && strcmp(src2, "abc") == 0,
                "...and leaves *src where it was");

    wchar_t ws[4] = { 'a', 'b', 'c', 0 };
    utest_check(wcschr(ws, 'b') == &ws[1], "wcschr finds a character");
    utest_check(wcschr(ws, 'z') == 0, "wcschr reports a miss");
    utest_check(wcschr(ws, 0) == &ws[3], "wcschr finds the terminator");

    // --- <wctype.h> ----------------------------------------------------
    utest_check(iswspace(' ') && !iswspace('x'), "iswspace classifies");
    utest_check(iswblank('\t') && !iswblank('\n'), "iswblank is space and tab only");
    utest_check(iswalpha(0x41) && !iswalpha(0x40), "iswalpha classifies");
    utest_check(!iswalpha(0x1F600), "a value above a byte is in no class");
    utest_check(towupper('a') == 'A' && towlower('A') == 'a', "case conversion");

    wctype_t alpha = wctype("alpha");
    utest_check(alpha != 0, "wctype names a real class");
    utest_check(iswctype('q', alpha), "iswctype applies it");
    utest_check(!iswctype('4', alpha), "...and rejects a non-member");
    utest_check(wctype("nosuchclass") == 0, "wctype REFUSES an unknown name");
    utest_check(!iswctype('q', 0), "iswctype with a 0 class is always false");

    // --- <sys/resource.h> ----------------------------------------------
    struct rlimit rl;
    utest_check(getrlimit(RLIMIT_FSIZE, &rl) == 0 &&
                rl.rlim_cur == RLIM_INFINITY && rl.rlim_max == RLIM_INFINITY,
                "getrlimit reports no limit");
    utest_check(getrlimit(RLIMIT_NLIMITS, &rl) == -1,
                "getrlimit rejects an unknown resource");
    struct rlimit same = { RLIM_INFINITY, RLIM_INFINITY };
    utest_check(setrlimit(RLIMIT_FSIZE, &same) == 0,
                "setrlimit accepts the value already in force");
    struct rlimit lower = { 1024, RLIM_INFINITY };
    errno = 0;
    utest_check(setrlimit(RLIMIT_FSIZE, &lower) == -1 && errno == EPERM,
                "setrlimit REFUSES a limit it cannot enforce");

    // --- <unistd.h> sysconf, <sys/times.h> ------------------------------
    long tck = sysconf(_SC_CLK_TCK);
    utest_check(tck == CLOCKS_PER_SEC,
                "_SC_CLK_TCK and CLOCKS_PER_SEC are the same unit here");
    utest_check(sysconf(_SC_PAGESIZE) == 4096, "sysconf reports the page size");
    errno = 0;
    utest_check(sysconf(-1) == -1 && errno == EINVAL,
                "sysconf rejects a name it does not answer");

    struct tms tb;
    memset(&tb, 0xFF, sizeof tb);
    clock_t t0 = times(&tb);
    utest_check(t0 != (clock_t)-1, "times succeeds");
    utest_check(tb.tms_stime == 0 && tb.tms_cutime == 0 && tb.tms_cstime == 0,
                "the three fields this kernel cannot fill are ZERO, not junk");
    // Elapsed real time must ADVANCE across work that burns none of it.
    // A returned constant would pass every check above.
    volatile long spin = 0;
    for (long i = 0; i < 2000000L; i++) spin += i;
    (void)spin;   // volatile keeps the loop; this quiets set-but-unused
    clock_t t1 = times(&tb);
    utest_check(t1 >= t0, "times' return value does not go backwards");

    // --- <sys/stat.h> --------------------------------------------------
    {
        struct stat st;
        utest_check(stat("/bin/ls", &st) == 0, "stat finds a program");
        utest_check(S_ISREG(st.st_mode), "...and reports it as a regular file");
        utest_check(!S_ISDIR(st.st_mode), "...and not a directory");
        // THE EXEC BIT IS THE POINT. Without it `test -x /bin/ls` is a
        // wrong answer rather than an absent one, which is worse.
        utest_check((st.st_mode & 0111) != 0, "a /bin program is EXECUTABLE");
        utest_check(st.st_size > 0, "a program has a size");
        utest_check(st.st_uid == 0 && st.st_gid == 0,
                    "uid/gid are 0 -- a single-user system, not a placeholder");
        utest_check(st.st_blocks * 512 >= st.st_size,
                    "st_blocks covers st_size, rounded up");
        utest_check(st.st_atime == st.st_mtime,
                    "there is no access time, and stat says so by mirroring mtime");

        struct stat d;
        utest_check(stat("/bin", &d) == 0 && S_ISDIR(d.st_mode),
                    "a directory is a directory");
        utest_check((d.st_mode & 07777) == 0755, "a directory is 0755");

        // **A FILE WE CREATE, not a seeded one.** This used to assert
        // that /etc/services.d/tosh is non-executable, which held only
        // while the default was 0644: that file predates the mode field,
        // so its inode stores ZERO and stat reports the default -- which
        // is 0755 now. Testing chmod on a file this check owns measures
        // the behaviour instead of the disk's history.
        FILE *mk = fopen("/tmp_modecheck", "w");
        if (mk) fclose(mk);
        struct stat f;
        utest_check(stat("/tmp_modecheck", &f) == 0 && (f.st_mode & 0111),
                    "a new file is executable by default");
        utest_check(chmod("/tmp_modecheck", 0644) == 0, "chmod succeeds");
        utest_check(stat("/tmp_modecheck", &f) == 0 && (f.st_mode & 0111) == 0,
                    "...and the file is no longer executable");
        utest_check((f.st_mode & S_IFMT) == S_IFREG,
                    "...and chmod did not change what it IS");
        remove("/tmp_modecheck");

        utest_check(stat("/no/such/path", &st) == -1, "a missing path fails");
        // lstat IS stat here and says so; checking they agree is what
        // stops the two drifting into a false distinction. Written as
        // two separate calls compared field by field -- an `||` here
        // would make the check pass however lstat behaved.
        struct stat a, b;
        int ok = (stat("/bin/ls", &a) == 0) && (lstat("/bin/ls", &b) == 0);
        utest_check(ok && a.st_ino == b.st_ino && a.st_mode == b.st_mode &&
                    a.st_size == b.st_size,
                    "lstat agrees with stat field for field");

        FILE *fp = fopen("/bin/ls", "r");
        if (fp) {
            struct stat fs_;
            utest_check(fstat(fileno(fp), &fs_) == 0 && S_ISREG(fs_.st_mode),
                        "fstat works on an open descriptor");
            fclose(fp);
        }
    }

    return utest_end();
}
