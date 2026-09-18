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
// returns true -- but the REASON changed, and the old one is worth not
// leaving here to mislead. It used to be the resume pointer: a nested
// isr_dispatch() overwrote the outer handler's `g_next_kernel_rsp`, so
// its epilogue returned into a frame that had already been popped (a
// blocking keyboard syscall was tried that way: it worked for one
// keystroke, then hung). That is FIXED -- idt.c's isr_dispatch()
// wrapper keeps the value per call, on the C stack.
//
// What is still unsafe is everything that assumed a syscall runs to
// completion -- but the list is shorter than it was, and the examples
// this comment used to give are DONE. `heap_os_lock()` is real in ring
// 0 now (preemption-off, kernel/mm/heap_os.c), so a tick landing
// mid-kmalloc no longer hands the free list to a second walker, and
// query.c guards its providers at the one place every caller passes
// through. What actually holds the trap gate today is measured and
// different: it makes desktop latency under disk I/O twenty times
// WORSE, because `FS_OP()` still holds preemption off for a whole
// backend call -- see docs/blocking-design.md. Until a caller can
// yield inside one, this still means "poll, do not block".
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

// THE DEPTH TRAVELS WITH THE CONTEXT, and these are the scheduler's
// half of that. A context switch changes whose kernel stack
// isr_in_progress() is describing, so scheduler.c saves the depth
// into the outgoing context on the way out and reloads the incoming
// one's on the way in.
// Nothing else should call these: as a plain global the counter leaked
// to 348 the moment syscalls could be preempted, which left
// isr_in_progress() answering "yes" for the rest of the boot and pinned
// ata.c on its spin path.
int isr_depth_get(void);
void isr_depth_set(int depth);

// Spurious LAPIC interrupts seen since boot -- reported by `lsdev`
// beside the controller. See idt.c.
uint32_t idt_spurious_count(void);

#endif
