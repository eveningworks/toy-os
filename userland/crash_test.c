// A freestanding userland test program that deliberately crashes, to
// exercise the new ring-3 fault recovery path (idt.c's fault handler,
// process.c's process_context_recover(), syscall.c's
// syscall_process_exit_cleanup(), vmm.c's vmm_destroy_address_space())
// end to end -- unlike ring3_test.c/elf_test.c's deliberate faults
// (which use their own raw, manual iretq and are NOT recoverable by
// design, see process.h), this one runs through the ordinary
// process_run_ring3() path, so the kernel now catches the fault, tears
// the process down, and hands control straight back to whoever ran it
// -- no reboot needed, unlike ring3test/elftest.
//
// Prints a message, then writes through a wild pointer at a fixed low
// address (0x1000) -- present in every process's shared kernel identity
// map (see vmm.h) but never USER-accessible, so this reliably page-faults
// (a supervisor-only protection violation, not "unmapped entirely") the
// same way a real null/wild pointer bug would in any ring-3 program.
#include <stdint.h>
#include "sys.h"





static uint64_t my_strlen(const char *s) {
    uint64_t n = 0;
    while (s[n]) n++;
    return n;
}

static void put(const char *s) {
    sys_write(1, s, my_strlen(s));
}

int main(void) {
    put("crash_test: about to write through a wild pointer at 0x1000\n");
    put("crash_test: (this address is kernel-only, not user-accessible --\n");
    put("crash_test: see syscall_abi.h / vmm.h for why every process's\n");
    put("crash_test: page tables have SOMETHING mapped there but can't\n");
    put("crash_test: touch it from ring 3)\n");

    volatile int *wild = (volatile int *)0x1000;
    *wild = 0xDEAD; // page fault -- never returns; the kernel catches
                     // this, tears the process down, and hands control
                     // back to whatever called process_run_ring3()

    // Unreachable -- if this ever prints, the fault didn't happen and
    // something about the test itself is wrong, not the recovery path.
    put("crash_test: UNEXPECTEDLY SURVIVED THE WRITE -- test is broken\n");
    sys_call(SYS_EXIT, 1, 0, 0);
}
