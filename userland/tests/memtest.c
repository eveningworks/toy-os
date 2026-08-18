// Memory stress and INTEGRITY: take everything sbrk will give, write a
// pattern that depends on the address, and read it back.
//
// WHY THE PATTERN DEPENDS ON THE ADDRESS, which is the whole design.
// Filling memory with a constant proves almost nothing: if two virtual
// pages end up pointing at the SAME physical frame -- the bug you get
// when the allocator hands out a frame that is still mapped somewhere
// -- both read back the constant and look perfect. A value derived
// from the address means an aliased page reads back the OTHER page's
// value, and the mismatch names both.
//
// The salt makes it work ACROSS processes too. Each run picks a random
// one, so two copies writing the same virtual address write different
// bytes, and cross-process aliasing shows up as a value that is a valid
// pattern for somebody else's salt. The mix is deliberately invertible
// for exactly that reason: on a mismatch this recovers the salt whose
// pattern the memory DOES hold, which turns "memory is wrong" into
// "this is another process's data".
//
// What it does NOT do is exhaust the machine's RAM: SYS_SBRK is bounded
// per process by UADDR_HEAP_LIMIT (~14 MiB), so one copy tests the
// bound and its own integrity. Filling a machine takes several copies
// at once -- tools/mem_stress.py drives that, with a smaller guest so
// the total is actually reachable.
//
//   run/spawn: spawn /tests/memtest          (as much as sbrk allows)
//              spawn /tests/memtest 4        (cap at 4 MiB, for a quick pass)
#include "rt/sys.h"
#include "lib/stdio.h"
#include "lib/string.h"

#define CHUNK (64 * 1024)     // one sbrk step
#define MIN_USEFUL_BYTES (1 << 20) // below this the test proves nothing -- see below

// Invertible on purpose: salt == value ^ (va * K), so a mismatch can be
// asked "whose pattern IS this?".
#define MIX_K 0x9E3779B97F4A7C15ULL

static uint64_t pattern(uint64_t va, uint64_t salt) {
    return (va * MIX_K) ^ salt;
}

static void say(const char *s) { sys_eprint(s); }

int main(int argc, char **argv) {
    uint64_t cap = 0; // 0 = no cap
    if (argc > 1) {
        uint64_t mib = 0;
        for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++) mib = mib * 10 + (uint64_t)(*p - '0');
        cap = mib << 20;
    }

    uint64_t salt = 0;
    if (sys_getrandom(&salt, sizeof salt) != (long)sizeof salt || salt == 0) {
        // A zero salt would make the pattern purely address-derived and
        // identical in every process, which is exactly the case this
        // test exists to distinguish. Refuse rather than run blind.
        say("memtest: FAIL -- no salt (getrandom)\n");
        return 1;
    }

    char line[224]; // the aliasing diagnosis is long -- 128 truncated it
    snprintf(line, sizeof line, "memtest: salt 0x%lx\n", salt);
    say(line);

    // --- take everything sbrk will give -----------------------------
    uint8_t *base = (uint8_t *)sys_sbrk(0);
    if (base == (uint8_t *)-1) { say("memtest: FAIL -- sbrk(0) refused\n"); return 1; }

    uint64_t got = 0;
    int refusals = 0;
    while (!cap || got < cap) {
        void *p = sys_sbrk(CHUNK);
        if (p == (void *)-1) { refusals++; break; }
        got += CHUNK;
    }

    snprintf(line, sizeof line, "memtest: got %lu KiB (%lu chunks), sbrk %s\n",
             got / 1024, got / CHUNK, refusals ? "refused as expected" : "capped by request");
    say(line);

    // A run that got almost nothing cannot detect anything, and must
    // say so rather than passing: this is the fixture-too-small trap --
    // a green result whose input never reached the code under test.
    if (got < MIN_USEFUL_BYTES) {
        say("memtest: FAIL -- got too little memory to prove anything\n");
        return 1;
    }

    // --- write, then verify -----------------------------------------
    //
    // Two passes, not one: writing and checking each word immediately
    // would keep it in cache and in a register, and would not notice a
    // page whose mapping changes under it later in the run.
    uint64_t words = got / 8;
    volatile uint64_t *m = (volatile uint64_t *)base;
    for (uint64_t i = 0; i < words; i++) {
        m[i] = pattern((uint64_t)(uintptr_t)&m[i], salt);
    }
    say("memtest: written, verifying\n");

    uint64_t bad = 0;
    for (uint64_t i = 0; i < words; i++) {
        uint64_t va = (uint64_t)(uintptr_t)&m[i];
        uint64_t want = pattern(va, salt);
        uint64_t have = m[i];
        if (have == want) continue;
        if (bad == 0) {
            // The first mismatch gets the full diagnosis. `other` is
            // the salt this memory's value WOULD be a valid pattern
            // for at this address. If it is our own salt the write was
            // simply lost or torn; if it is not, the memory holds a
            // pattern written at a DIFFERENT address or by a different
            // process -- which is an aliased frame either way.
            uint64_t other = have ^ (va * MIX_K);
            snprintf(line, sizeof line,
                     "memtest: MISMATCH at 0x%lx: want 0x%lx have 0x%lx\n", va, want, have);
            say(line);
            snprintf(line, sizeof line,
                     "memtest:   %s (implied salt 0x%lx)\n",
                     other == salt ? "our own salt: a lost or torn write"
                                   : "NOT our salt: a pattern from another address "
                                     "or another process -- an ALIASED frame",
                     other);
            say(line);
        }
        bad++;
    }

    if (bad) {
        snprintf(line, sizeof line, "memtest: FAIL -- %lu of %lu words wrong\n", bad, words);
        say(line);
        return 1;
    }

    snprintf(line, sizeof line, "memtest: PASSED -- %lu KiB written and verified\n", got / 1024);
    say(line);
    return 0;
}
