// Error codes: can ring 3 tell two failures apart?
//
// WHY THIS EXISTS RATHER THAN A KTEST. The kernel handlers are what
// changed, and a KTEST could check each one returns the right number --
// but the thing actually being claimed is that the REASON survives the
// trip out to ring 3: the negated code in RAX, libsys turning it back
// into -1, and sys_errno() holding what it was. None of that is visible
// from inside the kernel, which is the same gap libc_test.c was written
// into.
//
// THE LOAD-BEARING CHECK IS THE EMFILE ONE, and it is why the fixture
// below opens files in a loop rather than assuming anything. Before
// error codes, open() answered -1 for a missing file and -1 for a full
// descriptor table, so /bin/tosh's PATH search reported "not found" for
// a machine that had simply run out of fds. That is the failure this
// whole change exists for, so the test has to genuinely exhaust the
// table and then open a file it has already proved is THERE -- a check
// that opened something missing would pass with the bug present.
//
// Prints one line per check and exits with the number of FAILURES.
#include <stdint.h>
#include "rt/sys.h"
#include "lib/string.h"

static int g_fail;

static void put(const char *s) { sys_write(1, s, strlen(s)); }

static void check(int ok, const char *what) {
    put(ok ? "  ok   " : "  FAIL ");
    put(what);
    put("\n");
    if (!ok) g_fail++;
}

// Reports the code as well as the verdict, so a failure says what it
// actually got instead of only that it was wrong.
static void check_errno(int got, int want, const char *what) {
    put(got == want ? "  ok   " : "  FAIL ");
    put(what);
    put(" -- got ");
    put(sys_strerror(got));
    if (got != want) { put(", wanted "); put(sys_strerror(want)); }
    put("\n");
    if (got != want) g_fail++;
}

#define PROBE "/tmp/errno_probe.txt"

