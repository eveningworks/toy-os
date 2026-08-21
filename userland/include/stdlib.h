#ifndef USERLAND_LIB_STDLIB_H
#define USERLAND_LIB_STDLIB_H

// malloc/free for ring 3, under the C names.
//
// NOT a second allocator: this is the kernel's own
// kernel/lib/heap_core.c compiled a second time into libuapp.a, with
// SYS_SBRK behind it instead of the physical frame allocator (see
// api/heap_os.h). So a ring-3 malloc and the kernel's kmalloc cannot
// disagree about splitting, coalescing or double frees -- the same rule
// that already makes ring-3 `strlen` and `k_strlen` one implementation.
//
// It also means `heap debug on`'s machinery is here: red-zones each
// side of the payload and poison on free, catching an overflow or a
// use-after-free at the free() that follows it.
//
// **What it does NOT do is return memory to the kernel.** sbrk cannot
// move down, so free() returns a block to this process's free list and
// the process's footprint never shrinks. See userland/lib/heap_os.c.
//
// Deliberately absent, as everywhere else in this half-libc: no
// `realloc` (Stage 3 of docs/libc-design.md), no `aligned_alloc`, and
// no `getenv` -- which would always answer NULL, because crt0 receives
// an envp of exactly NULL and SYS_SPAWN has nowhere to put one. A
// getenv() that cannot ever find anything is worse than its absence.
#include <stddef.h>
#include <heap.h> // the toolkit allocator this header renames

// --- leaving ---------------------------------------------------------
//
// exit() runs the atexit handlers in reverse order of registration and
// then FLUSHES EVERY STREAM. crt0 calls it when main() returns, which
// is what makes a buffered printf() reliable; a program that calls
// sys_exit() (rt/sys.h) directly bypasses both.
void exit(int code) __attribute__((noreturn));
// Returns 0, or -1 if the handler is NULL or the table (32 entries, C's
// minimum) is full.
int  atexit(void (*fn)(void));
// Exits with 134 (128 + SIGABRT) WITHOUT flushing the buffered streams:
// abort() means the state is not to be trusted, and committing a
// half-written file is worse than losing it. stderr is unbuffered, so
// anything already reported is already out.
void abort(void) __attribute__((noreturn));

// The same contract as kmalloc(): NULL for a zero-sized or unsatisfiable
// request, 16-byte aligned otherwise.
static inline void *malloc(size_t n) { return kmalloc(n); }

// Zeroed. calloc()'s two arguments are multiplied here WITHOUT an
// overflow check on purpose being a thing to notice: n * size can wrap,
// and a wrapped product allocates a small block for a huge request.
// Checked below rather than trusted.
static inline void *calloc(size_t n, size_t size) {
    if (n && size && n > (size_t)-1 / size) return 0; // would wrap
    return kzalloc(n * size);
}

static inline void free(void *p) { kfree(p); }

#endif
