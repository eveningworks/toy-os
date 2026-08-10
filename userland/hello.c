// A minimal freestanding "userland" test program: no libc, no crt0, no
// syscalls -- this is the smallest thing that can be a real, separately
// compiled and linked ELF64 executable. Loaded by kernel/core/elf.c and
// run in ring 3 by kernel/core/elf_test.c, replacing the 17 hand-encoded
// machine-code bytes the earlier ring3_test.c used.
//
// Same proof pattern as ring3_test.c: write a marker value somewhere the
// kernel can read back afterward, then execute a privileged instruction
// (hlt) that ring 3 cannot -- the resulting fault is how the kernel
// finds out this ran at all, since there's no syscall/exit path yet.
#include <stdint.h>
#include "userland_contract.h"

void _start(void) {
    *(volatile uint32_t *)USERLAND_MARKER_ADDR = 0xC0FFEE;
    __asm__ volatile ("hlt");
    for (;;) { } // unreachable
}
