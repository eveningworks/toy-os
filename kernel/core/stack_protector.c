// Stack-canary runtime support (Milestone 2, docs/roadmap.md) --
// __stack_chk_guard/__stack_chk_fail are never called by name from
// anywhere in this codebase; GCC's own generated code (any function
// -fstack-protector-strong decides to instrument, see the Makefile's
// CFLAGS comment) implicitly reads __stack_chk_guard in its prologue
// and calls __stack_chk_fail() from its epilogue if the value on the
// stack no longer matches on return. Both symbols just need to exist
// with external linkage; nothing else in the kernel references them
// directly, so this file has no header -- there's no API surface for
// another .c file to include.
//
// __stack_chk_guard is a fixed compile-time constant, not a random
// value seeded at boot -- there's no entropy source in this kernel yet
// (see docs/roadmap.md's kernel ASLR item, blocked on the same gap).
// A fixed guard still catches the common case this is meant to catch
// (a linear buffer overflow walking off the end of a local array and
// corrupting whatever's next on the stack) -- it just can't defend
// against an attacker who has read this exact binary and can craft an
// overflow that writes the correct guard bytes back on its way past.
// See docs/decisions.md for the fuller writeup.
#include <stdint.h>
#include "vga.h"
#include "klog.h"

uintptr_t __stack_chk_guard = 0xDEC0DE99AA55C3A5ULL;

void __stack_chk_fail(void) {
    vga_set_color(VGA_WHITE, VGA_RED);
    vga_write("\n*** KERNEL PANIC: stack smashing detected ***\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    klog_write("PANIC: stack smashing detected (__stack_chk_fail)\n");

    // Same unconditional halt idt.c's non-recoverable fault path ends
    // on -- a stack-layout bug this deep is always fatal, there's no
    // "recoverable" case the way a ring-3 process fault has one.
    for (;;) __asm__ volatile ("cli; hlt");
}
