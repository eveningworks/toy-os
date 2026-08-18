#ifndef KERNEL_KSTACK_H
#define KERNEL_KSTACK_H

#include <stdint.h>

// A kernel stack, with the three things that make an overflow of one
// survivable and diagnosable instead of a mystery in an unrelated
// process: a GUARD PAGE below it, a CANARY at its base, and a POISON
// fill so how deep it has ever been is measurable.
//
// There are two owners and they must not drift, which is why this is a
// header rather than a pattern each copies:
//
//   * kernel/proc/scheduler.c -- one per process slot.
//   * kernel/proc/process.c   -- one for the LEGACY blocking loader
//     (`run` / `config set` from the physical shell), which has no
//     scheduler slot and used to carry its own 8 KiB array with none of
//     the above. That is not hypothetical: the same deep path
//     (setting -> etc_config -> VFS -> TFS3 journal -> ATA) that
//     overflowed a process's stack also overflows this one, so `config
//     set cursor_size normal` at the shell double-faulted the kernel.
//
// SIZE: 16 KiB, matching Linux's THREAD_SIZE on x86-64 and Windows'
// x64 kernel stack. Depth here is many medium frames rather than one
// big one -- see docs/decisions.md, and CFLAGS' -Wframe-larger-than,
// which is the other half of keeping it that way.
#define KSTACK_BYTES        16384
#define KSTACK_GUARD_BYTES  4096

// Written at the very bottom and checked at every context switch --
// Linux's STACK_END_MAGIC. It covers what the guard page cannot: a
// frame big enough to STEP OVER the guard and land in the neighbour.
// Arbitrary, and deliberately not a plausible pointer, length or ASCII.
#define KSTACK_MAGIC 0x57ACC0DE0BADF00DULL

// What the unused part is filled with, so kstack_used() can measure the
// high-water mark. Linux's CONFIG_DEBUG_STACK_USAGE.
#define KSTACK_POISON 0xAA

// Page-aligned, and the guard FIRST: a stack grows down, so the guard
// has to be the thing below it.
struct kstack {
    uint8_t guard[KSTACK_GUARD_BYTES];
    uint8_t stack[KSTACK_BYTES];
} __attribute__((aligned(4096)));

static inline uint64_t kstack_base(const struct kstack *k) {
    return (uint64_t)(uintptr_t)&k->stack[0];
}

static inline uint64_t kstack_top(const struct kstack *k) {
    return (uint64_t)(uintptr_t)&k->stack[KSTACK_BYTES];
}

// Lays down the canary and poisons the rest, up to `reserve_top` bytes
// left untouched at the top (a synthesized trapframe the caller has
// already written there). Resets `*peak`.
void kstack_arm(struct kstack *k, uint32_t reserve_top, uint32_t *peak);

// Is the canary intact? A 0 means something has already written outside
// this stack, so nothing about the owning process can be trusted.
int kstack_canary_ok(const struct kstack *k);

// The high-water mark in bytes. `*peak` is the caller's running maximum
// and is updated; the scan walks DOWN from it, so it touches only what
// is new since the last call rather than the whole unused remainder.
uint32_t kstack_used(const struct kstack *k, uint32_t *peak);

// Makes the guard page unmapped. Must run AFTER paging_enforce_wx(),
// which rewrites every PDE. Returns 0 if the identity map could not be
// split -- in which case the stack still works and simply is not
// guarded, so callers report the count rather than assuming.
int kstack_guard_arm(struct kstack *k);

// Does `addr` fall in this stack's guard page? The fault reporter asks,
// so an overflow is named as one instead of printed as an anonymous
// #PF (or, worse, a bare "Double fault") somewhere in the kernel.
int kstack_guard_contains(const struct kstack *k, uint64_t addr);

#endif
