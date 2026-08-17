// Reads what the CPU says about itself, and what this kernel has
// actually switched on. See kernel/include/api/cpuinfo.h for the
// supported-vs-enabled split that shapes the whole thing.
//
// In arch/x86_64 by kernel/README.md's test: CPUID, RDTSC and the
// control registers are all x86 instructions with no portable
// equivalent -- a different CPU would replace this file, not adapt it.
#include "cpuinfo.h"
#include "timer.h"
#include "string.h"

// MSR 0xC0000080. Read with RDMSR, which is ring 0 only -- part of why
// the "enabled" half of this header can't come from userland.
#define MSR_EFER      0xC0000080u
#define EFER_NXE      (1u << 11)
#define EFER_LMA      (1u << 10)

#define CR0_WP        (1u << 16)
#define CR0_PG        (1u << 31)
#define CR0_EM        (1u << 2)
#define CR0_MP        (1u << 1)

#define CR4_PAE       (1u << 5)
#define CR4_PGE       (1u << 7)
#define CR4_OSFXSR    (1u << 9)
#define CR4_OSXMMEXCPT (1u << 10)
#define CR4_UMIP      (1u << 11)
#define CR4_SMEP      (1u << 20)
#define CR4_SMAP      (1u << 21)
#define CR4_OSXSAVE   (1u << 18)

struct regs4 { uint32_t eax, ebx, ecx, edx; };

static struct regs4 cpuid_leaf(uint32_t leaf, uint32_t subleaf) {
    struct regs4 r;
    __asm__ volatile ("cpuid"
                       : "=a"(r.eax), "=b"(r.ebx), "=c"(r.ecx), "=d"(r.edx)
                       : "a"(leaf), "c"(subleaf));
    return r;
}

static uint64_t read_cr0(void) {
    uint64_t v; __asm__ volatile ("mov %%cr0, %0" : "=r"(v)); return v;
}

static uint64_t read_cr4(void) {
    uint64_t v; __asm__ volatile ("mov %%cr4, %0" : "=r"(v)); return v;
}

static uint64_t read_msr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t read_tsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// Copies 4 bytes of a register into a buffer, little-endian -- how
// CPUID returns every string it produces (vendor, brand).
static void put_reg(char *dst, uint32_t v) {
    dst[0] = (char)(v & 0xFF);
    dst[1] = (char)((v >> 8) & 0xFF);
    dst[2] = (char)((v >> 16) & 0xFF);
    dst[3] = (char)((v >> 24) & 0xFF);
}

// ---------------------------------------------------------------------
// Clock speed
// ---------------------------------------------------------------------

// Calibration is done ONCE and cached: it costs real wall-clock time
// (it has to wait for PIT ticks to elapse), and nothing about the
// answer changes between calls. Without the cache, every `lscpu` and
// every repaint of the Control Panel's System Info page would stall for
// the calibration window.
static uint32_t g_cached_mhz = 0;
static uint8_t g_cached_source = CPU_MHZ_UNKNOWN;

// Number of 100Hz PIT ticks to measure across. 20 ticks = 200ms: long
// enough that the +/-1 tick quantisation is a ~5% error rather than a
// 100% one, short enough not to be a visible hang. This is the whole
// reason the result is cached.
#define CALIBRATE_TICKS 20
// PIT_HZ comes from timer.h -- it used to be redefined here, which is
// two statements of one fact that a tick-rate change would have split.

// RDTSC against the PIT. Deliberately the fallback, not the first
// choice: it measures the TSC's rate, which on any modern part is a
// fixed "invariant" reference frequency rather than the core's current
// clock -- so this is honest about being a measurement, and
// cpu_mhz_source records which of the two answers the caller got.
//
// Under QEMU's TCG this measures emulated time against emulated ticks
// and the number means very little; that's a property of the
// environment, not a bug here, and `lscpu` labels the source so the
// reader can tell.
static uint32_t measure_mhz(void) {
    uint64_t start_tick = pit_ticks();
    // Wait for the next tick edge first, so the measurement window
    // starts aligned rather than partway through a tick.
    while (pit_ticks() == start_tick) { __asm__ volatile ("pause"); }

    uint64_t t0 = read_tsc();
    uint64_t tick0 = pit_ticks();
    while (pit_ticks() - tick0 < CALIBRATE_TICKS) { __asm__ volatile ("pause"); }
    uint64_t t1 = read_tsc();
    uint64_t ticks = pit_ticks() - tick0;
    if (ticks == 0) return 0;

    // cycles per second = delta_tsc / (ticks / PIT_HZ); then / 1e6 for MHz.
    uint64_t cycles = t1 - t0;
    return (uint32_t)((cycles * PIT_HZ) / (ticks * 1000000ull));
}

