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
// completion. `heap_os_lock()` is a no-op in ring 0 BY DESIGN
// (heap_core.c), so a tick landing mid-kmalloc and switching to a
// process that also allocates corrupts the free list -- and
// bounce_alloc() is on every read and write. Several query providers
// and lib/tunables.c say "the kernel is single-threaded" in as many
// words. Those are what the roadmap's "Interruptible syscalls" item
// has to clear before the gate can become a trap gate; until then this
// still means "poll, do not block".
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
// beside `kernel_rsp` on the way out and restores it on the way in.
// Nothing else should call these: as a plain global the counter leaked
// to 348 the moment syscalls could be preempted, which left
// isr_in_progress() answering "yes" for the rest of the boot and pinned
// ata.c on its spin path.
int isr_depth_get(void);
void isr_depth_set(int depth);

// Point isr_common's epilogue at `rsp` instead of what it interrupted --
// the whole context-switch mechanism, and the only route to the live
// isr_dispatch() call's resume local. The slot accessors are the
// scheduler's: the pointer names a local on one context's kernel stack,
// so it travels with `kernel_rsp` exactly as the depth does.
void isr_resume_set(uint64_t rsp);
void *isr_resume_slot_get(void);
void isr_resume_slot_set(void *slot);

// Nominate the INCOMING context's (resume slot, depth) for a switch that
// has not taken effect yet. isr_dispatch() installs it after restoring
// its own, which is the only moment both are true of the right stack --
// see the pair's comment in idt.c.
void isr_context_defer(void *slot, int depth);

// What the live dispatch will restore on its way out -- the pair a
// context being switched AWAY from must be saved with, since its resume
// skips every dispatch tail. See idt.c.
void isr_context_outer(const uint64_t *regs, void **slot, int *depth);

// Spurious LAPIC interrupts seen since boot -- reported by `lsdev`
// beside the controller. See idt.c.
uint32_t idt_spurious_count(void);

#endif
