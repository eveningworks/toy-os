// M16 scheduler demo (see kernel/proc/scheduler.c). A tiny freestanding
// ring-3 program that proves preemptive, non-cooperative scheduling:
// prints 'A' a fixed number of times, each followed by a long busy-spin
// (spanning several 100Hz timer ticks), then exits. Run alongside
// counter_b.c (which does the same with 'B') via the shell's `schedtest`
// command. There is no yield syscall anywhere in this project -- if the
// output interleaves on screen instead of printing all 20 As followed
// by all 20 Bs, the ONLY possible explanation is the timer preempting
// one process mid-spin and handing the CPU to the other.
#include <stdint.h>
#include "sys.h"







// A global (not a stack local) so it's definitely inside a PT_LOAD
// segment elf.c mapped -- same reasoning as write_test.c's approach.
static char out_char = 'A';

#define ITERATIONS  20
#define SPIN_ITERS  30000000u // long enough to span several 10ms slices

int main(void) {
    for (int i = 0; i < ITERATIONS; i++) {
        sys_call(SYS_WRITE, 1, (uint64_t)(uintptr_t)&out_char, 1);
        for (volatile uint32_t j = 0; j < SPIN_ITERS; j++) { }
    }
    sys_exit(0);
}
