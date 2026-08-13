// `lscpu` -- what the CPU is, what it supports, and what this kernel
// has actually turned on.
//
// Named after Linux's lscpu, and a sibling of this project's own lspci:
// a real ring-3 ELF in /bin driven by a syscall (SYS_CPU_INFO), not a
// shell builtin.
//
// **The `enabled` column is the part Linux's lscpu doesn't have**, and
// it's the reason this is more than a CPUID dump. CPUID answers "can
// this silicon do X"; it says nothing about whether the OS switched X
// on. Those are different questions with different answers -- SSE2 is
// supported by every x86-64 CPU ever built and was NOT enabled in this
// kernel until CR4.OSFXSR got set. So a supported feature that needs an
// OS opt-in prints as `sse2*` with the star explained in the legend,
// and the "Enabled by this kernel" block below lists each control-
// register bit by name and source.
//
// Why a syscall at all, when CPUID is unprivileged and this program
// could just execute it: CR0/CR4/EFER are privileged and ring 3 cannot
// read them, so the `enabled` half has to come from the kernel
// regardless. Given that, taking the whole struct in one call keeps the
// decoding (family/model combining, leaf-4 cache arithmetic) in one
// place. See api/cpuinfo.h.
#include <stdint.h>
#include "syscall_abi.h"
#include "cpuinfo.h"
#include "cpu_features.h" // the (word,bit) -> name table, shared with the Control Panel

static inline int64_t syscall2(uint64_t num, uint64_t arg1, uint64_t arg2) {
    int64_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(num), "D"(arg1), "S"(arg2) : "memory");
    return ret;
}

static inline int64_t syscall3(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    int64_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(num), "D"(arg1), "S"(arg2), "d"(arg3) : "memory");
    return ret;
}

static inline void sys_exit(int code) __attribute__((noreturn));
static inline void sys_exit(int code) {
    syscall2(SYS_EXIT, (uint64_t)(int64_t)code, 0);
    for (;;) { }
}

static uint64_t my_strlen(const char *s) {
    uint64_t n = 0;
    while (s[n]) n++;
    return n;
}

// Same small local print helpers lspci.c carries, and for the same
// reason: kfmt.h's k_snprintf() is kernel code, never linked into a
// ring-3 ELF.
static void put(const char *s) {
    syscall3(SYS_WRITE, 1, (uint64_t)(uintptr_t)s, my_strlen(s));
}

static void put_udec(uint32_t v) {
    char buf[11];
    int i = 10;
    buf[i] = '\0';
    if (v == 0) buf[--i] = '0';
    while (v > 0) { buf[--i] = (char)('0' + (v % 10)); v /= 10; }
    put(buf + i);
}

static void put_hex(uint32_t v, int digits) {
    char buf[9];
    for (int i = 0; i < digits; i++) {
        uint8_t nib = (uint8_t)((v >> ((digits - 1 - i) * 4)) & 0xF);
        buf[i] = nib < 10 ? (char)('0' + nib) : (char)('a' + nib - 10);
    }
    buf[digits] = '\0';
    put(buf);
}

// Left-justified label, so the values line up in a column without a
// printf-style width specifier (there is no printf here).
static void put_label(const char *s) {
    put(s);
    put(":");
    int pad = 22 - (int)my_strlen(s) - 1;
    for (int i = 0; i < pad; i++) put(" ");
}

// A cache size reads better as "32 KiB" or "16 MiB" than "16384 KiB".
static void put_cache_size(uint32_t kb) {
    if (kb >= 1024 && (kb % 1024) == 0) {
        put_udec(kb / 1024);
        put(" MiB");
    } else {
        put_udec(kb);
        put(" KiB");
    }
}

static struct cpu_info g_ci; // a global, not a stack local: 300+ bytes,
                              // and the user stack here is a single page

int main_lscpu(void);

void _start(void) {
    sys_exit(main_lscpu());
}

