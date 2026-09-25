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
// __stack_chk_guard starts as a fixed compile-time constant and is
// REPLACED with a random one early in boot (stack_guard_randomize()
// below, called from kernel_main() once krandom_init() has run). The
// constant is still what protects every frame before that point, which
// is most of the arch bring-up -- it is a fallback, not a placeholder.
//
// Why a random guard matters at all: a fixed one catches the common
// case either way (a linear overflow walking off the end of a local
// array), but an attacker who has read this exact binary knows the
// constant and can write it back on the way past. A value that differs
// per boot cannot be baked into an exploit.
#include <stdint.h>
#include "vga.h"
#include "klog.h"
#include "kfmt.h"     // klog_printf
#include "krandom.h"
#include "panic.h"

uintptr_t __stack_chk_guard = 0xDEC0DE99AA55C3A5ULL;

// **The trap this function exists to contain.** With
// -mstack-protector-guard=global, an instrumented function reads this
// global in its PROLOGUE and compares against it in its EPILOGUE. So
// changing it while any instrumented frame is live makes that frame
// fail its check on return -- a "stack smashing detected" panic caused
// by the defence itself, in code that did nothing wrong.
//
// Two things make this safe here, and both are requirements on the
// CALLER rather than properties of this code:
//
//   1. It must be called from kernel_main() directly, as a statement
//      between other calls -- never from inside a helper. At that
//      point the only instrumented frame that can be live is
//      kernel_main()'s own.
//   2. kernel_main() must never return, which it doesn't (it ends in
//      the shell loop, and the halt below it is unreachable). Its own
//      epilogue would compare against the new guard having stored the
//      old one, so a kernel_main() that returned would panic here.
//
// This function is itself marked no_stack_protector so it cannot be the
// frame that trips over its own change.
__attribute__((no_stack_protector))
void stack_guard_randomize(void) {
    enum krandom_quality q = krandom_quality();
    if (q == KRANDOM_NONE) {
        // Refuse rather than install a guard mixed from nothing: the
        // compile-time constant above is no weaker than a "random" one
        // derived from a source that has admitted it has no entropy,
        // and swapping it would only make the failure look handled.
        klog_write("krandom: stack guard left at its build-time value (no entropy)\n");
        return;
    }

    uint64_t g = krandom_u64();

    // A guard containing a zero byte is the classic weakness -- a
    // string-copy overflow stops at a NUL, so an attacker who only
    // needs to reproduce the bytes up to the first zero has less work
    // to do. Force the low byte to zero DELIBERATELY instead, which is
    // what glibc does: it makes the guard terminate a string copy
    // rather than survive one, turning that same property into a
    // defence. The cost is eight bits of the guard, knowingly spent.
    g &= ~0xFFULL;

    __stack_chk_guard = (uintptr_t)g;
    klog_printf("krandom: stack guard randomized from %s\n", krandom_quality_name(q));
}

void __stack_chk_fail(void) {
    vga_set_color(VGA_WHITE, VGA_RED);
    vga_write("\n*** KERNEL PANIC: stack smashing detected ***\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    klog_write(KLOG_CRIT "PANIC: stack smashing detected (__stack_chk_fail)\n");

    // Same unconditional halt idt.c's non-recoverable fault path ends
    // on -- a stack-layout bug this deep is always fatal, there's no
    // "recoverable" case the way a ring-3 process fault has one.
    panic_finish();
}
