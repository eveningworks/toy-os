#ifndef KASAN_H
#define KASAN_H

#include <stddef.h>
#include <stdint.h>

// KASAN, the kernel address sanitizer (`make KASAN=1`): GCC's
// -fsanitize=kernel-address, Linux's generic mode. Every load and store
// the compiler instruments is checked against a SHADOW byte saying how
// much of that 8-byte granule may be touched right now; the heap, the
// frame allocator, stack arrays and globals keep the shadow current.
//
// THE SHADOW: shadow(a) = (a >> 3) + KASAN_SHADOW_OFFSET, for every
// kernel address (the identity map, below 512 GiB). That range is top-
// level page-table slot KASAN_PML4_INDEX, shared by every address space
// and supervisor-only. It is mapped to one zero page before any
// instrumented code runs (kasan_early_init), and to real pages over
// usable RAM once the frame allocator exists (kasan_init).
//
// A shadow byte: 0 = all 8 bytes accessible; 1..7 = only the first k;
// negative = none, and WHICH negative says why (the KASAN_* kinds below
// and GCC's stack ones), which is how a report names its bug.
//
// Everything here compiles in every build. In one without KASAN the
// poison calls do nothing, so their callers need no #ifdef.

// Must match the Makefile's -fasan-shadow-offset; kasan.c asserts it.
#define KASAN_SHADOW_OFFSET 0xFFFFC00000000000ULL
#define KASAN_PML4_INDEX    384
#define KASAN_GRANULE       8
// The kernel map is PML4 slot 0, so this is what the shadow covers.
#define KASAN_COVERED_BYTES (512ULL << 30)

// Shadow values. GCC writes the stack ones itself (0xF1-0xF8).
#define KASAN_PAGE_FREE     0xFF  // a frame the allocator holds free
#define KASAN_HEAP_REDZONE  0xFC  // around a kmalloc block, and its header
#define KASAN_HEAP_FREE     0xFB  // a freed block, held in quarantine
#define KASAN_HEAP_UNUSED   0xFA  // heap space never handed out
#define KASAN_GLOBAL_REDZONE 0xF9

#ifdef TOYOS_KASAN
void kasan_poison(const void *addr, size_t size, uint8_t kind);
void kasan_unpoison(const void *addr, size_t size);
// A free the allocator refused -- a double free, or a pointer it never
// handed out -- reported like a bad access at the caller's `ip`.
void kasan_report_bad_free(const void *ptr, const char *what, uintptr_t ip);
// kasan_init()'s last step: checks start, over [0, limit).
void kasan_set_ready(uint64_t limit);
#else
static inline void kasan_poison(const void *a, size_t s, uint8_t k) { (void)a; (void)s; (void)k; }
static inline void kasan_unpoison(const void *a, size_t s) { (void)a; (void)s; }
#endif

// Boot, in this order: the zero-page shadow before any instrumented
// code (kernel_relocate_boot's first line); real shadow over RAM right
// after pmm_init(); then the globals' constructors. Nothing before
// kasan_init() is CHECKED, only allowed to write shadow.
void kasan_early_init(void);
void kasan_init(void);
void kasan_run_constructors(void);

// Clears stale stack poison below `sp` on the stack containing it --
// needed wherever frames are abandoned without returning (a longjmp-
// style resume), since only a RETURN unpoisons a frame's redzones.
void kasan_unpoison_stack_below(uint64_t sp);

// For a test that corrupts memory ON PURPOSE (the heap's own red-zone
// tests): checks are skipped between begin and end. Nests.
void kasan_suppress_begin(void);
void kasan_suppress_end(void);

// Reports since boot, and the most recent one's first line ("" before).
unsigned kasan_report_count(void);
const char *kasan_last_report(void);
// 1 when this is a KASAN=1 kernel with its shadow live.
int kasan_enabled(void);

// THE CHECK ITSELF, pure: `shadow` points at the shadow byte of
// granule (addr >> 3). Returns the first bad byte's offset from `addr`
// plus 1, or 0 when all `size` bytes are accessible. Separate so the
// KTESTs can run it over a fake shadow in any build.
size_t kasan_scan(const int8_t *shadow, uint64_t addr, size_t size);

#endif
