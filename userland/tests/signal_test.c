// Signals and process groups, from ring 3 -- the syscall surface that
// kernel/proc/signal_test.c's KTESTs cannot reach.
//
// MUST BE SCHEDULER-SPAWNED, not driven by `run`. The legacy loader has
// no procs[] slot, so it has no pid, no group, no pending mask and no
// children -- every check here would be measuring the absence of a
// process rather than the behaviour of one. kernel/proc/signal_test.c
// spawns it, the same arrangement cputime_test and pipe_test already
// have, and tools/usertest_run.py excludes it by name with that reason.
//
// WHAT THIS IS FOR, beyond coverage: the kernel half can assert that a
// pending bit gets set, and cannot assert that a process actually DIES
// of it -- that needs a real process, really returning to ring 3, really
// being torn down, and a parent really reading the exit code back. That
// round trip is the thing worth testing, and it is only writable here.
#include "rt/sys.h"
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include "errno.h"

#include "lib/utest.h"

// **`noinline` IS LOAD BEARING, not style.** These carry a 192-byte
// line buffer each, and inlined into a main() with two dozen calls the
// buffers do not overlap -- 4.4 KB of frame against a 2 KB budget and a
// 16 KB ring-3 stack. One out-of-line copy costs nothing this test
// measures.
__attribute__((noinline))
// The call sites here read `check(what, ok, detail)`; the harness takes
// the boolean first. One adapter rather than transposing a hundred call
// sites: a transposed argument pair compiles and INVERTS the check,
// which is the failure a green suite hides.
static void check(const char *what, int ok, const char *detail) {
    utest_check_detail(ok, what, detail);
}

__attribute__((noinline))
static void checkf(const char *what, int ok, int got, int want) {
    char d[64];
    snprintf(d, sizeof d, "got %d, wanted %d", got, want);
    check(what, ok, ok ? 0 : d);
}

// --- what the handler tests need --------------------------------------
//
// `volatile` on every one of these is not decoration: a handler runs
// between two instructions of code the compiler believes nothing can
// interrupt, so without it a check reads a value cached in a register
// from before the signal and reports a working handler as broken.
static volatile int g_caught;
static volatile int g_witness;
static volatile int g_depth;
static volatile int g_max_depth;
static volatile int g_entries;
static volatile int g_chld;
static jmp_buf g_segv_jmp;

static void on_signal(int sig) { g_caught = sig; }

static void on_sigchld(int sig) { (void)sig; g_chld++; }

// Sends itself the same signal it is handling. If the kernel did not
// block it, this re-enters and g_max_depth climbs; POSIX says it must
// not, and abi/signal_abi.h says why that matters on a 4-page stack.
static void on_signal_reentrant(int sig) {
    g_caught = sig;
    g_entries++;
    g_depth++;
    if (g_depth > g_max_depth) g_max_depth = g_depth;
    // ONCE, AND THE COUNTER HAS TO BE `entries` RATHER THAN `depth`.
    // Keyed on depth this never terminates, and it is worth recording
    // because the failure is the feature working: the blocked signal is
    // DEFERRED, not dropped, so it is delivered again the moment
    // sigreturn clears the mask -- at depth 0, where a depth test says
    // "send another one". Two runs of this test hung the machine before
    // that was obvious.
    if (g_entries == 1) sys_kill(sys_getpid(), sig);
    g_depth--;
}

// JUMPS OUT rather than returning, and that is not laziness. A fault
// frame puts the process back on the faulting instruction, so returning
// from a SIGSEGV handler that has not fixed the cause re-executes the
// fault forever -- signal.c's own comment says so, and this is what
// that reads like from the program's side.
static void on_segv(int sig) {
    g_caught = sig;
    longjmp(g_segv_jmp, 1);
}

// "<pid> <sig>" for /tests/sigpoke, in a static buffer -- the arguments
// are a single string here, not an argv array.
static const char *poke_args(int pid, int sig) {
    static char buf[32];
    snprintf(buf, sizeof buf, "%d %d", pid, sig);
    return buf;
}

