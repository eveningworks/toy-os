#ifndef KDEBUG_H
#define KDEBUG_H

// THE KERNEL DEBUGGER: a GDB remote-protocol stub in the kernel itself
// (kernel/debug/), so a real `gdb` can stop a bare-metal machine, set
// breakpoints and watchpoints, read and write memory and continue --
// Linux's KGDB, Windows' KD. docs/kdebug-design.md is the plan.
//
// OFF UNLESS `kdebug=ttySN` IS ON THE BOOT LINE, and nothing reachable
// from the running system can turn it on: whoever holds the port owns
// the machine. Every hook below is one load and a branch when unarmed.

#include <stdint.h>

// Parses `kdebug=ttySN[,wait]` and claims the port. `,wait` stops the
// boot here until a debugger attaches. Before serial_irq_init().
void kdebug_init(void);

// Armed at boot. The idle loop keeps its tick while this is true, since
// the tick is what notices a break-in.
int kdebug_armed(void);

// isr_dispatch()'s question for #DB, NMI and #BP: 1 means the debugger
// took the trap and `regs` is ready to resume; 0 means it was not ours.
int kdebug_trap(uint64_t vector, uint64_t *regs);

// A ring-0 fault about to panic: stop at the faulting frame first. The
// panic proceeds when the debugger continues or detaches.
void kdebug_fatal(uint64_t vector, uint64_t *regs);

// panic_finish()'s hook: stop once before a panic that had no trap
// frame (a stack-protector hit, boot_require()).
void kdebug_panic(void);

// The timer tick's hook: a ^C or a packet from an attaching debugger
// stops the kernel at `regs`, the frame the tick interrupted.
void kdebug_poll(uint64_t *regs);

#endif
