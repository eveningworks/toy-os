// KTESTs for the FP-state plumbing (see fpu.h).
//
// Scope note, because the gap matters: these cover the half that can be
// tested from inside the kernel -- that SSE actually got enabled, and
// that a fresh state area is the pristine one rather than garbage. They
// do NOT cover save/restore across a context switch, and can't: the
// kernel is built `-mno-sse` and has no FP state of its own to preserve,
// which is the entire design (fpu.h). That half is proved from ring 3
// instead, by two /tests/fpu_race processes racing each other under the
// scheduler -- run `fputest` from the shell.
#include "fpu.h"
#include "ktest.h"
#include "string.h"

#define CR4_OSFXSR     (1u << 9)
#define CR4_OSXMMEXCPT (1u << 10)
#define CR0_EM         (1u << 2)
#define CR0_TS         (1u << 3)

// FXSAVE area layout, the two fields worth asserting on (Intel SDM Vol.
// 1, "FXSAVE Area Layout"): the x87 control word at offset 0 and MXCSR
// at offset 24.
#define FXSAVE_OFF_FCW   0
#define FXSAVE_OFF_MXCSR 24

static uint64_t cr0(void) {
    uint64_t v;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(v));
    return v;
}

static uint64_t cr4(void) {
    uint64_t v;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(v));
    return v;
}

static uint16_t read_u16(const uint8_t *p, int off) {
    return (uint16_t)(p[off] | ((uint16_t)p[off + 1] << 8));
}

KTEST("fpu", "enabled at boot") {
    KTEST_ASSERT(fpu_enabled());
}

KTEST("fpu", "control registers configured for SSE") {
    // EM must be clear or every FP instruction #UDs; OSFXSR must be set
    // or FXSAVE itself doesn't exist.
    KTEST_ASSERT((cr0() & CR0_EM) == 0);
    KTEST_ASSERT((cr4() & CR4_OSFXSR) != 0);
    KTEST_ASSERT((cr4() & CR4_OSXMMEXCPT) != 0);
}

KTEST("fpu", "TS stays clear -- eager, never lazy") {
    // If this ever fails, someone has reintroduced lazy FPU switching.
    // See fpu.h: that's the scheme CVE-2018-3665 exploited, and the one
    // Linux deleted in 4.14.
    KTEST_ASSERT((cr0() & CR0_TS) == 0);
}

KTEST("fpu", "fresh state is the architectural default") {
    uint8_t area[FPU_STATE_SIZE] __attribute__((aligned(FPU_STATE_ALIGN)));
    k_memset(area, 0xAB, sizeof(area)); // poison first -- a no-op
                                         // fpu_init_state() must not pass
    fpu_init_state(area);

    // 0x037F: all exceptions masked, 64-bit precision, round to nearest.
    KTEST_ASSERT_EQ(read_u16(area, FXSAVE_OFF_FCW), 0x037F);
    // 0x1F80: all SIMD exceptions masked, round to nearest, FTZ off.
    KTEST_ASSERT_EQ(read_u16(area, FXSAVE_OFF_MXCSR), 0x1F80);
}

KTEST("fpu", "save/restore round-trips a state area") {
    uint8_t a[FPU_STATE_SIZE] __attribute__((aligned(FPU_STATE_ALIGN)));
    uint8_t b[FPU_STATE_SIZE] __attribute__((aligned(FPU_STATE_ALIGN)));

    // Both zeroed first, and that is NOT boilerplate. FXSAVE does not
    // write all 512 bytes: the tail (offset 464 onward) is "available
    // for software" and the CPU leaves it exactly as it found it, as do
    // several reserved fields. Comparing two uninitialized buffers
    // therefore compares leftover stack garbage, which is how this test
    // failed the first time it ran. Zeroing both means every byte
    // FXSAVE skips is equal by construction and only the bytes it
    // actually writes are under test.
    //
    // Same property is why fpu_init_state() memcpy()s a full 512-byte
    // template instead of doing an FXSAVE into each new process's area.
    k_memset(a, 0, sizeof(a));
    k_memset(b, 0, sizeof(b));

    // Save the live state twice with a restore in between: whatever the
    // CPU currently holds must survive the round trip byte-for-byte.
    // This is the exact sequence scheduler.c performs across a switch,
    // minus the switch.
    fpu_save(a);
    fpu_restore(a);
    fpu_save(b);
    KTEST_ASSERT_EQ(k_memcmp(a, b, FPU_STATE_SIZE), 0);
}
