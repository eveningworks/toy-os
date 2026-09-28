// KASAN's runtime: the functions GCC's -fsanitize=kernel-address calls,
// the check behind them, and the report. See api/kasan.h for the shadow
// layout; the page tables that back it are kernel/mm/kasan_shadow.c.
//
// NEVER INSTRUMENTED (KASAN_EXCLUDE in the Makefile): every function
// here reads or writes shadow, and a checked access in here would call
// back into here.
//
// OUTLINE MODE (--param asan-instrumentation-with-call-threshold=0):
// every load and store is a call to __asan_{load,store}N_noabort, so
// the address filter is ours -- a user address, MMIO above RAM, or
// anything before the shadow is live is skipped rather than looked up.
// GCC's STACK redzones are the exception: it writes those itself, at
// the fixed offset, which is why the shadow has to exist from the first
// instrumented instruction (kasan_early_init).
#include <stddef.h>
#include <stdint.h>
#include "kasan.h"
#include "kfmt.h"
#include "klog.h"
#include "string.h"
#include "ksyms.h"
#include "reloc.h"
#include "idt.h"
#include "scheduler.h"

#ifdef TOYOS_KASAN

_Static_assert(KASAN_SHADOW_OFFSET == KASAN_SHADOW_OFFSET_CFG,
               "kasan.h and the Makefile disagree about the shadow offset");
_Static_assert(((KASAN_SHADOW_OFFSET >> 39) & 511) == KASAN_PML4_INDEX,
               "the shadow offset is not in KASAN_PML4_INDEX's slot");

static int g_ready;
static uint64_t g_limit;       // first address with no real shadow behind it
static int g_suppress;
static int g_reporting;
static unsigned g_count;
static char g_last[192];

// Sites already reported, by return address: one report per faulting
// instruction, as UBSAN does per source site. Open addressing; a full
// table stops recording rather than reporting forever.
#define SEEN_SLOTS 512
static uintptr_t g_seen[SEEN_SLOTS];

static inline int8_t *shadow_of(uint64_t a) {
    return (int8_t *)(uintptr_t)((a >> 3) + KASAN_SHADOW_OFFSET);
}

int kasan_enabled(void) { return g_ready; }
unsigned kasan_report_count(void) { return g_count; }
const char *kasan_last_report(void) { return g_last; }
void kasan_suppress_begin(void) { g_suppress++; }
void kasan_suppress_end(void) { if (g_suppress) g_suppress--; }

void kasan_set_ready(uint64_t limit) {
    g_limit = limit < KASAN_COVERED_BYTES ? limit : KASAN_COVERED_BYTES;
    g_ready = 1;
}

// --- shadow writes -----------------------------------------------------

// A START that is not granule-aligned is an object's END: the bytes
// before it stay accessible, so its granule records how many there are.
// A trailing partial granule is left alone -- the shadow can say "the
// first k bytes", never "the last k".
void kasan_poison(const void *addr, size_t size, uint8_t kind) {
    uint64_t a = (uint64_t)(uintptr_t)addr, end = a + size;
    if (!size || a >= KASAN_COVERED_BYTES) return;
    if (a & 7) {
        uint64_t next = (a | 7) + 1;
        if (next > end) return;
        *shadow_of(a) = (int8_t)(a & 7);
        a = next;
    }
    for (; a + 8 <= end; a += 8) *shadow_of(a) = (int8_t)kind;
}

void kasan_unpoison(const void *addr, size_t size) {
    uint64_t a = (uint64_t)(uintptr_t)addr & ~7ULL, end = (uint64_t)(uintptr_t)addr + size;
    if (!size || a >= KASAN_COVERED_BYTES) return;
    for (; a + 8 <= end; a += 8) *shadow_of(a) = 0;
    if (a < end) *shadow_of(a) = (int8_t)(end - a);
}

#endif // TOYOS_KASAN

// --- the check ---------------------------------------------------------

