#ifndef GDT_H
#define GDT_H

#include <stdint.h>

// Segment selectors. Values are fixed by the layout gdt_init() builds --
// null, kernel code, kernel data, user data, user code, TSS (in that
// order; user data before user code is the conventional order some CPUs'
// fast syscall instructions assume, so it's kept even though we aren't
// using those yet).
#define SEL_KERNEL_CODE 0x08
#define SEL_KERNEL_DATA 0x10
#define SEL_USER_DATA   (0x18 | 3) // | 3 = RPL 3, required in the selector
#define SEL_USER_CODE   (0x20 | 3) // whenever it's used from ring 3
#define SEL_TSS         0x28

// Builds the GDT (kernel + user code/data segments) and the TSS, loads
// them (lgdt + segment reloads + ltr), and sets the TSS's RSP0 to a
// dedicated kernel stack. This MUST run before anything ever transitions
// to ring 3 -- without a valid TSS.RSP0, the first interrupt that occurs
// while running in ring 3 (even just the timer tick) has nowhere valid to
// switch to and the CPU triple-faults.
void gdt_init(void);

// Updates the TSS's RSP0 -- the kernel stack the CPU switches to on the
// next ring3->ring0 transition (interrupt/exception/syscall). M8-M15
// only ever needed one value here (kernel_stack0, set once by
// gdt_init()); M16's scheduler (scheduler.c) calls this on every
// process switch so that if the newly-running process is interrupted,
// its trapframe lands on ITS OWN dedicated kernel stack rather than
// clobbering another process's.
void gdt_set_kernel_stack(uint64_t rsp0);

#endif
