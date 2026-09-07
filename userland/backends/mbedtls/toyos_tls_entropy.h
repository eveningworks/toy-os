#ifndef TOYOS_TLS_ENTROPY_H
#define TOYOS_TLS_ENTROPY_H

// How good is this machine's randomness, asked before a key is drawn
// from it.
//
// Separate from `SYS_GETRANDOM`, which always answers and deliberately
// never grades its answer. See toyos_platform.c for why keying a TLS
// connection is the one case where the grade has to be looked at.
#include <stddef.h>

enum toyos_entropy_grade {
    TOYOS_ENTROPY_NONE = 0, // krandom_init() has not run; a fixed sequence
    TOYOS_ENTROPY_WEAK,     // TSC jitter -- software timing software, under QEMU
    TOYOS_ENTROPY_GOOD,     // virtio-rng or RDSEED/RDRAND
    TOYOS_ENTROPY_UNKNOWN,  // the query failed; treat as no better than WEAK
};

// Fills `name` with the source's own description ("TSC jitter",
// "hardware (RDSEED/RDRAND)") when `cap` is non-zero, so a caller can
// say WHICH source it is refusing rather than only that it refused.
enum toyos_entropy_grade toyos_entropy_grade(char *name, size_t cap);

#endif
