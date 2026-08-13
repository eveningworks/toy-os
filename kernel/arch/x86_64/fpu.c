// See kernel/include/kernel/fpu.h for the design writeup -- in
// particular why floating point is ring-3-only here, and why the
// save/restore is eager rather than the classic CR0.TS lazy scheme.
//
// Everything in this file is a control-register access or an FXSAVE, so
// it lives in arch/x86_64 by kernel/README.md's own test: a different
// CPU would need all of it rewritten, not adapted.
#include "fpu.h"
#include "string.h" // k_memcpy

#define CR0_MP (1u << 1)  // monitor coprocessor -- WAIT/FWAIT respects TS
#define CR0_EM (1u << 2)  // emulation: 1 = FP instructions #UD. Must be 0.
#define CR0_TS (1u << 3)  // task switched -- deliberately left 0, see fpu.h
#define CR0_NE (1u << 5)  // native x87 exceptions (#MF), not the legacy PIC FERR pin

#define CR4_OSFXSR     (1u << 9)   // FXSAVE/FXRSTOR available, and SSE enabled
#define CR4_OSXMMEXCPT (1u << 10)  // unmasked SIMD FP exceptions raise #XF

// CPUID.01H:EDX -- both architecturally guaranteed in long mode, read
// anyway (see fpu_init()'s doc comment).
#define CPUID_EDX_FXSR (1u << 24)
#define CPUID_EDX_SSE2 (1u << 26)

static int g_enabled = 0;

// The post-FNINIT state every new process starts from. Captured once by
// fpu_init() with a real FXSAVE rather than hand-written -- see
// fpu_init_state()'s comment on why hand-writing this bites.
static uint8_t g_pristine[FPU_STATE_SIZE] __attribute__((aligned(FPU_STATE_ALIGN)));

static inline uint64_t read_cr0(void) {
    uint64_t v;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(v));
    return v;
}

static inline void write_cr0(uint64_t v) {
    __asm__ volatile ("mov %0, %%cr0" :: "r"(v));
}

static inline uint64_t read_cr4(void) {
    uint64_t v;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(v));
    return v;
}

static inline void write_cr4(uint64_t v) {
    __asm__ volatile ("mov %0, %%cr4" :: "r"(v));
}

int fpu_init(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile ("cpuid"
                       : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                       : "a"(1));
    if (!(edx & CPUID_EDX_FXSR) || !(edx & CPUID_EDX_SSE2)) return 0;

    uint64_t cr0 = read_cr0();
    cr0 &= ~(uint64_t)(CR0_EM | CR0_TS); // execute FP for real; never lazy
    cr0 |= CR0_MP | CR0_NE;
    write_cr0(cr0);

    uint64_t cr4 = read_cr4();
    cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
    write_cr4(cr4);

    // FNINIT resets the x87 side; the MXCSR write resets the SSE side to
    // its architectural default (all exceptions masked, round-to-nearest).
    // FXSAVE then captures both as the template new processes copy.
    uint32_t mxcsr = 0x1F80;
    __asm__ volatile ("fninit");
    __asm__ volatile ("ldmxcsr %0" :: "m"(mxcsr));
    __asm__ volatile ("fxsave (%0)" :: "r"(g_pristine) : "memory");

    g_enabled = 1;
    return 1;
}

int fpu_enabled(void) { return g_enabled; }

void fpu_init_state(void *area) {
    k_memcpy(area, g_pristine, FPU_STATE_SIZE);
}

void fpu_save(void *area) {
    __asm__ volatile ("fxsave (%0)" :: "r"(area) : "memory");
}

void fpu_restore(const void *area) {
    __asm__ volatile ("fxrstor (%0)" :: "r"(area) : "memory");
}
