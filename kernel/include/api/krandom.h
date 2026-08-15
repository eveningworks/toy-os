#ifndef KRANDOM_H
#define KRANDOM_H

#include <stdint.h>
#include <stddef.h>

// The kernel's entropy source (Milestone 2, docs/roadmap.md) -- the gap
// that blocked both a randomised stack canary and kernel ASLR, since
// this kernel previously had no source of unpredictability at all.
//
// **This is not a CSPRNG, and the API says so out loud.** It is a
// hardware source when the CPU has one, a timing-jitter harvest when it
// does not, and a strong 64-bit mixing function over whichever it got.
// There is no entropy accounting, no reseed schedule and no
// backtracking resistance, because none of those can be made honest on
// top of a jitter source measured inside an emulator. What there IS is
// krandom_quality(), so a caller that needs to know how much to trust
// its bytes can ask instead of assuming -- the same "a setting says
// whether it actually persisted" instinct as enum setting_result.
enum krandom_quality {
    KRANDOM_NONE = 0,   // krandom_init() has not run; output is a fixed sequence
    KRANDOM_JITTER,     // TSC jitter only -- unpredictable in principle, WEAK
                        // under an emulator, where the "hardware" timing this
                        // measures is itself software (see krandom.c)
    KRANDOM_HW,         // RDSEED/RDRAND -- the CPU's own entropy source
};

// Seeds the pool. Call once from kernel_main(), AFTER idt_init() and
// cpu_info_init(): the CPUID feature check needs the latter, and the
// jitter fallback needs the PIT ticking to have something to beat
// against. Safe to call again (re-seeds; a caller mid-way through
// krandom_bytes() is not a case that exists, since nothing here blocks).
void krandom_init(void);

// A 64-bit random value. Never fails -- when there is no entropy at
// all it still returns a well-mixed value, it just isn't unpredictable,
// which is what krandom_quality() is for.
uint64_t krandom_u64(void);

// Fills `buf` with `n` random bytes. Handles any n, including a tail
// shorter than 8 bytes.
void krandom_bytes(void *buf, size_t n);

// Which source the values are actually coming from.
enum krandom_quality krandom_quality(void);

// "hardware (RDSEED/RDRAND)" / "TSC jitter" / "none". Never NULL, so a
// caller can print it without a null check.
const char *krandom_quality_name(enum krandom_quality q);

#endif
