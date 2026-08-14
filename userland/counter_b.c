// M16 scheduler demo -- see counter_a.c for the full explanation. Same
// program, prints 'B' instead of 'A', so `schedtest`'s two processes are
// visibly distinguishable in the interleaved output.
#include <stdint.h>
#include "sys.h"







static char out_char = 'B';

#define ITERATIONS  20
#define SPIN_ITERS  30000000u

int main(void) {
    for (int i = 0; i < ITERATIONS; i++) {
        sys_call(SYS_WRITE, 1, (uint64_t)(uintptr_t)&out_char, 1);
        for (volatile uint32_t j = 0; j < SPIN_ITERS; j++) { }
    }
    sys_exit(0);
}
