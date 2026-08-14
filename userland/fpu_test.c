// Proves ring-3 floating point works at all: real `double` arithmetic
// in a ring-3 ELF, which would have raised #UD before CR4.OSFXSR was
// set (see kernel/include/kernel/fpu.h).
//
// Deliberately a value test, not a "did it crash" test. Enabling SSE
// wrongly has two distinct failure modes and only one of them faults:
// the instruction can be unavailable (#UD, obvious), or it can execute
// against a bad MXCSR/control word and quietly produce the wrong
// number. Comparing against expected results catches both.
//
// Exits 0 if every check passes, or the 1-based index of the first
// check that failed -- so a failure says WHICH one, not just "no".
#include <stdint.h>
#include "sys.h"







static void puts_(const char *s) {
    uint64_t n = 0;
    while (s[n]) n++;
    sys_call(SYS_WRITE, 1, (uint64_t)(uintptr_t)s, n);
}

// Globals rather than stack locals, same reasoning as write_test.c's:
// guaranteed to sit in a PT_LOAD segment the loader actually mapped.
static double g_a = 3.5;
static double g_b = 2.0;
static double g_acc;
static float  g_f = 1.5f;

// Exact in binary floating point (every value here is a dyadic
// rational), so these are `==` comparisons on purpose -- no epsilon
// needed, and an epsilon would hide exactly the sort of
// wrong-rounding-mode bug this is here to catch.
int main(void) {
    if (g_a + g_b != 5.5) sys_exit(1);
    if (g_a - g_b != 1.5) sys_exit(2);
    if (g_a * g_b != 7.0) sys_exit(3);
    if (g_a / g_b != 1.75) sys_exit(4);

    // int <-> double conversion, both directions (cvtsi2sd/cvttsd2si).
    if ((double)7 / g_b != 3.5) sys_exit(5);
    if ((int)(g_a * g_b) != 7) sys_exit(6);

    // float, not just double -- a different set of instructions
    // (movss/mulss) and a different half of the same registers.
    if (g_f * 2.0f != 3.0f) sys_exit(7);
    if ((double)g_f != 1.5) sys_exit(8);

    // A loop the compiler can't fold away, ending on an exact value:
    // 0.5 summed 64 times is 32.0, and every partial sum is exact.
    g_acc = 0.0;
    for (int i = 0; i < 64; i++) g_acc += 0.5;
    if (g_acc != 32.0) sys_exit(9);

    // sqrt via the hardware instruction rather than any libm -- there
    // is no libm here, and this is the one transcendental-ish operation
    // SSE2 gives directly.
    double r;
    __asm__ volatile ("sqrtsd %1, %0" : "=x"(r) : "x"(g_acc + 4.0)); // sqrt(36) == 6
    if (r != 6.0) sys_exit(10);

    // The stack must be 16-byte aligned at process entry, and this is
    // the check that proves it. `movapd` to a 16-byte-aligned stack
    // local #GPs -- faults outright, killing the process -- if RSP is
    // off by 8, because GCC placed `slot` relative to an RSP it assumed
    // was already aligned.
    //
    // elf_run.c's argv layout used to align RSP to 8, which was
    // invisible while userland was built -mno-sse and nothing could
    // emit an aligned SSE access at all. Nothing else in userland/
    // currently emits one either, so without this check the fix that
    // came with FP support would sit here untested until some future
    // program tripped over it. A crash here (rather than a non-zero
    // exit) is the expected failure mode.
    volatile double slot[2] __attribute__((aligned(16)));
    __asm__ volatile ("movapd %%xmm0, %0" : "=m"(slot[0]) :: "memory");
    __asm__ volatile ("movapd %0, %%xmm0" :: "m"(slot[0]) : "xmm0");

    puts_("fpu_test: all checks passed\n");
    sys_exit(0);
}
