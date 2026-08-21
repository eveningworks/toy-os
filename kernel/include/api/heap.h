#ifndef HEAP_H
#define HEAP_H

#include <stddef.h>
#include <stdint.h>

// A general-purpose KERNEL-space heap: kmalloc()/kfree(), built
// directly on top of pmm.h's physical frame allocator. See heap_core.c's
// top comment for how it's implemented (first-fit, address-ordered
// free list, coalescing on free) and why no separate page-table
// mapping step is needed to make a freshly allocated physical frame
// usable as a pointer (boot.asm identity-maps the whole low 4GiB for
// the kernel already).
//
// This is NOT exposed to ring-3 processes -- user-space allocation is
// the separate SYS_SBRK mechanism (process.c/echo_test.c), which grows
// a process's own private address space instead. kmalloc/kfree are for
// kernel-space code only; first real caller: the window manager's
// per-window app state for multi-instance apps (see gui_apps.h's
// `multi_instance` flag and apps/calculator.c) -- previously every
// app's state was a single static struct because there was nothing
// else to allocate it from.
//
// Not interrupt-safe or reentrant -- see heap_core.c's top comment.

void heap_init(void);

// Returns a pointer to at least `size` bytes, 16-byte aligned, or 0 if
// out of memory (pmm has no more frames to grow into). Uninitialized
// contents, same convention as the standard library's malloc().
void *kmalloc(size_t size);

// kmalloc() + zero-fills the returned block -- convenience for the
// common "allocate a fresh state struct" case (window_set_state()
// callers almost always want this, not raw kmalloc()).
void *kzalloc(size_t size);

// Frees a pointer previously returned by kmalloc()/kzalloc(). Safe
// no-op on NULL. Freeing a pointer this allocator didn't hand out, or
// double-freeing one, is undefined behavior -- same trust-the-caller
// contract as the rest of this kernel-space code (there's one trust
// domain here, not the ring0/ring3 boundary syscalls have to defend).
void kfree(void *ptr);

// The usable payload size of a live block, or 0 for NULL / a pointer
// this heap did not hand out / a block already freed.
//
// It exists because realloc() (ring 3's <stdlib.h>) must copy
// min(old, new) bytes and only the allocator knows the old size --
// copying `new` bytes from a shorter block walks off the end of the
// last block in a region into an unmapped page. Answering 0 for a
// pointer it does not recognise makes that copy degrade to nothing
// rather than to garbage.
//
// It is NOT a promise about how much was REQUESTED: an allocator is
// free to hand back more than was asked for, and a caller that writes
// into the difference is relying on this number rather than on its own.
uint64_t kmalloc_size(const void *ptr);

// Stats -- same shape as pmm_total_frames()/pmm_free_frames(), useful
// for a future meminfo-style command. `heap_total_bytes()` is every
// byte this allocator has ever claimed from pmm (used + free);
// `heap_used_bytes()` is what's currently handed out.
uint64_t heap_total_bytes(void);
uint64_t heap_used_bytes(void);

// ---- debug mode: red-zones and use-after-free poisoning ----
//
// A RUNTIME switch (`heap debug on` in the shell), not a build flag, so
// it is reachable in a booted OS and so one build exercises both
// states. Blocks allocated while it is on carry a canary on each side
// of the payload and are filled with a poison byte when freed; kfree()
// checks the canaries, and the next kmalloc() to reuse a freed block
// checks that the poison is intact -- which is what catches a write
// through an already-freed pointer.
//
// Toggling affects SUBSEQUENT allocations only. Blocks of both kinds
// coexist safely; see heap_core.c's top comment for the invariant that lets
// kfree() tell them apart, and why breaking it would fail silently.
//
// A detected violation is reported to the kernel log and the block is
// QUARANTINED -- permanently leaked rather than returned to the free
// list, because its metadata is exactly what proved untrustworthy.
// Nothing panics: that keeps the mechanism testable from a KTEST.
void heap_set_debug(int on);
int heap_debug(void);

// Verifies every poisoned free block right now and returns how many
// were damaged, instead of waiting for whatever allocation eventually
// reuses one -- which may be far away in time and in subsystem, or may
// never come. `heap check` in the shell.
uint64_t heap_check(void);

uint64_t heap_rz_checks(void);       // red-zone checks performed since boot
uint64_t heap_violations(void);      // violations detected (each one also logged)
// Block bytes withdrawn from circulation by those violations. Counted
// in NEITHER heap_used_bytes() nor the free total -- a quarantined
// block is deliberately stranded, so the two no longer sum to
// heap_total_bytes() once this is nonzero.
uint64_t heap_quarantined_bytes(void);

// Exercises kmalloc()/kzalloc()/kfree() (including coalescing) once and
// logs pass/fail via klog_write() -- same "prove it at boot" pattern as
// pmm_selftest(). Called once from kernel_main() right after
// heap_init().
int heap_selftest(void); // 1 = passed, 0 = failed (details logged) -- wrapped by a KTEST

#endif
