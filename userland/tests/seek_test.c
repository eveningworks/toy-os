// lseek, fstat, and O_APPEND -- the three syscalls a buffered stdio
// cannot be built without (docs/libc-design.md, Stage 0).
//
// WHY A /tests ELF RATHER THAN A KTEST. Every property here is about
// what a PROCESS sees: its own fd's position, and what fstat reports
// about descriptors only a process has (its console, its pipe). A KTEST
// runs in the kernel context, which has no descriptor table at all --
// the same reasoning fd_test.c's header gives.
//
// THE ASSERTION THAT MATTERS is the round trip, not the return value:
// every seek is followed by a read of what was written there, and the
// pattern is POSITION-DERIVED ('A' + i % 26) so a seek landing at the
// wrong offset reads back the WRONG letter rather than a plausible one.
// A file of identical bytes cannot tell a working seek from a dead one.
//
// IT IS SPAWNED, NOT `run`. SYS_PIPE needs a real scheduler slot, and
// the legacy `run` loader has none -- so under `run` the pipe half of
// the fstat checks cannot be reached at all, which is the one place an
// unseekable NON-terminal is tested. A spawned program's console output
// arrives while the harness is between commands, where it is dropped,
// so the verdict also goes to a FILE the harness can ask for whenever
// it likes (wait on the artifact, not on the timing -- the same shape
// wrap_test.c uses and for the same reason).
//
// Prints one line per check and exits with the number of failures.
#include <stdint.h>
#include "rt/sys.h"
#include <string.h>
#include <stdio.h>
#include "syscall_abi.h"

#define PATH "/tmp/seek_test.bin"
#define VERDICT_PATH "/tmp/seek_test.out"
#define N    512

static int g_fail;

// The whole transcript, not just the verdict. A spawned test's console
// output is dropped (see the header), so a failure with only a count in
// the file says WHICH of forty-odd checks broke exactly as well as
// "something did" -- which is not at all. Put the detail in the line a
// truncated log keeps.
static char g_log[4096];
static unsigned g_log_n;

static void put(const char *s) {
    unsigned n = (unsigned)strlen(s);
    sys_write(1, s, n);
    if (g_log_n + n < sizeof g_log) {
        k_memcpy(g_log + g_log_n, s, n);
        g_log_n += n;
    }
}

static void check(int ok, const char *what) {
    put(ok ? "  ok   " : "  FAIL ");
    put(what);
    put("\n");
    if (!ok) g_fail++;
}

static char pat(int i) { return (char)('A' + (i % 26)); }

