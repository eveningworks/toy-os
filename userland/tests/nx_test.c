// A freestanding userland test program that deliberately jumps into a
// writable data page and tries to execute it, to prove NX enforcement
// (Milestone 2, docs/roadmap.md; kernel/mm/vmm.c's PAGE_NX, boot.asm's
// EFER.NXE) is actually real, not just present in the code -- same
// "prove it, don't just read the code and assume" shape as
// crash_test.c's deliberate-fault self-test, run the same way: an
// ordinary userland ELF run via the shell's `run nx_test`, success
// criterion is "the kernel catches this as a fault and control returns
// cleanly", not "the machine survives via halt".
//
// `code_buf` below lands in .bss -- writable, and (since
// userland/link.ld's PHDRS/W^X split) NOT executable, unlike a single
// merged RWX segment before this feature existed. Copying a tiny valid
// x86-64 function (just `ret`) into it and calling it is exactly the
// shape of a real exploit's second stage (inject bytes into a writable
// buffer, then jump to them) -- NX is what's supposed to turn that into
// a fault instead of a successful execution.
#include <stdint.h>
#include "rt/sys.h"





static uint64_t my_strlen(const char *s) {
    uint64_t n = 0;
    while (s[n]) n++;
    return n;
}

static void put(const char *s) {
    sys_write(1, s, my_strlen(s));
}

// Writable, not executable -- see this file's top comment.
static uint8_t code_buf[16];

typedef void (*fn_t)(void);

int main(void) {
    put("nx_test: writing a tiny function (just 'ret', 0xC3) into a\n");
    put("nx_test: writable .bss buffer, then calling it as code\n");

    code_buf[0] = 0xC3; // ret

    put("nx_test: about to call into code_buf -- if NX is enforced this\n");
    put("nx_test: instruction fetch faults and the kernel tears this\n");
    put("nx_test: process down instead of letting it run\n");

    fn_t injected = (fn_t)(uintptr_t)code_buf;
    injected(); // page fault (NX violation) -- never returns if NX works

    // Unreachable if NX enforcement is working -- if this ever prints,
    // the injected code executed successfully and NX did NOT stop it.
    put("nx_test: UNEXPECTEDLY SURVIVED THE CALL -- NX is NOT enforced\n");
    sys_call(SYS_EXIT, 1, 0, 0);
}
