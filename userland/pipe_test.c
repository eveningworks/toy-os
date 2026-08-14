// Proves the spawn/pipe/wait chain end to end, from ring 3.
//
// One process creates a pipe, spawns ANOTHER program with its stdout
// redirected into it, reads that output back, and waits for the exit
// code. Before this existed no process here could see another's output
// at all -- which is the single thing a terminal has to be able to do.
//
// Exits 0 only if every link worked; each failure gets its own non-zero
// code so a failing run says which one broke rather than just "no".
// kernel/proc/pipe_test.c spawns this and asserts on that code.
#include <stdint.h>
#include "sys.h"

// hello prints a known line and exits 0 -- a stable thing to capture.
#define CHILD "/bin/hello"
#define WANT  "Hello from a real ELF64 binary in ring 3!"

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

// Substring search: no libc here, and this is the actual assertion --
// that the bytes the child printed arrived intact.
static int contains(const char *hay, int hay_len, const char *needle) {
    int nl = slen(needle);
    if (nl == 0) return 1;
    for (int i = 0; i + nl <= hay_len; i++) {
        int j = 0;
        while (j < nl && hay[i + j] == needle[j]) j++;
        if (j == nl) return 1;
    }
    return 0;
}

int main(void) {
    int fds[2];
    if (sys_pipe(fds) != 1) return 1;

    int pid = sys_spawn(CHILD, 0, fds[1]);
    if (pid < 0) return 2;

    // Close OUR copy of the write end. The child holds its own, so this
    // does not signal EOF -- but keeping it open would mean the reader
    // below never sees EOF even after the child exits, because a live
    // writer (us) would still exist. Forgetting this is the classic
    // pipe deadlock.
    sys_close(fds[1]);

    char buf[512];
    int total = 0;
    for (;;) {
        int64_t n = sys_read(fds[0], buf + total, (size_t)(sizeof buf - 1 - total));
        if (n <= 0) break; // 0 = EOF; the read BLOCKS rather than spinning
        total += (int)n;
        if (total >= (int)sizeof buf - 1) break;
    }
    buf[total] = '\0';
    sys_close(fds[0]);

    int code = -1;
    if (sys_waitpid(pid, &code) != pid) return 3;
    if (code != 0) return 4;
    if (total == 0) return 5;
    if (!contains(buf, total, WANT)) return 6;

    return 0;
}