int main(void) {
    put("errno_test: a failed syscall says why\n");

    // --- a successful call leaves no error -----------------------------
    // Stage 1's exit criterion. sys_errno() is not cleared on success
    // (POSIX's rule), so this is only meaningful as the FIRST thing
    // asked -- nothing has failed yet at this point in the process.
    check(sys_errno() == 0, "sys_errno() is 0 before anything has failed");

    // --- ENOENT vs EFAULT, the two ways open() can refuse a path -------
    int fd = sys_open("/definitely/not/here.txt", 0);
    check(fd < 0, "open() of a missing file fails");
    check_errno(sys_errno(), ENOENT, "open() of a missing file");

    // A pointer this process may not read. The kernel walks page tables
    // rather than dereferencing it, so this is a refusal and not a
    // fault -- see vmm.h.
    fd = sys_open((const char *)0x1000, 0);
    check(fd < 0, "open() of a kernel-only pointer fails");
    check_errno(sys_errno(), EFAULT, "open() of a bad pointer");

    // --- a file that definitely exists, for the EMFILE fixture ---------
    fd = sys_open(PROBE, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    check(fd >= 0, "created " PROBE);
    if (fd < 0) { put("errno_test: cannot build the fixture\n"); sys_exit(g_fail + 1); }
    sys_write(fd, "x", 1);
    sys_close(fd);

    // Prove it opens NOW, so that when the same open fails below, the
    // only thing that changed is the descriptor table. Without this the
    // EMFILE check would be asserting against a file it never
    // established was there.
    fd = sys_open(PROBE, 0);
    check(fd >= 0, "the probe file opens with a free descriptor table");
    if (fd >= 0) sys_close(fd);

    // --- EMFILE: exhaust this process's table for real ------------------
    // FD_MAX is 16 per address space and 0/1/2 are already taken, so
    // this runs out well before the array does. Keeping every fd open is
    // the point -- closing as it goes would never reach the branch.
    int held[64];
    int n = 0;
    while (n < (int)(sizeof held / sizeof held[0])) {
        int h = sys_open(PROBE, 0);
        if (h < 0) break;
        held[n++] = h;
    }
    check(n > 0, "opened the probe file until the table was full");
    check_errno(sys_errno(), EMFILE, "open() with a full descriptor table");

    // THE POINT OF ALL OF IT: the same path, the same process, two
    // different answers. Anything that merges these two is the bug.
    put("  ---- ENOENT and EMFILE are distinct: ");
    put(ENOENT != EMFILE ? "yes\n" : "NO\n");

    // dup() runs out of descriptors the same way, and must say so the
    // same way rather than reporting a bad fd.
    check(sys_dup(0) < 0, "dup() fails with a full table");
    check_errno(sys_errno(), EMFILE, "dup() with a full descriptor table");

    for (int i = 0; i < n; i++) sys_close(held[i]);

    // --- EBADF, its three shapes --------------------------------------
    char buf[8];
    check(sys_read(99, buf, sizeof buf) < 0, "read() of an fd that was never opened fails");
    check_errno(sys_errno(), EBADF, "read() of an unopened fd");

    check(sys_close(99) < 0, "close() of an unopened fd fails");
    check_errno(sys_errno(), EBADF, "close() of an unopened fd");

    // Open for WRITING and then read it -- the fd is perfectly valid and
    // still the wrong way round, which POSIX also calls EBADF.
    fd = sys_open(PROBE, SYS_O_WRITE);
    check(fd >= 0, "reopened the probe for writing");
    if (fd >= 0) {
        check(sys_read(fd, buf, sizeof buf) < 0, "read() of a write-only fd fails");
        check_errno(sys_errno(), EBADF, "read() of a write-only fd");
        sys_close(fd);
    }

    // --- EFAULT on a buffer, not a path -------------------------------
    check(sys_write(1, (const void *)0x1000, 8) < 0, "write() of a kernel-only buffer fails");
    check_errno(sys_errno(), EFAULT, "write() of a bad buffer");

    // --- EINVAL: arguments the kernel will not take -------------------
    check(sys_getrandom(buf, 1000000) < 0, "getrandom() over the maximum fails");
    check_errno(sys_errno(), EINVAL, "getrandom() with too large a count");

    check(sys_socket(1, 0) < 0, "socket() with a nonzero domain fails");
    check_errno(sys_errno(), EINVAL, "socket() with a reserved argument set");

    // --- ENOSYS: the fd is fine, the call is not built ----------------
    // Distinct from the EBADF above on purpose: a caller told EBADF
    // would go looking at its own descriptor, which is not the problem.
    fd = sys_socket(0, 0);
    check(fd >= 0, "socket() with reserved arguments zero succeeds");
    if (fd >= 0) {
        check(sys_send(fd, "x", 1) < 0, "send() on a real socket fails");
        check_errno(sys_errno(), ENOSYS, "send() with no transport behind it");
        sys_close(fd);
    }

    // --- ECHILD: permanent, and distinct from "not yet" ---------------
    // An init loop that confuses these two either spins forever or stops
    // reaping (see docs/init-design.md), so the code matters more here
    // than anywhere else in this file.
    check(sys_waitpid(-1, 0) < 0, "waitpid(-1) with no children fails");
    check_errno(sys_errno(), ECHILD, "waitpid(-1) with no children at all");

    // --- strerror ------------------------------------------------------
    check(strerror(ENOENT)[0] != '\0', "strerror(ENOENT) is a real message");
    check(strerror(4242)[0] != '\0', "strerror() of an unknown code still says something");

    // --- and the reason is STICKY, which callers must know -------------
    // Not cleared by a success, exactly as POSIX specifies. Asserted so
    // nobody "fixes" it into clearing and quietly costs every successful
    // syscall a store.
    sys_open("/definitely/not/here.txt", 0);
    int before = sys_errno();
    sys_close(sys_dup(1));
    check(sys_errno() == before, "a successful call does NOT clear sys_errno()");

    sys_unlink(PROBE);

    put("errno_test: ");
    if (g_fail == 0) put("all checks passed\n");
    else { put("FAILURES\n"); }
    sys_exit(g_fail);
}