size_t kasan_scan(const int8_t *shadow, uint64_t addr, size_t size) {
    uint64_t end = addr + size;
    for (uint64_t g = addr & ~7ULL; g < end; g += 8, shadow++) {
        int8_t s = *shadow;
        if (s == 0) continue;
        uint64_t lo = g > addr ? g : addr;          // first byte of the access in this granule
        uint64_t hi = g + 8 < end ? g + 8 : end;     // one past its last
        if (s < 0) return (size_t)(lo - addr) + 1;
        if (hi - g > (uint64_t)s)                    // it reaches past the k accessible bytes
            return (size_t)((lo - g >= (uint64_t)s ? lo : g + (uint64_t)s) - addr) + 1;
    }
    return 0;
}

#ifdef TOYOS_KASAN

static const char *kind_name(uint64_t bad) {
    int8_t s = *shadow_of(bad);
    // A partial granule is the END of an object; what lies past it says why.
    if (s > 0 && s < 8) s = *shadow_of(bad + 8);
    switch ((uint8_t)s) {
    case KASAN_PAGE_FREE:      return "use-after-free (a freed page)";
    case KASAN_HEAP_REDZONE:   return "heap-out-of-bounds";
    case KASAN_HEAP_FREE:      return "use-after-free";
    case KASAN_HEAP_UNUSED:    return "heap access to memory never allocated";
    case KASAN_GLOBAL_REDZONE: return "global-out-of-bounds";
    case 0xF1: case 0xF2: case 0xF3: return "stack-out-of-bounds";
    case 0xF5:                 return "stack-use-after-return";
    case 0xF8:                 return "stack-use-after-scope";
    default:                   return "wild-memory-access";
    }
}

static int first_time(uintptr_t ip) {
    unsigned h = (unsigned)((ip >> 2) * 2654435761u) % SEEN_SLOTS;
    for (unsigned i = 0; i < SEEN_SLOTS; i++, h = (h + 1) % SEEN_SLOTS) {
        if (g_seen[h] == ip) return 0;
        if (!g_seen[h]) { g_seen[h] = ip; return 1; }
    }
    return 0;
}

static void report(const char *kind, uint64_t addr, size_t size, int write,
                   uint64_t bad, uintptr_t ip) {
    if (!first_time(ip)) return;
    g_reporting = 1;
    uint32_t off = 0;
    const char *sym = ksyms_lookup(ip, &off);
    k_snprintf(g_last, sizeof g_last, "KASAN: %s in %s+0x%x: %s of size %lu at 0x%lx",
               kind, sym ? sym : "?", off, write ? "write" : "read",
               (unsigned long)size, (unsigned long)addr);
    g_count++;
    klog_printf(KLOG_ERR "%s\n", g_last);
    klog_printf(KLOG_ERR "  first bad byte 0x%lx, shadow 0x%02x, pid %d, link-time pc 0x%lx\n",
                (unsigned long)bad, (uint8_t)*shadow_of(bad), scheduler_current_pid(),
                (unsigned long)(ip - kernel_reloc_delta()));
    uint64_t rsp;
    __asm__ volatile ("mov %%rsp, %0" : "=r"(rsp));
    idt_log_stack_scan(rsp, 6);
    g_reporting = 0;
}

static inline void check(uint64_t addr, size_t size, int write, uintptr_t ip) {
    if (!g_ready || g_suppress || g_reporting || !size) return;
    if (addr >= g_limit || addr + size > g_limit || addr + size < addr) return;
    const int8_t *s = shadow_of(addr);
    // The common case in one compare: an access inside one granule
    // whose shadow says all eight bytes are live.
    if (*s == 0 && (addr & 7) + size <= 8) return;
    size_t r = kasan_scan(s, addr, size);
    if (r) report(kind_name(addr + r - 1), addr, size, write, addr + r - 1, ip);
}

void kasan_report_bad_free(const void *ptr, const char *what, uintptr_t ip) {
    if (!g_ready || g_suppress || g_reporting) return;
    report(what, (uint64_t)(uintptr_t)ptr, 0, 1, (uint64_t)(uintptr_t)ptr, ip);
}

