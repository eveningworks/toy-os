// FS.base, the thread pointer. See kernel/include/kernel/tls.h.
#include "tls.h"

#define MSR_IA32_FS_BASE 0xC0000100u

void arch_set_fs_base(uint64_t base) {
    __asm__ volatile ("wrmsr"
                      :: "c"(MSR_IA32_FS_BASE),
                         "a"((uint32_t)base),
                         "d"((uint32_t)(base >> 32)));
}

uint64_t arch_get_fs_base(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(MSR_IA32_FS_BASE));
    return ((uint64_t)hi << 32) | lo;
}
