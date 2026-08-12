// A freestanding userland test program that deliberately overflows a
// local stack buffer, to prove the stack canary (Milestone 2,
// docs/roadmap.md; userland/stack_chk.c's __stack_chk_fail) actually
// catches a real overflow instead of silently corrupting the stack --
// not just "the kernel still boots with the flag on". Same shape as
// crash_test.c's deliberate-fault self-test: an ordinary userland ELF
// run via the shell's `run stack_smash_test`, success criterion is
// "the kernel/runtime catches this and control returns cleanly to the
// shell", not "the machine survives via halt".
#include <stdint.h>
#include "syscall_abi.h"

static inline int64_t syscall3(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    int64_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2), "d"(arg3)
        : "memory"
    );
    return ret;
}

static inline int64_t sys_write(int fd, const char *buf, uint64_t len) {
    return syscall3(SYS_WRITE, (uint64_t)fd, (uint64_t)(uintptr_t)buf, len);
}

static uint64_t my_strlen(const char *s) {
    uint64_t n = 0;
    while (s[n]) n++;
    return n;
}

static void put(const char *s) {
    sys_write(1, s, my_strlen(s));
}

// volatile buf + writing through a separate volatile pointer (not
// buf[i] directly) so GCC can't statically prove the loop is
// out-of-bounds against buf's known size and either warn or optimize
// it away -- the whole point is for these stores to actually happen at
// runtime and walk past buf[]'s 8 bytes into the canary
// -fstack-protector-strong placed right after it on the stack.
//
// noinline is load-bearing, not just documentation: at -O2 GCC inlines
// this one-call-site function straight into _start, which moves the
// canary check to _start's OWN epilogue -- after _start's later code
// (the "survived" message + syscall3(SYS_EXIT, ...)) already ran and
// exited the process, so the check never gets a chance to fire at all.
// Confirmed by disassembly the first time this test was written: the
// process printed "UNEXPECTEDLY SURVIVED" and exited normally with the
// canary silently corrupted, not because canaries don't work but
// because the check was unreachable code by that point. Keeping
// smash() a real, non-inlined function gives it its own `ret` (and
// therefore its own canary check) that fires immediately on return,
// before _start ever reaches the "survived" path.
static void __attribute__((noinline)) smash(void) {
    volatile char buf[8];
    volatile char *p = buf;
    for (int i = 0; i < 64; i++) p[i] = 0x41;
}

void _start(void) {
    put("stack_smash_test: about to overflow a local stack buffer\n");
    put("stack_smash_test: __stack_chk_fail should catch this on return\n");

    smash();

    // Unreachable -- if this ever prints, the canary didn't fire and
    // something about the test itself is wrong, not proof the
    // protection works.
    put("stack_smash_test: UNEXPECTEDLY SURVIVED -- canary didn't fire, test is broken\n");
    syscall3(SYS_EXIT, 1, 0, 0);
}
