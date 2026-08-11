#ifndef HEAP_H
#define HEAP_H

#include <stddef.h>
#include <stdint.h>

// A general-purpose KERNEL-space heap: kmalloc()/kfree(), built
// directly on top of pmm.h's physical frame allocator. See heap.c's
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
// Not interrupt-safe or reentrant -- see heap.c's top comment.

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

// Stats -- same shape as pmm_total_frames()/pmm_free_frames(), useful
// for a future meminfo-style command. `heap_total_bytes()` is every
// byte this allocator has ever claimed from pmm (used + free);
// `heap_used_bytes()` is what's currently handed out.
uint64_t heap_total_bytes(void);
uint64_t heap_used_bytes(void);

// Exercises kmalloc()/kzalloc()/kfree() (including coalescing) once and
// logs pass/fail via klog_write() -- same "prove it at boot" pattern as
// pmm_selftest(). Called once from kernel_main() right after
// heap_init().
void heap_selftest(void);

#endif
