// fork() and exec(): the compatibility pair beside spawn
// (docs/fork-design.md), proved from ring 3.
//
// What a broken version would still pass, and the check that stops it:
// a fork that COPIED nothing would still return twice, so the child
// writes a global, a heap page and a stack local and reports the values
// back through a pipe -- the parent must see its own untouched and the
// child's changed; a fork that copied only fds 0-2 would still run a
// pipeline, so fd 3 is written by the child under the number the parent
// opened; an exec that tore down first would still run the program, so
// a missing path must come back as -1 to a process that then EXITS with
// a code the parent reads.
//
// SPAWNED by tools/usertest_run.py: a fork needs a scheduler slot, which
// the legacy `run` loader has not got (sys_fork -> EPERM there).
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "rt/sys.h"
#include "syscall_abi.h"
#include "lib/utest.h"

static int g_global = 1;

static int read_all(int fd, char *buf, int cap) {
    int total = 0;
    for (;;) {
        ssize_t n = read(fd, buf + total, (size_t)(cap - 1 - total));
        if (n <= 0) break;
        total += (int)n;
        if (total >= cap - 1) break;
    }
    buf[total] = '\0';
    return total;
}

static int wait_code(int pid) {
    int code = -1;
    if (sys_waitpid(pid, &code) != pid) return -1000;
    return code;
}

// N fork/exit cycles, each child writing a global and a heap page so
// the copy-on-write path is exercised in both directions.
static void fork_cycles(int n) {
    for (int i = 0; i < n; i++) {
        int pid = fork();
        if (pid == 0) { g_global++; _exit(0); }
        wait_code(pid);
    }
}

static uint64_t free_frames(void) {
    struct query_meminfo m;
    if (sys_query_record(QUERY_MEMINFO, 0, &m, sizeof m) < (int)sizeof m) return 0;
    return m.frame_free;
}

