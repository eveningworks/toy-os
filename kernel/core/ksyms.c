// Address -> function name, for panics. See ksyms.h for the contract
// and tools/gen_syms.py for where the table comes from.
//
// The blob is a flat, self-describing byte array with no pointers in it
// (that is the whole design -- a table of pointers would add one
// relocation per symbol to the ~8,000 the kernel already patches at
// boot). Everything here therefore reads unaligned bytes by hand rather
// than casting, which also means the section needs no particular
// alignment to be safe.

#include "ksyms.h"
#include "reloc.h"

extern const unsigned char __ksyms_start[];
extern const unsigned char __ksyms_end[];

#define KSYMS_MAGIC 0x534D5953u // "SYMS", matching tools/gen_syms.py

static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// The table, or NULL if this image was built without one. Validated on
// every call rather than cached: this runs from a fault handler, where
// trusting state written earlier is exactly the wrong instinct.
static const unsigned char *table(uint32_t *out_count, uint32_t *out_strings) {
    if (__ksyms_end <= __ksyms_start) return 0;
    if ((uint64_t)(__ksyms_end - __ksyms_start) < 12) return 0;
    if (rd32(__ksyms_start) != KSYMS_MAGIC) return 0;

    uint32_t count = rd32(__ksyms_start + 4);
    uint32_t strings = rd32(__ksyms_start + 8);
    uint64_t span = (uint64_t)(__ksyms_end - __ksyms_start);
    // A truncated or corrupted section must report "no symbols", not
    // walk off the end while the machine is already panicking.
    if ((uint64_t)strings > span) return 0;
    if ((uint64_t)12 + (uint64_t)count * 8 > (uint64_t)strings) return 0;

    if (out_count) *out_count = count;
    if (out_strings) *out_strings = strings;
    return __ksyms_start;
}

uint32_t ksyms_count(void) {
    uint32_t count = 0;
    return table(&count, 0) ? count : 0;
}

const char *ksyms_lookup(uint64_t addr, uint32_t *out_off) {
    if (out_off) *out_off = 0;

    uint32_t count = 0, strings = 0;
    const unsigned char *t = table(&count, &strings);
    if (!t || count == 0) return 0;

    // The table holds LINK-TIME addresses; the caller has a running one.
    // Undo the boot relocation rather than making every caller do it --
    // forgetting this step is how a lookup silently finds nothing on
    // every boot but the one where the kernel happened not to move.
    uint64_t delta = kernel_reloc_delta();
    if (addr < delta) return 0;
    uint64_t link = addr - delta;
    if (link > 0xFFFFFFFFu) return 0;
    uint32_t want = (uint32_t)link;

    const unsigned char *ents = t + 12;
    if (want < rd32(ents)) return 0; // below the first function

    // Greatest entry <= want. Binary search because a fault handler
    // should not walk two thousand entries with the machine already in
    // trouble.
    uint32_t lo = 0, hi = count - 1, best = 0;
    while (lo <= hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t a = rd32(ents + (uint64_t)mid * 8);
        if (a <= want) {
            best = mid;
            if (mid == count - 1) break;
            lo = mid + 1;
        } else {
            if (mid == 0) break;
            hi = mid - 1;
        }
    }

    uint32_t base = rd32(ents + (uint64_t)best * 8);
    uint32_t noff = rd32(ents + (uint64_t)best * 8 + 4);
    if ((uint64_t)strings + noff >= (uint64_t)(__ksyms_end - __ksyms_start))
        return 0;

    if (out_off) *out_off = want - base;
    return (const char *)(t + strings + noff);
}
