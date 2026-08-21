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
// Deliberately absent: `aligned_alloc` (nothing needs an alignment
// stronger than the allocator's 16), and `getenv` -- which would always
// answer NULL, because crt0 receives an envp of exactly NULL and
// SYS_SPAWN has nowhere to put one. A getenv() that cannot ever find
// anything is worse than its absence.
#include <stddef.h>
#include <heap.h> // the toolkit allocator this header renames

// --- numbers from text -----------------------------------------------
//
// NOT wrappers over knum.h's k_parse_* family, and the reason is the
// contract rather than the arithmetic: k_parse_u64() answers "did the
// WHOLE string parse", which is what a config file wants, while
// strtol() must stop at the first byte it cannot use and hand that
// position back. A parser built on the first cannot be built out of the
// second without re-scanning, and vice versa. Both are correct; they
// answer different questions.
//
// `base` is 0 or 2..36. Base 0 means C's own literal rules: leading
// "0x" is hex, a leading "0" is octal, anything else decimal.
//
// On overflow the result SATURATES (LONG_MAX/LONG_MIN, ULONG_MAX) and
// errno is set to ERANGE, which is what C requires -- and is why a
// caller that cares must clear errno first: a successful call does not
// clear it.
//
// `endptr`, when not NULL, receives the first unconsumed byte. If no
// conversion was possible it receives `nptr` itself, so a caller can
// tell "0" from "not a number" -- both return 0.
long           strtol(const char *nptr, char **endptr, int base);
unsigned long  strtoul(const char *nptr, char **endptr, int base);
// atoi()/atol() are strtol() with the errors thrown away, which is
// exactly what C says they are. They cannot report anything, so reach
// for strtol() in new code.
int            atoi(const char *s);
long           atol(const char *s);

// --- arithmetic -------------------------------------------------------

int   abs(int v);
long  labs(long v);

// --- sorting and searching --------------------------------------------
//
// **qsort IS NOT STABLE**, and this matters here because both existing
// sorts in this tree deliberately are: uui_table keeps a previous
// column's order within ties (the behaviour every desktop table has)
// and dirsort tie-breaks by name. Neither can be replaced by this, and
// neither should be -- qsort is here because a C library has one, not
// because anything in toy-os was waiting for it.
//
// The comparator returns <0, 0 or >0, as C's does.
void  qsort(void *base, size_t n, size_t size,
            int (*cmp)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t n, size_t size,
              int (*cmp)(const void *, const void *));

// --- pseudo-random ----------------------------------------------------
//
// **NOT the kernel's entropy source, and not a substitute for it.**
// This is C's rand(): a deterministic sequence from a seed, for
// shuffling and sampling. Anything that must be unguessable asks
// sys_getrandom() (rt/sys.h), which is krandom.h's real generator and
// reports its own quality.
//
// The generator is the one C99 itself prints as an example, so a
// sequence is reproducible from a seed across any implementation that
// copies the standard's -- which is what makes a seeded test repeatable.
#define RAND_MAX 32767
int   rand(void);
void  srand(unsigned seed);

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

// Grows or shrinks a block, COPYING every time it moves -- there is no
// grow-in-place path, because the shared allocator has no call for
// "extend this block if the next one is free" and adding one to satisfy
// a single caller is not the bar this project holds. realloc(NULL, n)
// is malloc(n) and realloc(p, 0) frees and returns NULL, as C says.
//
// It cannot make the process smaller either way: free() returns a block
// to this process's own list and sbrk never moves down.
void *realloc(void *p, size_t n);

#endif
