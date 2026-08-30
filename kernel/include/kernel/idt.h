#ifndef IDT_H
#define IDT_H

#include <stdint.h>

struct interrupt_frame;

void idt_init(void);
void idt_set_gate(uint8_t vector, void (*handler)(void), uint8_t ist, uint8_t type_attr);

// Installs a callback invoked when a CPU exception (vector < 32) occurs
// while the faulting code was running in ring 3 (CS RPL == 3) -- after
// the generic "KERNEL PANIC" banner and register dump are printed, but
// before the machine halts. Pass NULL to remove. This exists so a
// ring-3 test/demo can print its own diagnostics without idt.c needing
// to know anything about it.
typedef void (*ring3_fault_hook_fn)(uint64_t vector, uint64_t error_code, uint64_t cs, uint64_t cr2);
void idt_set_ring3_fault_hook(ring3_fault_hook_fn hook);

// True while isr_dispatch() is anywhere on the current call stack --
// i.e. we're inside a hardware-IRQ handler OR a syscall (int 0x80),
// both of which run with interrupts disabled (IF=0, an interrupt
// gate's automatic behavior -- see idt_init()'s idt_set_gate() calls).
// Exists so a driver that wants to genuinely block-with-interrupts-on
// (build 430's ata.c, waiting on a DMA-completion IRQ) can tell whether
// doing so here would be safe.
//
// It is NOT safe to `sti` then block (`hlt` or otherwise) when this
// returns true. g_next_kernel_rsp (this file) is a single global
// "where to resume" pointer, unconditionally overwritten by every
// isr_dispatch() call including a nested one -- see the SYS_READ_KEY
// comment in syscall.c for the exact failure this caused the one time
// it was tried anyway (worked for one keystroke, then hung). Fixing
// that reentrancy is its own separate, not-yet-done item (README.md's
// "Ideas for what's next") -- this function exists so callers can
// route AROUND the hazard instead (poll instead of block) rather than
// trip over it.
int isr_in_progress(void);

// Resets the isr_in_progress() depth counter to 0. Only meaningful
// caller: process_run_ring3() (process.c), right after resuming via its
// "not the first return" branch (process_context_exit()/
// process_context_recover()'s longjmp-style restore) -- that jump
// abandons whatever C call stack was in flight, isr_dispatch() frames
// included, so their matching depth decrements never run. This is the
// one place in this kernel where "we're definitely back at a known,
// non-interrupt call site" is actually true, so it's the one safe place
// to force the counter back to a known-good value instead of trusting
// the (now unreachable) decrements that should have run.
void isr_reset_depth(void);

// Spurious LAPIC interrupts seen since boot -- reported by `lsdev`
// beside the controller. See idt.c.
uint32_t idt_spurious_count(void);

#endif
