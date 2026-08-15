// Tests for the entropy source (kernel/lib/krandom.c).
//
// WHAT THESE CAN AND CANNOT CHECK
// -------------------------------
// They cannot check that the output is random. Statistical tests need
// far more samples than is sane to draw inside a boot-time suite, and
// passing one would not prove unpredictability anyway -- a counter run
// through a good mixer passes chi-squared comfortably. Writing such a
// test would produce the most dangerous kind of green: a number that
// looks like evidence and is not.
//
// So these check the properties that CAN be established, and which are
// where the real bugs would be:
//
//   * the API's contracts (byte counts, tails, no overrun);
//   * that consecutive draws differ, which is what a stuck source or a
//     pool that fails to advance would break -- the exact failure mode
//     of getting the RDRAND carry-flag check wrong, since the register
//     is architecturally ZERO on failure;
//   * that the mixer avalanches, which is what makes a jittery input's
//     few varying bits reach the whole word;
//   * that quality reporting agrees with what the CPU actually says,
//     because an honest label is the entire point of this API.
#include "ktest.h"
#include "krandom.h"
#include "random_hw.h"
#include "cpuinfo.h"

KTEST("krandom", "consecutive draws differ") {
    // A stuck source is THE failure mode here, and it is silent: an
    // RDRAND whose carry flag is ignored returns a hard zero every
    // time, and a pool that never advances returns one value forever.
    // Eight draws all being equal is not a plausible accident.
    uint64_t v[8];
    for (int i = 0; i < 8; i++) v[i] = krandom_u64();

    int all_same = 1;
    for (int i = 1; i < 8; i++) if (v[i] != v[0]) all_same = 0;
    KTEST_ASSERT(!all_same); // eight consecutive krandom_u64() draws were identical;

    int any_zero_run = 0;
    for (int i = 0; i < 8; i++) if (v[i] == 0) any_zero_run = 1;
    KTEST_ASSERT(!any_zero_run); // a draw returned 0 -- the RDRAND/RDSEED failure value;
}

KTEST("krandom", "bytes fills exactly the requested length") {
    // The tail path (n % 8) is hand-written and is where an off-by-one
    // would live. Guard bytes on both sides catch a write past either
    // end, which a plain "did it fill" check cannot see.
    for (size_t n = 1; n <= 24; n++) {
        uint8_t buf[40];
        for (size_t i = 0; i < sizeof buf; i++) buf[i] = 0xAA;

        krandom_bytes(buf + 8, n);

        for (int i = 0; i < 8; i++) {
            KTEST_ASSERT(buf[i] == 0xAA); // krandom_bytes wrote before the buffer;
        }
        for (size_t i = 8 + n; i < sizeof buf; i++) {
            KTEST_ASSERT(buf[i] == 0xAA); // krandom_bytes wrote past the requested length;
        }
    }
}

KTEST("krandom", "a large fill is not one value repeated") {
    // krandom_bytes() draws a fresh krandom_u64() per 8 bytes. Copying
    // one value across the whole buffer would satisfy "it filled the
    // buffer" and every byte-count check above, so compare the 8-byte
    // blocks against each other.
    uint8_t buf[64];
    krandom_bytes(buf, sizeof buf);

    int distinct_blocks = 0;
    for (size_t b = 1; b < sizeof buf / 8; b++) {
        int same = 1;
        for (int i = 0; i < 8; i++) if (buf[b * 8 + i] != buf[i]) same = 0;
        if (!same) distinct_blocks++;
    }
    KTEST_ASSERT(distinct_blocks > 0); // every 8-byte block of a 64-byte fill was identical;
}

KTEST("krandom", "output is spread across the whole 64 bits") {
    // The jitter source's variation lives in a handful of low bits;
    // mix64() is what spreads it. If the mixer were dropped (or the
    // pool were only ever a small counter), the high bits would sit
    // still while the low ones moved -- so OR the draws together and
    // require both halves to have seen a 1.
    //
    // What this does NOT catch, measured rather than assumed: a source
    // stuck at a constant passes it, as long as that constant has bits
    // in both halves (a positive control returning 0x1122334455667788
    // left this test green and reddened the two above instead). That is
    // the right division of labour -- it is here to catch a MIXER that
    // stopped spreading, not a source that stopped moving.
    uint64_t any = 0;
    for (int i = 0; i < 32; i++) any |= krandom_u64();

    KTEST_ASSERT((any >> 32) != 0); // no draw in 32 ever set a bit above 31;
    KTEST_ASSERT((any & 0xFFFFFFFFULL) != 0); // no draw in 32 ever set a bit below 32;
}

KTEST("krandom", "reported quality matches what the CPU offers") {
    // The honesty check: this API's whole justification is that a
    // caller can ask how much to trust the bytes, so the label must not
    // drift from the source. KRANDOM_HW is claimable only if CPUID
    // actually reports one of the two instructions.
    enum krandom_quality q = krandom_quality();
    KTEST_ASSERT(q != KRANDOM_NONE); // krandom_init() did not run before the suite;

    int hw = arch_has_rdseed() || arch_has_rdrand();
    if (q == KRANDOM_HW) {
        KTEST_ASSERT(hw); // claimed hardware entropy on a CPU reporting neither RDSEED nor RDRAND;
    }
    // The converse is deliberately NOT asserted: a CPU can report the
    // instruction and still have it fail every retry, which correctly
    // leaves the jitter fallback in charge. Requiring HW-when-available
    // would make that legitimate case a test failure.

    KTEST_ASSERT(krandom_quality_name(q) != 0); // quality name was NULL;
}

KTEST("krandom", "the stack guard was randomized away from its build-time value") {
    // The end-to-end check that entropy actually reached its first
    // consumer. 0xDEC0DE99AA55C3A5 is stack_protector.c's compile-time
    // constant; kernel_main() replaces it via stack_guard_randomize().
    // Also verify the deliberate zero low byte, which is a defence (it
    // makes the guard terminate a string copy) and not an accident.
    extern uintptr_t __stack_chk_guard;
    KTEST_ASSERT(__stack_chk_guard != 0xDEC0DE99AA55C3A5ULL); // stack guard is still the build-time constant;
    KTEST_ASSERT(__stack_chk_guard != 0); // stack guard is zero;
    KTEST_ASSERT((__stack_chk_guard & 0xFF) == 0); // stack guard's low byte is not the deliberate NUL;
}