int main(int argc, char **argv) {
    // `--forks N`: N silent fork/exit cycles and exit -- what
    // tools/frame_balance.py spawns from a quiet system, where every
    // frame this process took must come back at its exit.
    if (argc == 3 && strcmp(argv[1], "--forks") == 0) {
        fork_cycles(atoi(argv[2]));
        return 0;
    }
    utest_begin("fork_test", "fork() and exec()", UTEST_VERDICT_FILE);

    // --- 1. it returns twice, and the child's exit reaches the parent
    int pid = fork();
    if (pid == 0) _exit(7);
    utest_checkf(pid > 0, "fork returns a pid (%d)", pid);
    utest_checkf(wait_code(pid) == 7, "waitpid sees the child's exit code");

    // --- 2. copy-on-write: three kinds of memory, both directions
    int *heap = malloc(4096 * 3);
    heap[0] = 10; heap[1024] = 20; heap[2048] = 30;
    volatile int local = 100;
    int fds[2];
    utest_check(pipe(fds) == 0, "pipe");
    pid = fork();
    if (pid == 0) {
        g_global = 2; heap[0] = 11; heap[1024] = 21; heap[2048] = 31; local = 200;
        // A fresh heap page in the child, past the parent's touch.
        int *more = malloc(64 * 1024);
        more[8000] = 55;
        char line[128];
        int n = snprintf(line, sizeof line, "%d %d %d %d %d %d", g_global, heap[0],
                         heap[1024], heap[2048], (int)local, more[8000]);
        write(fds[1], line, (size_t)n);
        _exit(0);
    }
    close(fds[1]);
    char buf[256];
    read_all(fds[0], buf, sizeof buf);
    close(fds[0]);
    utest_checkf(wait_code(pid) == 0, "the COW child exited 0");
    utest_checkf(strcmp(buf, "2 11 21 31 200 55") == 0,
                 "the child saw its own writes: \"%s\"", buf);
    utest_checkf(g_global == 1 && heap[0] == 10 && heap[1024] == 20 &&
                 heap[2048] == 30 && local == 100,
                 "the parent's global, heap and stack are untouched");

    // --- 3. the kernel writing INTO a shared page (read() into a COW
    // buffer) lands privately. The buffer is a page the parent does NOT
    // touch between the fork and the child's read: a stack buffer would
    // share its page with the parent's own frames, and the parent's
    // first call after fork would un-share it before the kernel's copy
    // path was ever asked to -- which is how a version of this check
    // stayed green with that path disabled.
    char *raw = malloc(3 * 4096);
    char *pg = (char *)(((uintptr_t)raw + 4095) & ~(uintptr_t)4095);
    memset(pg, 'P', 16); pg[16] = 0;
    utest_check(pipe(fds) == 0, "pipe (2)");
    pid = fork();
    if (pid == 0) {
        close(fds[1]);
        read(fds[0], pg, 8);                  // the kernel writes into pg
        _exit(pg[0] == 'C' ? 0 : 1);
    }
    close(fds[0]);
    write(fds[1], "CCCCCCCC", 8);
    close(fds[1]);
    utest_checkf(wait_code(pid) == 0, "the child's read() landed in ITS buffer");
    utest_checkf(strcmp(pg, "PPPPPPPPPPPPPPPP") == 0,
                 "...and not in the parent's: \"%s\"", pg);

    // --- 4. the whole fd table crosses: fd 3+ under the same number
    char path[64];
    int have_tmp = tmppath(path, sizeof path, TMP_VOLATILE, "fork_test.txt") != 0;
    utest_check(have_tmp, "a scratch path");
    if (have_tmp) {
        int f = open(path, O_WRONLY | O_CREAT | O_TRUNC);
        utest_checkf(f >= 3, "opened fd %d", f);
        pid = fork();
        if (pid == 0) { write(f, "child", 5); _exit(0); }
        utest_checkf(wait_code(pid) == 0, "the fd child exited 0");
        close(f);
        f = open(path, O_RDONLY);
        int n = f >= 0 ? read_all(f, buf, sizeof buf) : -1;
        if (f >= 0) close(f);
        utest_checkf(n == 5 && strcmp(buf, "child") == 0,
                     "the child wrote through an inherited fd: \"%s\"", buf);
        unlink(path);
    }

    // --- 5. a grandchild: fork inside the fork
    pid = fork();
    if (pid == 0) {
        int gc = fork();
        if (gc == 0) _exit(3);
        _exit(wait_code(gc) == 3 ? 2 : 90);
    }
    utest_checkf(wait_code(pid) == 2, "a child can fork a grandchild and wait for it");

    // --- 6. fork; dup2; exec -- the shape a shell writes
    utest_check(pipe(fds) == 0, "pipe (3)");
    pid = fork();
    if (pid == 0) {
        dup2(fds[1], 1);
        close(fds[0]); close(fds[1]);
        char *argv[] = { "echo", "forked", "then", "exec'd", 0 };
        execv("/bin/echo", argv);
        _exit(99);                            // only if the exec failed
    }
    close(fds[1]);
    read_all(fds[0], buf, sizeof buf);
    close(fds[0]);
    utest_checkf(wait_code(pid) == 0, "the exec'd echo exited 0");
    utest_checkf(strcmp(buf, "forked then exec'd\n") == 0,
                 "its argv arrived through the exec: \"%s\"", buf);

    // --- 7. execve carries an environment of its own
    utest_check(pipe(fds) == 0, "pipe (4)");
    pid = fork();
    if (pid == 0) {
        dup2(fds[1], 1);
        close(fds[0]); close(fds[1]);
        char *argv[] = { "env_child", "FORKTEST", 0 };
        char *envp[] = { "FORKTEST=yes", "PATH=/nowhere", 0 };
        execve("/tests/env_child", argv, envp);
        _exit(99);
    }
    close(fds[1]);
    read_all(fds[0], buf, sizeof buf);
    close(fds[0]);
    utest_checkf(wait_code(pid) == 0, "the execve'd child exited 0");
    utest_checkf(strncmp(buf, "yes|kept|/nowhere", 17) == 0,
                 "it saw the environment execve gave it: \"%s\"", buf);

    // --- 8. a failed exec leaves the process intact
    pid = fork();
    if (pid == 0) {
        char *argv[] = { "nope", 0 };
        int r = execv("/no/such/program", argv);
        _exit(r == -1 && errno == ENOENT ? 33 : 44);
    }
    utest_checkf(wait_code(pid) == 33, "exec of a missing path returns -1/ENOENT to a live process");

    // --- 9. execvp walks PATH
    utest_check(pipe(fds) == 0, "pipe (5)");
    pid = fork();
    if (pid == 0) {
        dup2(fds[1], 1);
        close(fds[0]); close(fds[1]);
        setenv("PATH", "/nowhere:/bin", 1);
        char *argv[] = { "echo", "via", "path", 0 };
        execvp("echo", argv);
        _exit(99);
    }
    close(fds[1]);
    read_all(fds[0], buf, sizeof buf);
    close(fds[0]);
    utest_checkf(wait_code(pid) == 0 && strcmp(buf, "via path\n") == 0,
                 "execvp found echo on PATH: \"%s\"", buf);

    // --- 10. frames come back. Measured in WINDOWS after a warm-up,
    // because this test runs a second after boot while services are
    // still demand-paging their heaps and the /lib image cache is
    // filling -- a single before/after read lost ~1800 frames to that
    // settling and none to fork. A steady leak drifts every window; the
    // quiet, whole-process measurement is tools/frame_balance.py.
    for (int i = 0; i < 8; i++) fork_cycles(1);
    long best = 1L << 30;
    for (int w = 0; w < 3; w++) {
        uint64_t before = free_frames();
        fork_cycles(20);
        long delta = (long)free_frames() - (long)before;
        if (delta < 0) delta = -delta;
        if (delta < best) best = delta;
    }
    utest_checkf(best <= 4, "frames return over a window of 20 fork/exit cycles (best drift %ld)",
                 best);

    return utest_end();
}