// Calibrates once, at boot. **This cannot be done lazily on first use,
// and that isn't a preference -- it deadlocks.** measure_mhz() spins
// waiting for pit_ticks() to advance, and pit_ticks() only advances
// from the timer IRQ. Every interrupt gate in this kernel (including
// the int 0x80 syscall gate) clears IF on entry, so a lazy calibration
// reached through SYS_CPU_INFO waits forever for a tick that cannot
// arrive while it holds the CPU. Found exactly that way: /bin/lscpu
// hung with no output and no fault.
//
// Doing it at boot also means no caller ever pays the 200ms window.
void cpu_info_init(void) {
    struct regs4 r0 = cpuid_leaf(0, 0);

    // Leaf 16H reports the base frequency directly, but only exists on
    // Skylake and later -- and notably NOT on the `qemu64` model this
    // project boots by default, which is why the measured path below
    // isn't dead code.
    if (r0.eax >= 0x16) {
        struct regs4 r = cpuid_leaf(0x16, 0);
        if ((r.eax & 0xFFFF) != 0) {
            g_cached_mhz = r.eax & 0xFFFF;
            g_cached_source = CPU_MHZ_CPUID_16H;
            return;
        }
    }

    g_cached_mhz = measure_mhz();
    g_cached_source = g_cached_mhz ? CPU_MHZ_MEASURED : CPU_MHZ_UNKNOWN;
}

static void fill_mhz(struct cpu_info *out) {
    // Whatever cpu_info_init() worked out, or UNKNOWN if it never ran.
    // Never calibrates here -- see cpu_info_init()'s comment.
    out->mhz = g_cached_mhz;
    out->mhz_source = g_cached_source;
}

// ---------------------------------------------------------------------
// Caches
// ---------------------------------------------------------------------

// AMD's L2/L3 associativity is a 4-bit ENCODING, not a count -- the
// values jump (5 means 6-way, 6 means 8-way) and 0xF means fully
// associative. Table per the AMD APM, CPUID Fn8000_0006.
static uint16_t amd_assoc(uint32_t enc) {
    static const uint16_t TABLE[16] = {
        0, 1, 2, 3, 4, 6, 8, 0, 16, 0, 32, 48, 64, 96, 128, 0xFFFF
    };
    return TABLE[enc & 0xF];
}

static void add_cache(struct cpu_info *out, uint8_t level, uint8_t type,
                       uint32_t size_kb, uint16_t ways, uint16_t line_size) {
    if (out->cache_count >= CPU_MAX_CACHES || size_kb == 0) return;
    struct cpu_cache *c = &out->cache[out->cache_count++];
    c->level = level;
    c->type = type;
    c->size_kb = size_kb;
    c->ways = ways;
    c->line_size = line_size;
    // Leaf 4 reports sets directly; these AMD leaves don't, so derive
    // it. Guarded because a fully-associative cache has one set and
    // `ways` is the 0xFFFF sentinel, not a divisor.
    c->sets = (ways && ways != 0xFFFF && line_size)
               ? (size_kb * 1024u) / ((uint32_t)ways * line_size)
               : 1;
}