// Spin until `pid` is really SCHED_BLOCKED, or give up. Returns 1 if it
// got there.
//
// A SPAWNED CHILD IS NOT RUNNING YET, and that difference is the whole
// point of this helper: a signal sent to a child that has not been
// scheduled once is delivered at its first syscall, which is a real and
// correct path but NOT the one a "does a blocked process get
// interrupted?" check is about.
//
// Yields rather than sleeping, so the child gets the CPU; bounded, so a
// child that never blocks fails the check instead of hanging the suite.
static int wait_until_blocked(int pid) {
    struct proc_info info;
    for (int spin = 0; spin < 20000; spin++) {
        for (int i = 0; sys_proc_info(i, &info) == 0; i++) {
            if (info.pid != pid) continue;
            if (info.state == PROC_STATE_BLOCKED) return 1;
            break;
        }
        sys_yield();
    }
    return 0;
}

// A child that will not exit on its own: spins across many timer slices.
// The point of a LONG-running one is that every check below is really
// about the signal rather than about the child having finished anyway --
// the failure mode this project keeps writing rules about.
#define SPINNER "/tests/spin_test"
#define SPIN_ARGS "100000"

int main(void) {
    utest_begin("signal_test", "signals: delivery, handlers and groups", UTEST_KLOG);

    int me = sys_getpid();

    // --- groups -------------------------------------------------------

    int mine = sys_getpgid(0);
    check("every process is in a group", mine > 0, 0);

    // Lead our own, so nothing below depends on which group whoever
    // spawned this test happens to be in.
    checkf("a process can lead its own group",
           sys_setpgid(PGID_SELF, PGID_SELF) == 0, sys_getpgid(0), me);
    checkf("...and getpgid reports it", sys_getpgid(0) == me, sys_getpgid(0), me);

    check("getpgid of a pid that does not exist is refused",
          sys_getpgid(4000) < 0, 0);

    // --- dispositions -------------------------------------------------

    // SIGKILL is what makes a force-quit trustworthy; a process that
    // could ignore it could not be ended at all.
    check("SIGKILL cannot be ignored",
          sys_signal(SIGKILL, (sighandler_t)SIG_IGN) == SIG_ERR, 0);
    check("SIGQUIT cannot be ignored either",
          sys_signal(SIGQUIT, (sighandler_t)SIG_IGN) == SIG_ERR, 0);
    check("a signal number out of range is refused",
          sys_signal(99, (sighandler_t)SIG_IGN) == SIG_ERR, 0);
    // A HANDLER WITH NO RESTORER is the one refusal a caller can still
    // trip, and it is why sys_signal() exists: it fills the field in.
    struct k_sigaction bad = { .handler = 0x400000, .restorer = 0 };
    check("a handler with no restorer is refused",
          sys_sigaction(SIGINT, &bad, 0) < 0, 0);
    struct k_sigaction odd = { .handler = 0x400000,
                             .restorer = (uint64_t)(uintptr_t)__sigrestore,
                             .flags = 0x40 };
    check("an unknown SA_ flag is refused, not silently dropped",
          sys_sigaction(SIGINT, &odd, 0) < 0, 0);

    long prev = (long)(uintptr_t)sys_signal(SIGINT, (sighandler_t)SIG_IGN);
    checkf("signal() returns the PREVIOUS disposition", prev == SIG_DFL, (int)prev, SIG_DFL);
    prev = (long)(uintptr_t)sys_signal(SIGINT, (sighandler_t)SIG_IGN);
    checkf("...and reports it changed", prev == SIG_IGN, (int)prev, SIG_IGN);

    // THE LOAD-BEARING ONE: an ignored signal must not kill us. If it
    // did, this program would simply stop and the harness would report
    // a truncated run rather than a failed check -- so the line after it
    // is the evidence.
    sys_kill(me, SIGINT);
    check("an IGNORED signal does not terminate the process", 1, 0);
    sys_signal(SIGINT, (sighandler_t)SIG_DFL);

    // SIGCHLD's default is to be ignored, which is what lets the kernel
    // send one on every child exit without every program knowing.
    sys_kill(me, SIGCHLD);
    check("SIGCHLD's default action is to be ignored", 1, 0);

    // --- a child dies of a signal, and says which one -------------------

    int child = sys_spawn_group(SPINNER, SPIN_ARGS, -1, 0, PGID_NEW);
    if (child <= 0) {
        check("spawned a long-running child", 0, "spawn failed");
        return utest_end();
    }
    checkf("a child can be spawned into a group of its OWN",
           sys_getpgid(child) == child, sys_getpgid(child), child);

    check("...which is not its parent's", sys_getpgid(child) != sys_getpgid(0), 0);

    sys_kill(child, SIGTERM);
    int code = -1;
    sys_waitpid(child, &code);
    // 128 + the signal, the convention every Unix shell prints -- and
    // the whole reason a signalled death is distinguishable from an
    // ordinary non-zero exit.
    checkf("a signalled child reports 128 + the signal",
           code == SIGNAL_EXIT_BASE + SIGTERM, code, SIGNAL_EXIT_BASE + SIGTERM);

    // --- a whole GROUP, with one call -----------------------------------
    //
    // The property process groups exist for: `cat | grep | less` must
    // die as a unit. Two children in one group, one kill.
    int a = sys_spawn_group(SPINNER, SPIN_ARGS, -1, 0, PGID_NEW);
    int b = a > 0 ? sys_spawn_group(SPINNER, SPIN_ARGS, -1, 0, sys_getpgid(a)) : -1;
    if (a > 0 && b > 0) {
        int pg = sys_getpgid(a);
        checkf("two children can share one group", sys_getpgid(b) == pg,
               sys_getpgid(b), pg);
        // A NEGATIVE pid is the group, POSIX's spelling.
        check("kill(-pgid) reports success", sys_kill(-pg, SIGTERM) == 0, 0);

        int ca = -1, cb = -1;
        sys_waitpid(a, &ca);
        sys_waitpid(b, &cb);
        checkf("...and BOTH members died of it",
               ca == SIGNAL_EXIT_BASE + SIGTERM && cb == SIGNAL_EXIT_BASE + SIGTERM,
               ca, SIGNAL_EXIT_BASE + SIGTERM);
    } else {
        check("spawned two children into one group", 0, "spawn failed");
    }

    // --- a BLOCKED child is interruptible --------------------------------
    //
    // The half that needed EINTR. A process asleep in a blocking syscall
    // is not on its way back to ring 3, and delivery only happens there
    // -- so without waking it, a signal to a blocked process would be a
    // bit set forever. Parked on an EMPTY PIPE this process holds the
    // write end of, so nothing but the signal can release it: a child
    // blocked on the console would be woken by any keystroke and the
    // check would pass for the wrong reason.
    int fds[2];
    if (sys_pipe(fds) == 0) {
        int saved = sys_dup(0);
        sys_dup2(fds[0], 0);
        int reader = sys_spawn_group("/tests/catin", 0, -1, 0, PGID_NEW);
        sys_dup2(saved, 0);
        sys_close(saved);
        sys_close(fds[0]);   // the child holds its own copy

        if (reader > 0) {
            // WAIT UNTIL IT IS REALLY PARKED, and assert that it got
            // there. Without this the check passed with the EINTR wake
            // disabled entirely: the child had not been scheduled yet,
            // so the signal landed on a READY process and was delivered
            // at its FIRST syscall -- a correct outcome, of a different
            // mechanism than the one under test. The fixture never
            // reached the code under test (CLAUDE.md), and only a
            // positive control showed it.
            int parked = wait_until_blocked(reader);
            check("...and it really is parked before the signal is sent", parked, 0);

            // Nothing is ever written, and this process keeps the write
            // end open -- so the child cannot see EOF either. It is
            // parked, and only a signal can end it.
            sys_kill(reader, SIGTERM);
            int rc = -1;
            sys_waitpid(reader, &rc);
            checkf("a child BLOCKED in a syscall is interrupted and dies",
                   parked && rc == SIGNAL_EXIT_BASE + SIGTERM,
                   rc, SIGNAL_EXIT_BASE + SIGTERM);
        } else {
            check("spawned a child that blocks on a pipe", 0, "spawn failed");
        }
        sys_close(fds[1]);
    } else {
        check("could get a pipe for the blocking check", 0, "no pipes");
    }

    // --- SIGKILL reaches a process that ignores everything ---------------
    int stubborn = sys_spawn_group(SPINNER, SPIN_ARGS, -1, 0, PGID_NEW);
    if (stubborn > 0) {
        sys_kill(stubborn, SIGKILL);
        int sc = -1;
        sys_waitpid(stubborn, &sc);
        checkf("SIGKILL terminates, and reports 128 + 9",
               sc == SIGNAL_EXIT_BASE + SIGKILL, sc, SIGNAL_EXIT_BASE + SIGKILL);
    }

    // --- HANDLERS: the whole of stage 3 ----------------------------------
    //
    // Everything above tests a signal ENDING a process. These test one
    // being caught, run, and RETURNED FROM -- which means the frame the
    // kernel pushed unwound exactly, or the checks after each one would
    // not run at all. That is the strongest part of the evidence here:
    // a corrupted restore does not fail an assertion, it crashes, and
    // the harness reports a truncated run.

    sys_signal(SIGINT, on_signal);
    g_caught = 0; g_witness = 0x1234;
    sys_kill(me, SIGINT);
    checkf("a handler RUNS when the signal is sent", g_caught == SIGINT,
           g_caught, SIGINT);
    check("...and execution resumes after it", g_witness == 0x1234, 0);

    // A HANDLER IS NOT A ONE-SHOT. System V reset the disposition on
    // every delivery, which is a race every program had to write around;
    // BSD and glibc leave it installed, and so does this.
    g_caught = 0;
    sys_kill(me, SIGINT);
    checkf("...and stays installed for the next one", g_caught == SIGINT,
           g_caught, SIGINT);

    // THE REGISTERS COME BACK. A frame that restores RIP and RSP and
    // loses a callee-saved register produces a bug thousands of
    // instructions away, so this pins a value in one across the
    // delivery. Written in asm because C gives no way to insist a
    // particular register survives a particular statement.
    uint64_t before = 0x5A5AC0FFEE5A5A11ULL, after = 0;
    __asm__ volatile (
        "mov %1, %%r12\n\t"
        "int $0x80\n\t"        // sys_kill(me, SIGINT) -- inline, so no
        "mov %%r12, %0"        // call boundary is allowed to spill r12
        : "=r"(after)
        : "r"(before), "a"((uint64_t)SYS_KILL), "D"((uint64_t)me),
          "S"((uint64_t)SIGINT)
        : "r12", "rcx", "r11", "memory"
    );
    check("a general register survives the round trip", after == before, 0);

    // NESTING IS BLOCKED WHILE A HANDLER RUNS -- POSIX's default, and
    // what stops a repeated signal walking the 4-page user stack into
    // its guard page. The handler sends itself the same signal; if it
    // nested, g_depth would come back above 1.
    sys_signal(SIGINT, on_signal_reentrant);
    g_depth = 0; g_max_depth = 0; g_entries = 0; g_caught = 0;
    sys_kill(me, SIGINT);
    checkf("the signal is BLOCKED inside its own handler", g_max_depth == 1,
           g_max_depth, 1);
    // AND DEFERRED, NOT DROPPED -- the second entry is the evidence, and
    // it is the half that makes the check above mean something. A kernel
    // that lost the blocked signal entirely would also report max depth
    // 1, and would be wrong.
    checkf("...and delivered again once the handler returns", g_entries == 2,
           g_entries, 2);

    // SIGSEGV IS A SIGNAL NOW, not an unconditional teardown. The
    // handler jumps clear rather than returning, because returning
    // would re-execute the faulting instruction forever -- signal.c
    // says so, and this is what that reads like from ring 3.
    sys_signal(SIGSEGV, on_segv);
    g_caught = 0;
    if (!setjmp(g_segv_jmp)) {
        // THROUGH A VOLATILE, so the compiler cannot see that this is
        // a null write and warn about the array bounds of address zero.
        // The address is deliberately not 0 either: page zero being
        // unmapped is the mechanism, and 0x10 exercises it without
        // looking like a forgotten initialisation.
        static volatile uintptr_t nowhere = 0x10;
        *(volatile int *)nowhere = 1;
        check("a null write faulted", 0, "it did not fault at all");
    }
    checkf("a FAULT is delivered to the process as SIGSEGV",
           g_caught == SIGSEGV, g_caught, SIGSEGV);
    sys_signal(SIGSEGV, (sighandler_t)SIG_DFL);

    // SA_RESTART ACROSS A BLOCKING READ, BOTH WAYS. The child signals us
    // while we are parked in read(), and the same sequence is run twice
    // -- once with the flag and once without.
    //
    // **RUNNING IT BOTH WAYS IS WHAT MAKES IT A TEST.** With the flag
    // the read is re-entered and returns its byte; without it the read
    // fails with EINTR. Either result alone is also what you would see
    // if the signal had simply arrived before the parent ever blocked,
    // so one direction proves nothing -- the two DIFFERING is the
    // evidence that the read was genuinely interrupted.
    for (int restart = 1; restart >= 0; restart--) {
        int rfd[2];
        if (sys_pipe(rfd) != 0) {
            check("could get a pipe for the restart check", 0, "no pipes");
            break;
        }
        struct k_sigaction act = {
            .handler  = (uint64_t)(uintptr_t)on_signal,
            .restorer = (uint64_t)(uintptr_t)__sigrestore,
            .flags    = restart ? SA_RESTART : 0,
        };
        sys_sigaction(SIGTERM, &act, 0);

        g_caught = 0;
        int poker = sys_spawn_group("/tests/sigpoke", poke_args(me, SIGTERM),
                                    rfd[1], 0, PGID_NEW);
        sys_close(rfd[1]); // the child holds its own copy
        if (poker > 0) {
            char b = 0;
            int n = sys_read(rfd[0], &b, 1);
            if (restart) {
                checkf("SA_RESTART: the interrupted read returns its byte",
                       n == 1 && b == 'R', n, 1);
            } else {
                checkf("without it: the interrupted read fails with EINTR",
                       n < 0 && sys_errno() == EINTR, sys_errno(), EINTR);
            }
            checkf("...and either way the handler ran", g_caught == SIGTERM,
                   g_caught, SIGTERM);
            sys_waitpid(poker, 0);
        } else {
            check("spawned the poker", 0, "spawn failed");
        }
        sys_close(rfd[0]);
    }
    sys_signal(SIGTERM, (sighandler_t)SIG_DFL);
    sys_signal(SIGINT, (sighandler_t)SIG_DFL);

    // --- SIGCHLD, on both of the two ways a child can die -----------------
    //
    // **TWO CHECKS BECAUSE THERE ARE TWO DEATHS, and the second is the
    // one a single check would miss.** A process leaves through
    // scheduler_on_exit() when it exits under its own power and through
    // scheduler_kill() when somebody else ends it -- separate functions
    // in the kernel, and this tree has already shipped a bug where work
    // that belonged in both lived in only one (killing a process freed
    // none of its memory for months). Testing only the exit path would
    // pass against exactly that mistake.
    //
    // Counted rather than flagged, and reset before each, because the
    // pending set is a BITMASK: two deaths close enough together
    // collapse into one delivery, so the deaths here are sequenced --
    // one child fully reaped before the next is spawned -- and the count
    // is then an exact expectation rather than a lower bound.
    struct k_sigaction chld = {
        .handler  = (uint64_t)(uintptr_t)on_sigchld,
        .restorer = (uint64_t)(uintptr_t)__sigrestore,
        .flags    = SA_RESTART,
    };
    sys_sigaction(SIGCHLD, &chld, 0);

    g_chld = 0;
    int quick = sys_spawn_group("/bin/hello", 0, -1, 0, PGID_NEW);
    if (quick > 0) {
        sys_waitpid(quick, 0);
        checkf("a child exiting on its own raises SIGCHLD in the parent",
               g_chld == 1, g_chld, 1);
    } else {
        check("spawned a short-lived child", 0, "spawn failed");
    }

    g_chld = 0;
    int doomed = sys_spawn_group(SPINNER, SPIN_ARGS, -1, 0, PGID_NEW);
    if (doomed > 0) {
        // SIGKILL, so the death goes through scheduler_kill() rather
        // than through the victim's own return to ring 3 -- which is
        // precisely the path the check above cannot reach.
        sys_kill(doomed, SIGKILL);
        sys_waitpid(doomed, 0);
        checkf("...and so does one that is KILLED by somebody else",
               g_chld == 1, g_chld, 1);
    } else {
        check("spawned a child to kill", 0, "spawn failed");
    }

    // THE OTHER HALF OF THE CLAIM: with the handler taken away, a child
    // death must go back to costing this process nothing. If SIGCHLD
    // were reaching the pending set regardless of disposition, the
    // default action would terminate us and this line would never
    // print -- so the check after the spawn is the evidence, exactly as
    // the ignored-SIGINT check above is.
    sys_signal(SIGCHLD, (sighandler_t)SIG_DFL);
    int ignored = sys_spawn_group("/bin/hello", 0, -1, 0, PGID_NEW);
    if (ignored > 0) sys_waitpid(ignored, 0);
    check("a child death with SIGCHLD at its default does not disturb the parent",
          1, 0);

    // --- signalling something that is not there ---------------------------
    check("a signal to a pid that does not exist is refused",
          sys_kill(4000, SIGTERM) != 0, 0);
    check("a group with no members is refused", sys_kill(-4000, SIGTERM) != 0, 0);
    check("a number that is not a signal is refused", sys_kill(me, 99) != 0, 0);

    return utest_end();
}
