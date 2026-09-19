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
#include "net_abi.h"
#include <string.h>
#include "tmppath.h"
#include "lib/utmppath.h"

#include "lib/utest.h"

// Reports the code as well as the verdict, so a failure says what it
// actually got instead of only that it was wrong.
static void check_errno(int got, int want, const char *what) {
    utest_checkf(got == want, "%s -- got %s%s%s", what, sys_strerror(got),
                 got != want ? ", wanted " : "", got != want ? sys_strerror(want) : "");
}

static const char *p_probe(void) {
    static char p[64];
    if (!p[0]) tmppath(p, sizeof p, TMP_VOLATILE, "errno_probe.txt");
    return p;
}
#define PROBE p_probe()
// PATH "/sub" cannot be spelled as a concatenation once the prefix is
// built at runtime; this is that, done at the one place that needs it.
#define PROBE_UNDER(rel) utest_path(TMP_VOLATILE, "errno_probe.txt" rel)

int main(void) {
    utest_begin("errno_test", "a failed syscall says why", 0);

    // --- a successful call leaves no error -----------------------------
    // Stage 1's exit criterion. sys_errno() is not cleared on success
    // (POSIX's rule), so this is only meaningful as the FIRST thing
    // asked -- nothing has failed yet at this point in the process.
    utest_check(sys_errno() == 0, "sys_errno() is 0 before anything has failed");

    // --- ENOENT vs EFAULT, the two ways open() can refuse a path -------
    int fd = sys_open("/definitely/not/here.txt", 0);
    utest_check(fd < 0, "open() of a missing file fails");
    check_errno(sys_errno(), ENOENT, "open() of a missing file");

    // A pointer this process may not read. The kernel walks page tables
    // rather than dereferencing it, so this is a refusal and not a
    // fault -- see vmm.h.
    fd = sys_open((const char *)0x1000, 0);
    utest_check(fd < 0, "open() of a kernel-only pointer fails");
    check_errno(sys_errno(), EFAULT, "open() of a bad pointer");

    // --- a file that definitely exists, for the EMFILE fixture ---------
    fd = sys_open(PROBE, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    utest_check(fd >= 0, "created the probe file");
    if (fd < 0) { utest_check(0, "build the fixture"); sys_exit(utest_end()); }
    sys_write(fd, "x", 1);
    sys_close(fd);

    // Prove it opens NOW, so that when the same open fails below, the
    // only thing that changed is the descriptor table. Without this the
    // EMFILE check would be asserting against a file it never
    // established was there.
    fd = sys_open(PROBE, 0);
    utest_check(fd >= 0, "the probe file opens with a free descriptor table");
    if (fd >= 0) sys_close(fd);

    // --- O_CREAT into a directory that is not there -------------------
    // The failure this was written for: sys_open() discarded
    // fs_touch()'s 0, so this returned a working-looking fd and created
    // nothing -- silent at both the syscall and, through tftpd, the
    // network. What the positive control actually reddens is the first
    // check; the third is here for the OTHER failure -- a version that
    // reports the error and creates the file anyway.
    fd = sys_open("/definitely/not/here.txt", SYS_O_WRITE | SYS_O_CREAT);
    utest_check(fd < 0, "open(O_CREAT) into a missing directory fails");
    check_errno(sys_errno(), ENOENT, "open(O_CREAT) with no parent directory");
    fd = sys_open("/definitely/not/here.txt", 0);
    utest_check(fd < 0, "and the file was NOT created");
    if (fd >= 0) sys_close(fd);

    // A parent that exists and is a FILE. Distinct from ENOENT: one
    // says the path is absent, the other that it is wrong.
    fd = sys_open(PROBE_UNDER("/child.txt"), SYS_O_WRITE | SYS_O_CREAT);
    utest_check(fd < 0, "open(O_CREAT) under a file used as a directory fails");
    check_errno(sys_errno(), ENOTDIR, "open(O_CREAT) with a file as the parent");
    if (fd >= 0) sys_close(fd);

    // mkdir() answers the same two, having previously guessed ENOENT
    // for every refusal.
    utest_check(sys_mkdir("/definitely/not/here") == -1 && sys_errno() == ENOENT,
          "mkdir() with no parent directory is -1 ENOENT");
    utest_check(sys_mkdir(PROBE_UNDER("/sub")) == -1 && sys_errno() == ENOTDIR,
          "mkdir() under a file is -1 ENOTDIR");

    // --- EMFILE: exhaust this process's table for real ------------------
    //
    // **FILLED WITH dup(), NOT WITH open(), AND THAT IS THE WHOLE
    // POINT.** There are two tables -- FD_MAX descriptors per process
    // and FD_DESC_MAX open-file DESCRIPTIONS for the whole system -- and
    // open() needs one of each. On a machine with a dozen services
    // alive the shared one runs out FIRST, so an open loop stops with
    // this process's table half empty and every conclusion drawn from
    // "it is full now" is false: the dup() below then succeeds and the
    // test reports a bug that is not there. It failed exactly that way,
    // intermittently, for months. dup() takes only a descriptor -- it
    // shares the description it copies -- so this loop fills the one
    // table it means to and cannot be affected by anything else running.
    // STATIC, and sized past the kernel's per-space ceiling: FD_MAX is
    // 256 now that the table grows (kernel/include/kernel/syscalls.h),
    // and an array too small to reach it makes this loop end with the
    // table half full -- which is the exact false conclusion the
    // comment above warns about, arrived at from the other direction.
    // Static because 300 ints is past the ring-3 frame budget.
    static int held[300];
    int n = 0;
    while (n < (int)(sizeof held / sizeof held[0])) {
        int h = sys_dup(0);
        if (h < 0) break;
        held[n++] = h;
    }
    utest_check(n > 0, "dup()ed fd 0 until this process's table was full");
    check_errno(sys_errno(), EMFILE, "dup() with a full descriptor table");

    // ...and now open() must report the same thing, because the table
    // that is full is this process's. THE POINT OF ALL OF IT: the same
    // path, the same process, two different answers. Anything that
    // merges these two is the bug.
    utest_check(sys_open(PROBE, 0) < 0, "open() fails with a full table");
    check_errno(sys_errno(), EMFILE, "open() with a full descriptor table");
    utest_notef("ENOENT and EMFILE are distinct: %s", ENOENT != EMFILE ? "yes" : "NO");

    for (int i = 0; i < n; i++) sys_close(held[i]);

    // --- EBADF, its three shapes --------------------------------------
    char buf[8];
    utest_check(sys_read(99, buf, sizeof buf) < 0, "read() of an fd that was never opened fails");
    check_errno(sys_errno(), EBADF, "read() of an unopened fd");

    utest_check(sys_close(99) < 0, "close() of an unopened fd fails");
    check_errno(sys_errno(), EBADF, "close() of an unopened fd");

    // Open for WRITING and then read it -- the fd is perfectly valid and
    // still the wrong way round, which POSIX also calls EBADF.
    fd = sys_open(PROBE, SYS_O_WRITE);
    utest_check(fd >= 0, "reopened the probe for writing");
    if (fd >= 0) {
        utest_check(sys_read(fd, buf, sizeof buf) < 0, "read() of a write-only fd fails");
        check_errno(sys_errno(), EBADF, "read() of a write-only fd");
        sys_close(fd);
    }

    // --- EFAULT on a buffer, not a path -------------------------------
    utest_check(sys_write(1, (const void *)0x1000, 8) < 0, "write() of a kernel-only buffer fails");
    check_errno(sys_errno(), EFAULT, "write() of a bad buffer");

    // --- EINVAL: arguments the kernel will not take -------------------
    utest_check(sys_getrandom(buf, 1000000) < 0, "getrandom() over the maximum fails");
    check_errno(sys_errno(), EINVAL, "getrandom() with too large a count");

    utest_check(sys_socket(1, NET_ABI_SOCK_DGRAM, NET_ABI_IPPROTO_ICMP) < 0,
          "socket() with an unsupported address family fails");
    check_errno(sys_errno(), EINVAL, "socket() with a family that is not AF_INET");

    // --- EINVAL: the fd is fine, the ARGUMENTS are not ----------------
    // Distinct from the EBADF above on purpose: a caller told EBADF
    // would go looking at its own descriptor, which is not the problem.
    // send() on a datagram socket cannot know where to send -- nothing
    // names a peer -- so it is the call that is wrong, not the fd.
    fd = sys_socket(NET_ABI_AF_INET, NET_ABI_SOCK_DGRAM, NET_ABI_IPPROTO_ICMP);
    utest_check(fd >= 0, "socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP) succeeds");
    if (fd >= 0) {
        utest_check(sys_send(fd, "x", 1) < 0, "send() on a peerless socket fails");
        check_errno(sys_errno(), EINVAL, "send() with no peer named");

        // --- EFAULT on a struct argument, through the raw entry ------
        // The wrappers build the `struct net_msg` themselves, so only a
        // raw call can hand the kernel a bad one. A handler that tests
        // the copy helper's 1/0 result with `< 0` never sees the fault
        // and binds whatever its stack held; this is the check for that.
        utest_check(sys_call(SYS_BIND, (uint64_t)fd, 0x1000, 0) == -EFAULT,
              "bind() of a kernel-only struct pointer fails with EFAULT");
        utest_check(sys_call(SYS_CONNECT, (uint64_t)fd, 0x1000, 0) == -EFAULT,
              "connect() of a kernel-only struct pointer fails with EFAULT");
        utest_check(sys_call(SYS_SENDTO, (uint64_t)fd, 0x1000, 0) == -EFAULT,
              "sendto() of a kernel-only struct pointer fails with EFAULT");
        sys_close(fd);
    }

    // --- ECHILD: permanent, and distinct from "not yet" ---------------
    // An init loop that confuses these two either spins forever or stops
    // reaping (see docs/init-design.md), so the code matters more here
    // than anywhere else in this file.
    utest_check(sys_waitpid(-1, 0) < 0, "waitpid(-1) with no children fails");
    check_errno(sys_errno(), ECHILD, "waitpid(-1) with no children at all");

    // **A LIVE PID THAT IS NOT OURS IS ALSO ECHILD, AND THIS ONE USED TO
    // HANG.** waitpid() named a pid and the kernel only asked whether
    // that slot was alive -- scheduler_poll() reports on ANY process --
    // so the caller was told "running" and parked on its OWN channel,
    // while that process's death wakes its REAL parent's. Nothing ever
    // ended that sleep. pid 1 is init: certainly alive, certainly not
    // our child. Without the parentage check this line does not fail,
    // it never returns.
    utest_check(sys_waitpid(1, 0) < 0, "waitpid() on a live non-child fails");
    check_errno(sys_errno(), ECHILD, "waitpid() on a process that is not ours");

    // --- ENOSYS: a number with nothing behind it ----------------------
    // Three shapes, through the RAW entry because a wrapper cannot ask
    // for a number it does not know: index 0 (a table row with no
    // handler), a retired number (3 was SYS_GUI_INIT; its row is a
    // deliberate hole), and one past the table's end. Each must say
    // ENOSYS, and none may hand back the number it was asked for --
    // which is what a fall-through that leaves RAX alone does, and what
    // a program probing for a call would read as success.
    utest_check(sys_call(0, 0, 0, 0) == -ENOSYS, "syscall 0 (an empty table row) fails with ENOSYS");
    utest_check(sys_call(3, 0, 0, 0) == -ENOSYS, "syscall 3 (retired) fails with ENOSYS");
    utest_check(sys_call(UINT64_MAX, 0, 0, 0) == -ENOSYS, "a syscall number past the table fails with ENOSYS");
    // And the process is intact afterwards: an implemented call still works.
    // Raw again, and one that answers under the legacy loader too (no
    // scheduler slot there, so getpid is not a valid probe).
    struct rtc_time after;
    utest_check(sys_call(SYS_GETTIME, (uint64_t)(uintptr_t)&after, 0, 0) == 0,
                "an implemented call still works after the refused ones");

    // --- strerror ------------------------------------------------------
    utest_check(strerror(ENOENT)[0] != '\0', "strerror(ENOENT) is a real message");
    utest_check(strerror(4242)[0] != '\0', "strerror() of an unknown code still says something");

    // --- and the reason is STICKY, which callers must know -------------
    // Not cleared by a success, exactly as POSIX specifies. Asserted so
    // nobody "fixes" it into clearing and quietly costs every successful
    // syscall a store.
    sys_open("/definitely/not/here.txt", 0);
    int before = sys_errno();
    sys_close(sys_dup(1));
    utest_check(sys_errno() == before, "a successful call does NOT clear sys_errno()");

    // --- the flipped bucket: the calls whose failure was 0 -------------
    // Converted in one commit with every caller (docs/errno-design.md's
    // leftover). 0 is success now, and the refusals carry reasons.
    utest_check(sys_unlink("/definitely/not/here.txt") == -1 && sys_errno() == ENOENT,
          "unlink() of a missing file is -1 ENOENT");
    utest_check(sys_kill(4000, SIGTERM) == -1 && sys_errno() == ESRCH,
          "kill() of a missing pid is -1 ESRCH");
    utest_check(sys_proc_info(SYS_PROC_MAX + 5, &(struct proc_info){0}) == -1 &&
          sys_errno() == EINVAL,
          "proc_info() past the table is -1 EINVAL (the enumeration terminator)");
    utest_check(sys_proc_info(0, &(struct proc_info){0}) == 0,
          "proc_info(0) succeeds -- slot 0 is init on any boot with one");
    struct rtc_time t;
    utest_check(sys_gettime(&t) == 0, "gettime() succeeds with 0 now");
    int pfd[2];
    utest_check(sys_pipe(pfd) == 0, "pipe() succeeds with 0 now");
    sys_close(pfd[0]);
    sys_close(pfd[1]);

    sys_unlink(PROBE);

    sys_exit(utest_end());
}