// AMD's cache leaves: 80000005H for L1 (separate data and instruction),
// 80000006H for L2 and L3. This is the fallback when leaf 4 comes back
// empty, and it is NOT a rarely-taken path -- the `qemu64` model this
// project boots by default reports as AuthenticAMD, populates neither
// leaf 4 nor AMD's newer 8000001DH, and would otherwise show no cache
// information at all. Found by running lscpu and getting an empty
// section.
static void fill_caches_amd(struct cpu_info *out, uint32_t max_ext_leaf) {
    if (max_ext_leaf >= 0x80000005u) {
        struct regs4 r = cpuid_leaf(0x80000005u, 0);
        // ECX = L1 data, EDX = L1 instruction; both: [31:24] size KiB,
        // [23:16] associativity, [7:0] line size.
        add_cache(out, 1, CPU_CACHE_DATA,
                   (r.ecx >> 24) & 0xFF, (uint16_t)((r.ecx >> 16) & 0xFF),
                   (uint16_t)(r.ecx & 0xFF));
        add_cache(out, 1, CPU_CACHE_INSTRUCTION,
                   (r.edx >> 24) & 0xFF, (uint16_t)((r.edx >> 16) & 0xFF),
                   (uint16_t)(r.edx & 0xFF));
    }
    if (max_ext_leaf >= 0x80000006u) {
        struct regs4 r = cpuid_leaf(0x80000006u, 0);
        // ECX = L2: [31:16] size KiB, [15:12] assoc encoding, [7:0] line.
        add_cache(out, 2, CPU_CACHE_UNIFIED,
                   (r.ecx >> 16) & 0xFFFF, amd_assoc((r.ecx >> 12) & 0xF),
                   (uint16_t)(r.ecx & 0xFF));
        // EDX = L3: [31:18] size in 512 KiB units, [15:12] assoc, [7:0] line.
        add_cache(out, 3, CPU_CACHE_UNIFIED,
                   ((r.edx >> 18) & 0x3FFF) * 512, amd_assoc((r.edx >> 12) & 0xF),
                   (uint16_t)(r.edx & 0xFF));
    }
}

// Leaf 4 enumerates one cache per subleaf until it reports type 0.
// Preferred over the legacy leaf-2 descriptor byte table, which is a
// hardcoded lookup of magic constants covering only Intel parts Intel
// bothered to assign a byte to. Falls back to AMD's own leaves above
// when leaf 4 yields nothing.
static void fill_caches(struct cpu_info *out, uint32_t max_leaf, uint32_t max_ext_leaf) {
    out->cache_count = 0;
    if (max_leaf < 4) {
        fill_caches_amd(out, max_ext_leaf);
        return;
    }

    for (uint32_t i = 0; i < CPU_MAX_CACHES; i++) {
        struct regs4 r = cpuid_leaf(4, i);
        uint32_t type = r.eax & 0x1F;
        if (type == 0) break; // no more caches -- leaf 4's terminator

        struct cpu_cache *c = &out->cache[out->cache_count];
        c->type  = (uint8_t)(type <= CPU_CACHE_UNIFIED ? type : CPU_CACHE_NONE);
        c->level = (uint8_t)((r.eax >> 5) & 0x7);

        // Each of these fields is stored as (value - 1), per the SDM.
        uint32_t ways       = ((r.ebx >> 22) & 0x3FF) + 1;
        uint32_t partitions = ((r.ebx >> 12) & 0x3FF) + 1;
        uint32_t line_size  = (r.ebx & 0xFFF) + 1;
        uint32_t sets       = r.ecx + 1;

        c->ways      = (uint16_t)(((r.eax >> 9) & 1) ? 0xFFFF : ways); // bit 9 = fully associative
        c->line_size = (uint16_t)line_size;
        c->sets      = sets;
        c->size_kb   = (ways * partitions * line_size * sets) / 1024;

        out->cache_count++;
    }

    // Leaf 4 exists but described nothing -- the qemu64 case. Try AMD's.
    if (out->cache_count == 0) fill_caches_amd(out, max_ext_leaf);
}

// ---------------------------------------------------------------------
// The whole picture
// ---------------------------------------------------------------------

static uint32_t read_enabled(void) {
    uint64_t cr0 = read_cr0();
    uint64_t cr4 = read_cr4();
    uint64_t efer = read_msr(MSR_EFER);
    uint32_t en = 0;

    if (cr0 & CR0_PG) en |= CPU_EN_PAGING;
    if (cr0 & CR0_WP) en |= CPU_EN_WP;
    // "FPU enabled" is EM clear AND MP set -- EM=1 makes every FP
    // instruction #UD, which is the state this kernel booted in until
    // fpu_init() ran.
    if (!(cr0 & CR0_EM) && (cr0 & CR0_MP)) en |= CPU_EN_FPU;

    if (cr4 & CR4_PAE)        en |= CPU_EN_PAE;
    if (cr4 & CR4_PGE)        en |= CPU_EN_PGE;
    if (cr4 & CR4_OSFXSR)     en |= CPU_EN_SSE;
    if (cr4 & CR4_OSXMMEXCPT) en |= CPU_EN_XMMEXCPT;
    if (cr4 & CR4_OSXSAVE)    en |= CPU_EN_OSXSAVE;
    if (cr4 & CR4_SMEP)       en |= CPU_EN_SMEP;
    if (cr4 & CR4_SMAP)       en |= CPU_EN_SMAP;
    if (cr4 & CR4_UMIP)       en |= CPU_EN_UMIP;

    if (efer & EFER_NXE) en |= CPU_EN_NX;
    if (efer & EFER_LMA) en |= CPU_EN_LONG_MODE;

    return en;
}

