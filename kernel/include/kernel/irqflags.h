#ifndef IRQFLAGS_H
#define IRQFLAGS_H

// Interrupts off on THIS CPU, and back to how they were. Enough to keep
// a handler out of a critical section while one CPU runs everything
// (docs/smp-design.md: no application processor runs yet); NOT a lock
// against another CPU. Several files still carry their own copy of
// these two -- they move here as they are touched.
#include <stdint.h>

static inline uint64_t irq_save(void) {
    uint64_t f;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    return f;
}

static inline void irq_restore(uint64_t f) {
    __asm__ volatile ("pushq %0; popfq" :: "r"(f) : "memory", "cc");
}

#endif
