// Ring 3's half of the shared allocator -- see api/heap_os.h.
//
// Memory comes from SYS_SBRK, which is the only allocator-adjacent
// syscall this OS has. Two things follow, and both are visible to a
// caller of malloc():
//
// **Nothing is ever returned to the kernel.** sbrk only moves up, so
// free() returns a block to this process's own free list and the break
// stays where it was. A process that allocates 500 MiB and frees it all
// still holds 500 MiB. That is what early Unix did, and it is why
// mmap()/munmap() exists on real systems -- see docs/roadmap.md's
// demand-paging milestone, where malloc switching its backing to mmap
// is one function.
//
// **The pages are not real yet.** sbrk reserves address space; the
// frame arrives on first touch. The allocator writes a block header
// into a fresh region immediately, so the first page of every region
// faults in here -- and the rest arrive as the program actually uses
// them. A process that reserves far more than the machine has finds out
// at the page it cannot be given, not at the malloc().
#include "heap_os.h"
#include "rt/sys.h"

void *heap_os_alloc(uint64_t bytes) {
    // sbrk hands back the OLD break, which is the start of what was
    // just claimed -- exactly the region the allocator wants.
    void *p = sys_sbrk((int64_t)bytes);
    return (p == (void *)-1) ? 0 : p;
}

// stderr, not stdout: a GUI client's stdout goes nowhere useful and a
// spawned process's goes into its parent's pipe, while stderr reaches
// the kernel log and `dmesg` where a test can read it. Same reasoning
// as every other ring-3 diagnostic here.
void heap_os_report(const char *msg) { sys_eprint(msg); }

// No injector in ring 3 yet. The kernel's tests cover the allocator's
// own out-of-memory path, and it is the same code.
int heap_os_should_fail_alloc(void) { return 0; }

// THE FREE LIST, AND WHY RING 3 NEEDS A LOCK WHERE THE KERNEL DOES NOT.
//
// A process can have threads now, and they are preempted at any
// instruction -- so two of them inside kmalloc() walk and rewrite one
// list. heap_core.c's own top comment predicted exactly this ("if a
// future caller ever needs kmalloc from ... a genuinely preemptible
// kernel thread, this needs a lock first"); threads are that caller,
// arriving on the ring-3 side.
//
// A SPIN THAT YIELDS, not a futex: there is nothing to park on yet
// (docs/roadmap.md). Correct under a preemptive round-robin scheduler,
// because the holder is always eventually run, and cheap in the case
// that matters -- an uncontended acquire is one atomic exchange, which
// is what every single-threaded program in this system pays.
//
// The acquire/release ordering is not decoration on x86-64's TSO: the
// COMPILER is what would otherwise hoist a list read out of the
// critical section.
static volatile int g_heap_lock;

void heap_os_lock(void) {
    while (__atomic_exchange_n(&g_heap_lock, 1, __ATOMIC_ACQUIRE))
        sys_yield();
}

void heap_os_unlock(void) {
    __atomic_store_n(&g_heap_lock, 0, __ATOMIC_RELEASE);
}