void cpu_info_get(struct cpu_info *out) {
    if (!out) return;
    k_memset(out, 0, sizeof(*out));

    struct regs4 r0 = cpuid_leaf(0, 0);
    out->max_leaf = r0.eax;
    put_reg(&out->vendor[0], r0.ebx); // EBX,EDX,ECX -- not EBX,ECX,EDX.
    put_reg(&out->vendor[4], r0.edx); // Getting this order wrong spells
    put_reg(&out->vendor[8], r0.ecx); // "GenuintelineI".
    out->vendor[12] = '\0';

    if (out->max_leaf >= 1) {
        struct regs4 r1 = cpuid_leaf(1, 0);
        out->feature[CPU_WORD_1_EDX] = r1.edx;
        out->feature[CPU_WORD_1_ECX] = r1.ecx;

        // Family/model are split across base and extended fields, and
        // the combining rule differs per family -- the SDM's, not a
        // simplification: extended family always adds for family 0xF,
        // extended model shifts in for families 6 and 0xF only.
        uint32_t base_family = (r1.eax >> 8) & 0xF;
        uint32_t base_model  = (r1.eax >> 4) & 0xF;
        uint32_t ext_family  = (r1.eax >> 20) & 0xFF;
        uint32_t ext_model   = (r1.eax >> 16) & 0xF;

        out->family = (uint16_t)(base_family == 0xF ? base_family + ext_family
                                                     : base_family);
        out->model = (uint16_t)((base_family == 0x6 || base_family == 0xF)
                                 ? base_model + (ext_model << 4)
                                 : base_model);
        out->stepping = (uint8_t)(r1.eax & 0xF);
    }

    if (out->max_leaf >= 7) {
        struct regs4 r7 = cpuid_leaf(7, 0);
        out->feature[CPU_WORD_7_0_EBX] = r7.ebx;
        out->feature[CPU_WORD_7_0_ECX] = r7.ecx;
        out->feature[CPU_WORD_7_0_EDX] = r7.edx;
    }

    struct regs4 re0 = cpuid_leaf(0x80000000u, 0);
    out->max_ext_leaf = re0.eax;

    if (out->max_ext_leaf >= 0x80000001u) {
        struct regs4 re1 = cpuid_leaf(0x80000001u, 0);
        out->feature[CPU_WORD_EXT1_EDX] = re1.edx;
        out->feature[CPU_WORD_EXT1_ECX] = re1.ecx;
    }

    // The brand string is 48 bytes spread across three leaves, four
    // registers each, in order. Absent on very old parts, hence the
    // check -- callers get an empty string, not garbage.
    if (out->max_ext_leaf >= 0x80000004u) {
        int pos = 0;
        for (uint32_t leaf = 0x80000002u; leaf <= 0x80000004u; leaf++) {
            struct regs4 rb = cpuid_leaf(leaf, 0);
            put_reg(&out->brand[pos + 0],  rb.eax);
            put_reg(&out->brand[pos + 4],  rb.ebx);
            put_reg(&out->brand[pos + 8],  rb.ecx);
            put_reg(&out->brand[pos + 12], rb.edx);
            pos += 16;
        }
        out->brand[48] = '\0';
    }

    fill_caches(out, out->max_leaf, out->max_ext_leaf);
    out->enabled = read_enabled();
    fill_mhz(out);
}

// cpu_has_feature() and cpu_cache_type_name() are `static inline` in
// cpuinfo.h, not here -- /bin/lscpu can't link against this file. See
// that header's comment.
