// __stack_chk_guard/__stack_chk_fail for every userland ELF -- linked
// into each one (see the Makefile's $(FOO_ELF) rules and
// USERLAND_CFLAGS's comment). Same deal as kernel/lib/stack_protector.c:
// neither symbol is ever called by name from this codebase, GCC's own
// generated prologue/epilogue code for any -fstack-protector-strong
// -instrumented function references both implicitly. No header --
// nothing else includes this file, it's link-time-only runtime support.
//
// A fixed guard value, not random -- same reasoning as the kernel's
// (no entropy source exists yet, see docs/roadmap.md's kernel ASLR
// item) -- deliberately a DIFFERENT constant from the kernel's own
// __stack_chk_guard (kernel/lib/stack_protector.c) since these are
// two entirely separate address spaces/binaries with no reason to
// share a bit pattern.
//
// Unlike the kernel (which has nowhere to hand control back to on a
// genuinely fatal error), a userland stack-smash is just an ordinary
// process crash from the kernel's point of view -- SYS_EXIT with a
// distinct non-zero code is enough to tear it down cleanly and return
// control to whatever ran it (the shell's `run <name>`), same as any
// other process exit. No need to synthesize a fault the way a kernel
// bug would.
#include <stdint.h>
#include "syscall_abi.h"

uintptr_t __stack_chk_guard = 0x5A3CC5AA9900DEB1ULL;

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

void __attribute__((noreturn)) __stack_chk_fail(void) {
    static const char msg[] = "*** stack smashing detected -- process terminated ***\n";
    syscall3(SYS_WRITE, 1, (uint64_t)(uintptr_t)msg, sizeof(msg) - 1);
    syscall3(SYS_EXIT, 2, 0, 0); // distinct exit code -- "caught by the canary", not just any crash

    for (;;) {} // unreachable -- SYS_EXIT never returns
}
