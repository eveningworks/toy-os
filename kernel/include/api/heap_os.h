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

#endif
