#ifndef TLS_H
#define TLS_H

#include <stdint.h>

// The thread pointer: FS.base, which is what %fs-relative addressing in
// ring 3 resolves against.
//
// Per THREAD, not per address space -- it is the one register-visible
// piece of state two threads of one process must not share, and the
// whole reason a `__thread` variable can exist at all. Restored by the
// scheduler on every switch (scheduler.c's switch_to), because nothing
// else does: iretq reloads CS and SS and leaves the hidden segment
// bases exactly as they were.
//
// Written through the MSR rather than WRFSBASE: that instruction needs
// CR4.FSGSBASE, which this kernel does not enable, and a ring-0 wrmsr
// costs nothing at a 100 Hz switch rate.
//
// Here in arch/ because it is two instructions of inline assembly and
// kernel/README.md says that lives nowhere else.
void arch_set_fs_base(uint64_t base);
uint64_t arch_get_fs_base(void);

#endif
