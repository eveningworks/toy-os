// The entropy source, as a queryable FACT.
//
// SYS_GETRANDOM deliberately does not report quality -- abi/syscall_abi.h
// says so and gives the reason: a program checking it per draw would
// mostly use it to decide to carry on anyway. That reasoning is about a
// GUARANTEE attached to bytes, and it still holds.
//
// This is the other question. "What is this machine's entropy source?"
// is a diagnostic with a human audience, and it was the whole point of
// the kernel shell's `random` command -- the numbers look equally
// random whatever produced them, and the source is the only part a
// reader can actually judge. Ring 3 could not ask it at all, which is
// why `random` stayed a builtin. A provider answers it without putting
// a promise on SYS_GETRANDOM that this kernel cannot keep.
#include "query.h"
#include "krandom.h"
#include "string.h"
#include <stddef.h>

static int random_count(void) { return 1; } // scalar

static int random_fill(int index, void *out) {
    if (index != 0) return 0;
    struct query_random *r = out;
    k_memset(r, 0, sizeof *r);
    // The ABI numbers are the enum's numbers -- QUERY_RANDOM_* is
    // asserted against enum krandom_quality below rather than mapped,
    // so there is no translation table to keep true.
    r->quality = (uint64_t)krandom_quality();
    k_strlcpy(r->name, krandom_quality_name(krandom_quality()), sizeof r->name);
    return 1;
}

// The ABI numbers ARE the enum's, so a mismatch is a build error rather
// than a wrong answer at runtime. Without this the two could drift and
// the only symptom would be a program reporting the wrong source, which
// is worse than no report at all.
_Static_assert((int)KRANDOM_NONE   == QUERY_RANDOM_NONE,   "krandom/query enum drift");
_Static_assert((int)KRANDOM_JITTER == QUERY_RANDOM_JITTER, "krandom/query enum drift");
_Static_assert((int)KRANDOM_VIRTIO == QUERY_RANDOM_VIRTIO, "krandom/query enum drift");
_Static_assert((int)KRANDOM_HW     == QUERY_RANDOM_HW,     "krandom/query enum drift");

static const struct query_field random_fields[] = {
    QUERY_FIELD(struct query_random, quality, QUERY_TYPE_U64),
};

static const struct query_provider random_provider = {
    .cls = QUERY_RANDOM,
    .name = "random",
    .record_size = sizeof(struct query_random),
    .flags = 0, // scalar
    .count = random_count,
    .fill = random_fill,
    .fields = random_fields,
    .field_count = sizeof random_fields / sizeof random_fields[0],
};

void krandom_query_init(void) {
    query_register(&random_provider);
}
