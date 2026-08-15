#ifndef KERNEL_RANDOM_HW_H
#define KERNEL_RANDOM_HW_H

#include <stdint.h>

// The CPU's own randomness instructions, and the timestamp counter the
// fallback is built out of. Lives in arch/x86_64/ rather than beside
// kernel/lib/krandom.c because these are three inline-asm instructions
// and nothing else -- kernel/README.md's rule is that a different CPU
// would rewrite this file and leave the service above it alone.
//
// RDSEED and RDRAND are NOT interchangeable, and the difference is the
// reason both are here. RDRAND is the output of a DRBG that the CPU
// reseeds from its entropy source; RDSEED is that source's output
// directly. So RDSEED is what you want to SEED something with (it is
// what Intel documents for exactly that), and it is also the one that
// legitimately fails more often, because it can only hand out entropy
// as fast as the hardware collects it. Preferring RDSEED and falling
// back to RDRAND is the documented order.
//
// Both return 0 on failure rather than a value, and a caller must check
// it: the instructions report "no random number was available" in CF,
// and the destination register is defined to be ZERO in that case. A
// caller that ignores the flag therefore doesn't get a weak random
// number, it gets a hard zero, every time, silently. That is the trap
// this API shape exists to make unmissable -- there is no variant that
// returns the value directly.

// 1 and *out set, or 0 and *out untouched. Retries internally: the
// Intel SDM's own guidance is ten attempts for RDRAND, and RDSEED is
// expected to fail more often than that, so its caller should treat a
// failure as normal and fall back rather than as a broken CPU.
int arch_rdseed64(uint64_t *out);
int arch_rdrand64(uint64_t *out);

// Whether the CPU says it has each instruction (CPUID). Calling either
// of the above without checking is an invalid opcode, not a failure
// return, on a CPU that lacks it.
int arch_has_rdseed(void);
int arch_has_rdrand(void);

// The timestamp counter, unserialised. This is the raw material for the
// jitter fallback and is not random by itself -- see krandom.c.
uint64_t arch_rdtsc(void);

#endif