#define IP() ((uintptr_t)__builtin_return_address(0))

// The entry points GCC emits. _noabort is what -fsanitize=kernel-address
// calls (it recovers by default); the plain names are kept for a caller
// built with -fno-sanitize-recover.
#define SIZED(n)                                                                \
    void __asan_load##n##_noabort(uintptr_t a)  { check(a, n, 0, IP()); }       \
    void __asan_store##n##_noabort(uintptr_t a) { check(a, n, 1, IP()); }       \
    void __asan_load##n(uintptr_t a)            { check(a, n, 0, IP()); }       \
    void __asan_store##n(uintptr_t a)           { check(a, n, 1, IP()); }
SIZED(1)
SIZED(2)
SIZED(4)
SIZED(8)
SIZED(16)

void __asan_loadN_noabort(uintptr_t a, size_t n)  { check(a, n, 0, IP()); }
void __asan_storeN_noabort(uintptr_t a, size_t n) { check(a, n, 1, IP()); }
void __asan_loadN(uintptr_t a, size_t n)          { check(a, n, 0, IP()); }
void __asan_storeN(uintptr_t a, size_t n)         { check(a, n, 1, IP()); }

// Called before a noreturn call: its frames will never RETURN, which is
// the only thing that clears their redzones.
void __asan_handle_no_return(void) {
    uint64_t rsp;
    __asm__ volatile ("mov %%rsp, %0" : "=r"(rsp));
    kasan_unpoison_stack_below(rsp);
}

// A VLA or alloca() with redzones (asan-stack). Linux's shape: the left
// redzone and the tail past `size` up to the right one are poisoned.
void __asan_alloca_poison(uintptr_t addr, size_t size) {
    kasan_poison((const void *)(addr - 32), 32, 0xCA);
    kasan_unpoison((const void *)addr, size);
    uint64_t end = (addr + size + 31) & ~31ULL;
    kasan_poison((const void *)(addr + size), end + 32 - (addr + size), 0xCB);
}
void __asan_allocas_unpoison(const void *top, const void *bottom) {
    if (top && bottom > top) kasan_unpoison(top, (size_t)((const uint8_t *)bottom - (const uint8_t *)top));
}

// --- globals ---------------------------------------------------------------

// GCC's descriptor, one per instrumented global (ASan ABI v8, the one
// __asan_version_mismatch_check_v8 names).
struct asan_global {
    const void *beg;
    size_t size;
    size_t size_with_redzone;
    const char *name;
    const char *module_name;
    size_t has_dynamic_init;
    void *location;
    size_t odr_indicator;
};

void __asan_register_globals(struct asan_global *g, size_t n) {
    for (size_t i = 0; i < n; i++) {
        kasan_unpoison(g[i].beg, g[i].size);
        kasan_poison((const uint8_t *)g[i].beg + g[i].size,
                     g[i].size_with_redzone - g[i].size, KASAN_GLOBAL_REDZONE);
    }
}
void __asan_unregister_globals(struct asan_global *g, size_t n) { (void)g; (void)n; }
void __asan_version_mismatch_check_v8(void) {}
void __asan_before_dynamic_init(const char *m) { (void)m; }
void __asan_after_dynamic_init(void) {}

// The constructors GCC emitted to call __asan_register_globals, one per
// instrumented file. Nothing else in the kernel has a constructor.
typedef void (*ctor_fn)(void);
extern ctor_fn __init_array_start[], __init_array_end[];

void kasan_run_constructors(void) {
    for (ctor_fn *f = __init_array_start; f < __init_array_end; f++) (*f)();
}

#else  // !TOYOS_KASAN: nothing instrumented, nothing to do

int kasan_enabled(void) { return 0; }
unsigned kasan_report_count(void) { return 0; }
const char *kasan_last_report(void) { return ""; }
void kasan_suppress_begin(void) {}
void kasan_suppress_end(void) {}
void kasan_run_constructors(void) {}

#endif
