// The kernel's entropy source. See kernel/include/api/krandom.h for the
// contract, and docs/decisions.md for why it stops where it does.
//
// THREE LAYERS, AND ONLY THE FIRST IS REAL ENTROPY
// ------------------------------------------------
// 1. RDSEED, then RDRAND (kernel/arch/x86_64/random_hw.c). The CPU's own
//    entropy source. When this is available it is used for every value,
//    not just to seed -- there is no reason to prefer a software mixer
//    over the hardware it would be seeded from.
// 2. TSC jitter, when the CPU has neither. Sampled at init: read the
//    timestamp counter repeatedly around work whose duration the caller
//    cannot predict (a PIT tick boundary, a cache-missing walk), and
//    keep the LOW bits of the deltas, which is where the variation is.
// 3. A mixing function over whichever of those was obtained, so that
//    the bits handed out are spread across the whole 64 even when the
//    input's variation lives in a handful of low ones.
//
// **What is deliberately NOT claimed.** Under QEMU's TCG the timestamp
// counter is produced by software, and jitter measured against it is
// far weaker than the same code on real hardware -- it may be nearly
// deterministic. That is not a bug to fix here, it is a limit to
// report, which is what KRANDOM_JITTER means as distinct from
// KRANDOM_HW. A caller that needs real unpredictability must check
// krandom_quality() and decide; nothing here pretends the two are
// equivalent. (`--cpu max` gives the guest RDRAND/RDSEED, which is how
// the hardware path gets tested at all here -- the default qemu64 model
// has neither.)
#include "krandom.h"
#include "random_hw.h"
#include "klog.h"
#include "timer.h"

static uint64_t pool;                  // mixed state, advanced on every draw
static enum krandom_quality quality = KRANDOM_NONE;

// splitmix64's finalizer. A well-known bijective avalanche step: every
// input bit affects every output bit, so a counter, a timestamp, or a
// handful of jittery low bits all come out looking like noise. Chosen
// because it is one multiply-xor-shift chain rather than a primitive
// this kernel would then have to keep correct (see docs/decisions.md on
// not shipping a stream cipher for this).
//
// Being a bijection is the point AND the caveat: it does not create
// entropy, it only spreads what it is given. Feeding it a predictable
// counter gives predictable-but-scrambled output, which is precisely
// the KRANDOM_NONE case.
static uint64_t mix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

// One hardware value, if the CPU has either instruction. RDSEED first
// (it is the entropy source; RDRAND is a DRBG downstream of it), and
// either may legitimately decline, in which case we fall through.
static int hw_u64(uint64_t *out) {
    if (arch_has_rdseed() && arch_rdseed64(out)) return 1;
    if (arch_has_rdrand() && arch_rdrand64(out)) return 1;
    return 0;
}

// Harvest timing variation into the pool.
//
// The measurement is the DELTA between consecutive timestamp reads
// across a PIT tick boundary, and what is kept is those deltas mixed
// together -- not the timestamps themselves, which are mostly a
// predictable ramp. Waiting on the tick is what makes the interval
// something other than "however long this loop takes": the tick arrives
// on the interrupt controller's schedule, asynchronously to this code,
// so where in the loop it lands varies.
//
// SAMPLES is a compromise. Each one costs up to a full 10ms tick, so
// this is boot time being spent -- eight of them is ~80ms worst case,
// which is invisible next to the disk work around it, while one sample
// would be a single interrupt's phase and hardly worth mixing.
#define JITTER_SAMPLES 8

static void harvest_jitter(void) {
    for (int i = 0; i < JITTER_SAMPLES; i++) {
        uint64_t start_tick = pit_ticks();
        uint64_t t0 = arch_rdtsc();
        uint64_t spins = 0;
        // Spin until the tick changes, counting iterations. BOTH results
        // are mixed in: the elapsed cycle count and how many times this
        // loop got around, which are not the same measurement when
        // anything else (an interrupt, a cache miss) interrupts it.
        while (pit_ticks() == start_tick) spins++;
        uint64_t t1 = arch_rdtsc();
        pool = mix64(pool ^ (t1 - t0)) ^ mix64(spins + i);
    }
}

void krandom_init(void) {
    uint64_t hw;
    if (hw_u64(&hw)) {
        pool = mix64(pool ^ hw);
        quality = KRANDOM_HW;
        klog_write("krandom: hardware entropy (RDSEED/RDRAND)\n");
        return;
    }

    // No hardware source. Mix in the timestamp before harvesting as
    // well as the harvest itself -- on a machine where the jitter turns
    // out to be nearly deterministic, boot-to-boot variation in WHEN
    // this runs is still worth something, and it costs one instruction.
    pool = mix64(pool ^ arch_rdtsc());
    harvest_jitter();
    quality = KRANDOM_JITTER;
    klog_write("krandom: no RDSEED/RDRAND -- seeded from TSC jitter (weak under emulation)\n");
}

uint64_t krandom_u64(void) {
    uint64_t hw;
    if (quality == KRANDOM_HW && hw_u64(&hw)) {
        // Still mixed with the pool rather than returned raw: it costs
        // nothing, and it means a hardware source that silently starts
        // returning a constant cannot make this function return a
        // constant too.
        pool = mix64(pool ^ hw);
        return pool;
    }

    // Software path. The pool advances through mix64 on every draw, and
    // the timestamp is folded in each time so that two draws separated
    // by any real work differ by more than a counter step.
    pool = mix64(pool ^ arch_rdtsc());
    return mix64(pool);
}

void krandom_bytes(void *buf, size_t n) {
    uint8_t *p = (uint8_t *)buf;
    if (!p) return;
    while (n >= 8) {
        uint64_t v = krandom_u64();
        for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (i * 8));
        p += 8;
        n -= 8;
    }
    if (n) {
        // The tail takes its bytes from one fresh value rather than
        // reusing the last one, so a caller asking for 9 bytes cannot
        // get byte 8 equal to byte 0.
        uint64_t v = krandom_u64();
        for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(v >> (i * 8));
    }
}

enum krandom_quality krandom_quality(void) { return quality; }

const char *krandom_quality_name(enum krandom_quality q) {
    switch (q) {
        case KRANDOM_HW:     return "hardware (RDSEED/RDRAND)";
        case KRANDOM_JITTER: return "TSC jitter";
        default:             return "none";
    }
}