int main_lscpu(void) {
    if (syscall2(SYS_CPU_INFO, (uint64_t)(uintptr_t)&g_ci, 0) != 1) {
        put("lscpu: SYS_CPU_INFO failed\n");
        return 1;
    }

    // ---- identity ------------------------------------------------------
    put_label("Vendor");        put(g_ci.vendor);  put("\n");
    if (g_ci.brand[0]) {
        // The brand string is right-padded with spaces by many CPUs
        // (it's a fixed 48-byte field), so trim before printing.
        char trimmed[CPU_BRAND_LEN];
        int n = (int)my_strlen(g_ci.brand);
        while (n > 0 && g_ci.brand[n - 1] == ' ') n--;
        for (int i = 0; i < n; i++) trimmed[i] = g_ci.brand[i];
        trimmed[n] = '\0';
        // Leading spaces happen too, on Intel parts especially.
        const char *p = trimmed;
        while (*p == ' ') p++;
        put_label("Model name"); put(p); put("\n");
    }
    put_label("Family");        put_udec(g_ci.family);   put("\n");
    put_label("Model");         put_udec(g_ci.model);    put("\n");
    put_label("Stepping");      put_udec(g_ci.stepping); put("\n");

    put_label("CPU MHz");
    if (g_ci.mhz_source == CPU_MHZ_UNKNOWN) {
        put("unknown\n");
    } else {
        put_udec(g_ci.mhz);
        put(g_ci.mhz_source == CPU_MHZ_CPUID_16H
             ? "  (CPUID leaf 16H, base frequency)\n"
             : "  (measured: RDTSC against the PIT)\n");
    }

    put_label("Max CPUID leaf"); put("0x"); put_hex(g_ci.max_leaf, 8);
    put("  extended 0x"); put_hex(g_ci.max_ext_leaf, 8); put("\n");

    // ---- caches --------------------------------------------------------
    put("\nCaches:\n");
    if (g_ci.cache_count == 0) {
        put("  (CPUID leaf 4 reported none)\n");
    }
    for (int i = 0; i < g_ci.cache_count; i++) {
        const struct cpu_cache *c = &g_ci.cache[i];
        put("  L");
        put_udec(c->level);
        put(" ");
        put(cpu_cache_type_name(c->type));
        // Pad the type name out to the width of "instruction".
        for (int p = (int)my_strlen(cpu_cache_type_name(c->type)); p < 12; p++) put(" ");
        put_cache_size(c->size_kb);
        put(", ");
        if (c->ways == 0xFFFF) {
            put("fully associative");
        } else {
            put_udec(c->ways);
            put("-way");
        }
        put(", ");
        put_udec(c->line_size);
        put(" B lines, ");
        put_udec(c->sets);
        put(" sets\n");
    }

    // ---- what the kernel switched on -----------------------------------
    put("\nEnabled by this kernel:\n");
    for (int i = 0; i < CPU_ENABLE_COUNT; i++) {
        put("  ");
        put((g_ci.enabled & CPU_ENABLES[i].bit) ? "[on ] " : "[off] ");
        put(CPU_ENABLES[i].name);
        for (int p = (int)my_strlen(CPU_ENABLES[i].name); p < 16; p++) put(" ");
        put(CPU_ENABLES[i].source);
        put("\n");
    }

    // ---- supported features --------------------------------------------
    put("\nFlags (* = supported but NOT enabled by this kernel):\n ");
    int col = 1;
    int gated_off = 0;
    for (int i = 0; i < CPU_FEATURE_COUNT; i++) {
        const struct cpu_feature_desc *f = &CPU_FEATURES[i];
        if (!cpu_has_feature(&g_ci, f->word, f->bit)) continue;

        int needs = f->enable != 0;
        int off = needs && !(g_ci.enabled & f->enable);
        if (off) gated_off++;

        int len = (int)my_strlen(f->name) + (off ? 1 : 0) + 1;
        if (col + len > 78) { put("\n "); col = 1; }
        put(" ");
        put(f->name);
        if (off) put("*");
        col += len;
    }
    put("\n");

    if (gated_off == 0) {
        put("\nEvery gated feature the CPU supports is enabled.\n");
    } else {
        put("\n");
        put_udec((uint32_t)gated_off);
        put(" supported feature(s) are not enabled -- see the block above\n");
        put("for which control-register bit each one waits on.\n");
    }
    return 0;
}
