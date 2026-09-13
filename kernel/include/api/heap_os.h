#ifndef API_HEAP_OS_H
#define API_HEAP_OS_H

#include <stdint.h>

// The three things the allocator deliberately does not know.
//
// kernel/lib/heap_core.c is the free-list allocator -- first fit,
// address-ordered, coalescing, with red-zones and use-after-free
// poisoning behind a runtime toggle. It is compiled TWICE: once into
// the kernel as kmalloc()/kfree(), and once into libuapp.a as ring 3's
// malloc()/free(). That is the repo's shared-source rule (see geom.c,
// kfmt.c, etc_config.c), and the reason is the usual one -- two
// allocators would drift, and an allocator is the worst place for two
// implementations to disagree.
//
// Everything platform-specific is these three functions. Each side
// provides its own: kernel/mm/heap_os.c and userland/lib/heap_os.c.
//
// **The state is per COMPILATION UNIT, not shared.** heap_core.c's free
// list lives in its own statics, so the kernel's heap and a process's
// heap are separate instances of the same code -- which is exactly
// right, since they are separate address spaces and one must never hand
// out the other's memory.

// One contiguous run of at least `bytes`, or NULL. The allocator asks
// for this only when its free list cannot satisfy a request, and never
// gives any of it back -- neither side has a shrink path (the kernel's
// pmm regions are permanent, and sbrk cannot move down).
//
// The kernel returns identity-mapped physical frames; ring 3 returns
// sbrk'd address space, whose PAGES do not exist until touched. The
// allocator writes a block header into the region immediately, so ring
// 3's first touch happens right here.
void *heap_os_alloc(uint64_t bytes);

// Where a corruption report goes: the kernel log, or stderr. Takes
// finished text -- the allocator formats with k_snprintf so it needs no
// opinion about the sink.
void heap_os_report(const char *msg);

// Fault injection for the kernel's tests (fault_inject.h), so every
// caller's out-of-memory path can be exercised without exhausting the
// machine. Ring 3 always answers 0; there is no injector there yet.
int heap_os_should_fail_alloc(void);

// MUTUAL EXCLUSION OVER THE FREE LIST, and the fourth thing the
// allocator deliberately does not know: what a "thread" is on this side.
//
// heap_core.c holds ONE address-ordered list, and a walker that is
// interrupted halfway through a split or a coalesce leaves it
// inconsistent -- so any ring with two concurrent callers needs these
// to be real. The two rings answer differently, and both answers are
// correct for their ring:
//
//   - **Ring 3: a real lock.** A process can have several threads
//     (`docs/conventions/kernel.md`) and they are preempted at any
//     instruction, so two of them in malloc() would corrupt the list.
//   - **Ring 0: preemption off**, not a lock. It was a no-op until the
//     syscall gate stopped running with interrupts masked; see
//     kernel/mm/heap_os.c, which also records why masking interrupts
//     is not needed and what would change that. Formerly a no-op because
//     the kernel was not preempted inside
//     kernel code -- the scheduler only ever switches ring-3 processes,
//     and every syscall runs with interrupts off. That assumption is
//     stated in heap_core.c's top comment and ENDS AT SMP: the kernel
//     heap is split #1 in `docs/smp-design.md`, and this is the slot
//     that gets filled in.
//
// NOT RECURSIVE, and malloc() is therefore not async-signal-safe -- a
// signal handler that allocates while its own thread holds this lock
// deadlocks. That is true of every libc's malloc, glibc's included.
void heap_os_lock(void);
void heap_os_unlock(void);

#endif
