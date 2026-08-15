// RDSEED/RDRAND/RDTSC -- the three instructions kernel/lib/krandom.c is
// built on. See kernel/include/kernel/random_hw.h for why the API shape
// forces a caller to check for failure.
#include "random_hw.h"
#include "cpuinfo.h"

// The SDM's recommended retry count for RDRAND. RDSEED gets the same
// number here for simplicity; it is expected to run dry more often, and
// its caller is expected to fall back rather than insist.
#define RDRAND_RETRIES 10

int arch_rdseed64(uint64_t *out) {
    for (int i = 0; i < RDRAND_RETRIES; i++) {
        uint64_t v;
        uint8_t ok;
        // CF=1 means "a random value was placed in the destination".
        // On CF=0 the destination is architecturally ZERO, which is
        // exactly why *out is only written when ok.
        __asm__ volatile ("rdseed %0; setc %1" : "=r"(v), "=qm"(ok) :: "cc");
        if (ok) {
            *out = v;
            return 1;
        }
    }
    return 0;
}

int arch_rdrand64(uint64_t *out) {
    for (int i = 0; i < RDRAND_RETRIES; i++) {
        uint64_t v;
        uint8_t ok;
        __asm__ volatile ("rdrand %0; setc %1" : "=r"(v), "=qm"(ok) :: "cc");
        if (ok) {
            *out = v;
            return 1;
        }
    }
    return 0;
}

// CPUID.01H:ECX[30] and CPUID.07H:0:EBX[18] -- read through cpu_info
// rather than a private CPUID call here, so there is one place in this
// kernel that decides what the CPU supports (and one place a test can
// check, see cpuid_test.c). The bit numbers match cpu_features.h's
// "rdrand"/"rdseed" rows; if those ever disagree, that table is the one
// a human reads, so fix this to match it.
int arch_has_rdrand(void) {
    struct cpu_info ci;
    cpu_info_get(&ci);
    return cpu_has_feature(&ci, CPU_WORD_1_ECX, 30);
}

int arch_has_rdseed(void) {
    struct cpu_info ci;
    cpu_info_get(&ci);
    return cpu_has_feature(&ci, CPU_WORD_7_0_EBX, 18);
}

uint64_t arch_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
