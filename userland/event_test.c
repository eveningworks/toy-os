// The first ring-3 program to BLOCK in this kernel rather than
// spin-poll: waits for a given number of window events via
// SYS_WAIT_EVENT and exits with the number of them it actually
// received.
//
// Everything before this either polled (`echo.c` spins on
// SYS_READ_KEY, which is non-blocking by hard necessity -- see
// syscall.c's comment on why) or never waited at all. A process parked
// in SYS_WAIT_EVENT consumes no timeslices whatsoever; it is
// descheduled, not looping.
//
// Driven by kernel/proc/win_events_test.c, which spawns it, pushes a
// known number of events, and asserts the exit code matches. The exit
// code IS the assertion, which is why this prints nothing: the KTEST
// report it runs under is parsed off the same serial console (same
// reasoning as spin_test.c).
#include <stdint.h>
#include "syscall_abi.h"

static inline int64_t syscall2(uint64_t num, uint64_t arg1, uint64_t arg2) {
    int64_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2)
        : "memory"
    );
    return ret;
}

static inline void sys_exit(int code) __attribute__((noreturn));
static inline void sys_exit(int code) {
    syscall2(SYS_EXIT, (uint64_t)(int64_t)code, 0);
    for (;;) { } // unreachable
}

// The documented SYS_WAIT_EVENT contract, and the reason it is a loop:
// a 0 return means "you were woken, ask again", not "no event". The
// kernel cannot copy the event into this buffer at wake time -- the
// wake happens inside an interrupt handler under whatever address space
// was current, which is not necessarily this one -- so the copy has to
// happen back inside this process's own syscall. Same spurious-wakeup
// contract a condition variable has.
//
// This does NOT spin: every pass that finds nothing parks the process
// again in the kernel.
//
// Returns 1 with `*ev` filled, or -1 if the kernel refused (no event
// queue for this caller).
static int wait_event(struct win_event *ev) {
    int64_t r;
    do {
        r = syscall2(SYS_WAIT_EVENT, (uint64_t)(uintptr_t)ev, 0);
    } while (r == 0);
    return (int)r;
}

static int parse_count(const char *s) {
    if (!s || !*s) return 0;
    int v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return 0; // reject, don't guess
        v = v * 10 + (*s - '0');
        if (v > 1000) return 1000;
    }
    return v;
}

#define WANT_DEFAULT 3

void _start(int argc, char **argv) {
    int want = argc > 1 ? parse_count(argv[1]) : WANT_DEFAULT;
    if (want <= 0) want = WANT_DEFAULT;

    int got = 0;
    while (got < want) {
        struct win_event ev;
        if (wait_event(&ev) != 1) break; // refused -- exit with what we have
        if (ev.type != WIN_EV_NONE) got++;
    }

    sys_exit(got);
}
