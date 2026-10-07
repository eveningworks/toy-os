// Tests for kslots.c -- the table pipes, sockets and TCP connections
// live in.
//
// WHAT A BROKEN VERSION WOULD STILL PASS: a table that never grew would
// pass every check under eight objects, so one test allocates well past
// that; one that moved objects on growth would pass index lookups, so a
// pointer taken BEFORE the growth is compared after it; and one that
// handed a reused slot back dirty would pass on fresh memory, so a slot
// is written, freed and taken again.
#include "ktest.h"
#include "kslots.h"
#include "string.h"

struct thing { uint32_t a, b; char name[24]; };

KTEST("kslots", "it grows past its first size, and indices stay put") {
    struct kslots t = KSLOTS_INIT(sizeof(struct thing));   // per run; its few KiB are not returned
    int idx[40];
    struct thing *first = 0;
    for (int i = 0; i < 40; i++) {
        idx[i] = kslots_alloc(&t);
        KTEST_ASSERT(idx[i] == i);
        struct thing *p = kslots_at(&t, idx[i]);
        KTEST_ASSERT(p != 0);
        p->a = (uint32_t)i;
        if (i == 0) first = p;
    }
    KTEST_ASSERT(kslots_cap(&t) >= 40);
    KTEST_ASSERT(kslots_live(&t) == 40);
    // The address taken at index 0 before four growths is still the one.
    KTEST_ASSERT(kslots_at(&t, 0) == first);
    for (int i = 0; i < 40; i++)
        KTEST_ASSERT(((struct thing *)kslots_at(&t, idx[i]))->a == (uint32_t)i);
}

KTEST("kslots", "a freed slot reads as empty, and comes back zeroed") {
    struct kslots t = KSLOTS_INIT(sizeof(struct thing));   // per run; its few KiB are not returned
    int a = kslots_alloc(&t), b = kslots_alloc(&t);
    struct thing *pa = kslots_at(&t, a);
    pa->a = 0xDEADBEEF;
    k_strlcpy(pa->name, "dirty", sizeof pa->name);
    kslots_free(&t, a);
    KTEST_ASSERT(kslots_at(&t, a) == 0);
    KTEST_ASSERT(kslots_at(&t, b) != 0);
    KTEST_ASSERT(kslots_live(&t) == 1);
    // The lowest free slot is reused -- its memory, zeroed.
    int c = kslots_alloc(&t);
    KTEST_ASSERT(c == a);
    struct thing *pc = kslots_at(&t, c);
    KTEST_ASSERT(pc == pa);
    KTEST_ASSERT(pc->a == 0 && pc->name[0] == 0);
}

KTEST("kslots", "out-of-range and never-used indices are NULL") {
    struct kslots t = KSLOTS_INIT(sizeof(struct thing));   // per run; its few KiB are not returned
    KTEST_ASSERT(kslots_at(&t, 0) == 0);
    KTEST_ASSERT(kslots_at(&t, -1) == 0);
    int a = kslots_alloc(&t);
    KTEST_ASSERT(kslots_at(&t, a + 1) == 0);
    KTEST_ASSERT(kslots_at(&t, 100000) == 0);
    kslots_free(&t, -5);   // no effect, no fault
    kslots_free(&t, 100000);
    KTEST_ASSERT(kslots_live(&t) == 1);
}