int main(void) {
    put("seek_test: lseek, fstat and O_APPEND\n");

    // --- lay down a known, position-derived file ---------------------
    sys_unlink(PATH);
    int fd = sys_open(PATH, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    if (fd < 0) { put("  FAIL cannot create the fixture\n"); return 1; }
    char buf[N];
    for (int i = 0; i < N; i++) buf[i] = pat(i);
    check(sys_write(fd, buf, N) == N, "wrote the fixture");
    // A sequential write must leave the position at the END, which is
    // the property that used to be true by accident (writes appended
    // and ignored the position entirely).
    check(sys_lseek(fd, 0, SYS_SEEK_CUR) == N, "sequential writes advance the position");
    sys_close(fd);

    // --- seeking on a read fd ----------------------------------------
    fd = sys_open(PATH, 0);
    check(fd >= 0, "reopened for reading");

    char c = 0;
    check(sys_lseek(fd, 100, SYS_SEEK_SET) == 100, "SEEK_SET lands where asked");
    check(sys_read(fd, &c, 1) == 1 && c == pat(100), "and reads byte 100");
    // The read above advanced it by one, so CUR is relative to 101.
    check(sys_lseek(fd, 9, SYS_SEEK_CUR) == 110, "SEEK_CUR is relative");
    check(sys_read(fd, &c, 1) == 1 && c == pat(110), "and reads byte 110");
    check(sys_lseek(fd, -2, SYS_SEEK_END) == N - 2, "SEEK_END with a negative offset");
    check(sys_read(fd, &c, 1) == 1 && c == pat(N - 2), "and reads the second-to-last byte");
    check(sys_lseek(fd, 0, SYS_SEEK_SET) == 0, "rewind");
    check(sys_read(fd, &c, 1) == 1 && c == pat(0), "and reads byte 0 again");

    // Past the end is LEGAL and reads as EOF -- not an error.
    check(sys_lseek(fd, N + 1000, SYS_SEEK_SET) == N + 1000, "seeking past EOF is allowed");
    check(sys_read(fd, &c, 1) == 0, "and reads 0 there");

    // Before byte zero is the one refusal.
    check(sys_lseek(fd, -1, SYS_SEEK_SET) == -1 && sys_errno() == EINVAL,
          "seeking before byte 0 is EINVAL");
    check(sys_lseek(fd, 0, 99) == -1 && sys_errno() == EINVAL,
          "an unknown whence is EINVAL");
    sys_close(fd);
    check(sys_lseek(fd, 0, SYS_SEEK_SET) == -1 && sys_errno() == EBADF,
          "a closed fd is EBADF");

    // --- a write AT a position overwrites in place -------------------
    fd = sys_open(PATH, SYS_O_WRITE);
    check(fd >= 0, "reopened for writing, no truncate");
    check(sys_lseek(fd, 64, SYS_SEEK_SET) == 64, "seeked to 64");
    check(sys_write(fd, "ZZZZ", 4) == 4, "wrote four bytes there");
    sys_close(fd);

    fd = sys_open(PATH, 0);
    char win[8];
    sys_lseek(fd, 62, SYS_SEEK_SET);
    sys_read(fd, win, 8);
    // The neighbours are half the assertion: an overwrite that also
    // moved or truncated everything after it would still put ZZZZ at 64.
    check(win[0] == pat(62) && win[1] == pat(63), "the bytes before are untouched");
    check(win[2] == 'Z' && win[3] == 'Z' && win[4] == 'Z' && win[5] == 'Z',
          "the four bytes landed at 64");
    check(win[6] == pat(68) && win[7] == pat(69), "the bytes after are untouched");
    sys_close(fd);

    struct sys_stat st;
    check(sys_stat(PATH, &st) == 0 && st.size == N,
          "an in-place write did not change the size");

    // --- O_APPEND ignores the position -------------------------------
    fd = sys_open(PATH, SYS_O_WRITE | SYS_O_APPEND);
    check(fd >= 0, "reopened with O_APPEND");
    check(sys_lseek(fd, 0, SYS_SEEK_SET) == 0, "seeked to the start anyway");
    check(sys_write(fd, "tail", 4) == 4, "wrote four bytes");
    sys_close(fd);

    check(sys_stat(PATH, &st) == 0 && st.size == N + 4, "which went to the END");
    fd = sys_open(PATH, 0);
    sys_lseek(fd, N, SYS_SEEK_SET);
    sys_read(fd, win, 4);
    check(win[0] == 't' && win[1] == 'a' && win[2] == 'i' && win[3] == 'l',
          "and the tail is there");
    // The positive control for the line above: without O_APPEND this
    // same sequence overwrites byte 0, so byte 0 still being 'A' is
    // what proves the flag did something.
    sys_lseek(fd, 0, SYS_SEEK_SET);
    sys_read(fd, &c, 1);
    check(c == pat(0), "and byte 0 was NOT overwritten");

    // --- fstat -------------------------------------------------------
    check(sys_fstat(fd, &st) == 0, "fstat on a file fd");
    check(st.size == N + 4, "reports the size");
    check((st.flags & SYS_STAT_SEEKABLE) != 0, "and says it is seekable");
    check((st.flags & SYS_STAT_TTY) == 0, "and that it is not a terminal");
    sys_close(fd);

    check(sys_fstat(1, &st) == 0, "fstat on stdout");
    check((st.flags & SYS_STAT_TTY) != 0, "says it IS a terminal");
    check((st.flags & SYS_STAT_SEEKABLE) == 0, "and is not seekable");
    check(sys_lseek(1, 0, SYS_SEEK_CUR) == -1 && sys_errno() == ESPIPE,
          "so seeking it is ESPIPE");

    int p[2];
    if (sys_pipe(p) == 0) {
        check(sys_fstat(p[0], &st) == 0, "fstat on a pipe");
        check((st.flags & (SYS_STAT_TTY | SYS_STAT_SEEKABLE)) == 0,
              "which is neither a terminal nor seekable");
        check(st.size == 0, "and has no size to report");
        check(sys_lseek(p[0], 0, SYS_SEEK_CUR) == -1 && sys_errno() == ESPIPE,
              "seeking a pipe is ESPIPE");
        sys_close(p[0]);
        sys_close(p[1]);
    } else {
        check(0, "could not make a pipe");
    }

    check(sys_fstat(999, &st) == -1 && sys_errno() == EBADF, "fstat on a bad fd is EBADF");

    sys_unlink(PATH);

    char verdict[64];
    if (g_fail) snprintf(verdict, sizeof verdict, "seek_test: %d FAILURES\n", g_fail);
    else        snprintf(verdict, sizeof verdict, "seek_test: all checks passed\n");
    put(verdict);
    // Best effort: if /tmp is unwritable the printed line above is still
    // there for a human.
    int vfd = sys_open(VERDICT_PATH, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    if (vfd >= 0) {
        // Chunked: SYS_WRITE_MAX is 1024 bytes per call, and a short
        // write here would silently lose the tail of the transcript.
        for (unsigned off = 0; off < g_log_n; ) {
            unsigned n = g_log_n - off;
            if (n > SYS_WRITE_MAX) n = SYS_WRITE_MAX;
            int64_t w = sys_write(vfd, g_log + off, n);
            if (w <= 0) break;
            off += (unsigned)w;
        }
        sys_close(vfd);
    }
    return g_fail;
}
