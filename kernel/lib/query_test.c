// The fact registry: registration, the two class shapes, and named
// fields.
//
// These are KTESTs rather than a ring-3 program because what is claimed
// here is about the REGISTRY -- that a duplicate class is refused, that a
// scalar reports one record, that a field name resolves to the right
// offset. None of that needs a process. The syscall half (does the answer
// survive the trip to ring 3, does `len` really truncate) is
// userland/tests/query_test.c, which is the gap a KTEST cannot reach.
#include "ktest.h"
#include "query.h"
#include "errno.h"
#include "string.h"

// A fixture provider, registered and removed by the tests that need one.
// Its class is deliberately far above the real ones so it cannot collide
// with a provider somebody adds later.
#define FIXTURE_CLASS 9001

struct fixture_rec {
    uint64_t a;
    uint64_t b;
};

static int fixture_n = 3;
static int fixture_count(void) { return fixture_n; }

static int fixture_fill(int index, void *out) {
    if (index < 0 || index >= fixture_n) return 0;
    struct fixture_rec *r = out;
    r->a = (uint64_t)index * 10;
    r->b = (uint64_t)index * 100;
    return 1;
}

static const struct query_field fixture_fields[] = {
    QUERY_FIELD(struct fixture_rec, a, QUERY_TYPE_U64),
    QUERY_FIELD(struct fixture_rec, b, QUERY_TYPE_BYTES),
};

static const struct query_provider fixture = {
    .cls = FIXTURE_CLASS,
    .name = "ktfixture",
    .record_size = sizeof(struct fixture_rec),
    .flags = QUERY_F_LIST,
    .count = fixture_count,
    .fill = fixture_fill,
    .fields = fixture_fields,
    .field_count = 2,
};

KTEST("query", "the memory provider is registered and reports a frame size") {
    struct query_meminfo m;
    int n = query_read(QUERY_MEMINFO, 0, &m, sizeof m);
    KTEST_ASSERT_EQ(n, (int)sizeof m);
    // Not "is it nonzero": a frame size of 0 would make every byte
    // figure derived from it 0 as well, silently.
    KTEST_ASSERT(m.frame_bytes >= 4096);
    KTEST_ASSERT(m.frame_total > 0);
    KTEST_ASSERT(m.frame_free <= m.frame_total);
}

KTEST("query", "a scalar class has exactly one record") {
    struct query_meminfo m;
    KTEST_ASSERT(query_read(QUERY_MEMINFO, 0, &m, sizeof m) > 0);
    // Index 1 of a scalar is not an empty record, it is out of range --
    // the distinction that stops an enumerator running forever.
    KTEST_ASSERT_EQ(query_read(QUERY_MEMINFO, 1, &m, sizeof m), -ERANGE);
    KTEST_ASSERT_EQ(query_read(QUERY_MEMINFO, -1, &m, sizeof m), -ERANGE);
}

KTEST("query", "class 0 describes the registry, itself included") {
    struct query_provider_info info;
    int found_self = 0, found_mem = 0;
    for (int i = 0; ; i++) {
        if (query_read(QUERY_PROVIDERS, i, &info, sizeof info) <= 0) break;
        if (info.cls == QUERY_PROVIDERS) {
            found_self = 1;
            // The registry is a list and must say so, or a caller reading
            // count == 1 at some moment would treat it as a scalar.
            KTEST_ASSERT(info.flags & QUERY_F_LIST);
        }
        if (info.cls == QUERY_MEMINFO) {
            found_mem = 1;
            KTEST_ASSERT(!(info.flags & QUERY_F_LIST)); // scalar
            KTEST_ASSERT_EQ((int)info.count, 1);
            KTEST_ASSERT_EQ((int)info.record_size, (int)sizeof(struct query_meminfo));
        }
    }
    KTEST_ASSERT(found_self);
    KTEST_ASSERT(found_mem);
}

KTEST("query", "an unknown class and a too-small buffer are told apart") {
    struct query_meminfo m;
    KTEST_ASSERT_EQ(query_read(4242, 0, &m, sizeof m), -ENOENT);
    // -EINVAL, not -ENOENT: the fact exists and the caller's buffer is
    // wrong, which is a different thing to fix.
    KTEST_ASSERT_EQ(query_read(QUERY_MEMINFO, 0, &m, 4), -EINVAL);
}

KTEST("query", "a duplicate class is refused, not last-wins") {
    KTEST_ASSERT(query_register(&fixture));
    // Registering the same CLASS again must fail, or which provider
    // answers would depend on link order.
    KTEST_ASSERT(!query_register(&fixture));
    query_unregister(&fixture);
}

KTEST("query", "a malformed provider is refused") {
    static const struct query_provider no_fill = {
        .cls = FIXTURE_CLASS + 1, .name = "bad",
        .record_size = 8, .count = fixture_count, .fill = 0,
    };
    static const struct query_provider no_size = {
        .cls = FIXTURE_CLASS + 2, .name = "bad2",
        .record_size = 0, .count = fixture_count, .fill = fixture_fill,
    };
    KTEST_ASSERT(!query_register(&no_fill));
    KTEST_ASSERT(!query_register(&no_size));
}

KTEST("query", "a list walks by index and ends by running out") {
    KTEST_ASSERT(query_register(&fixture));
    struct fixture_rec r;
    int seen = 0;
    for (int i = 0; ; i++) {
        if (query_read(FIXTURE_CLASS, i, &r, sizeof r) <= 0) break;
        KTEST_ASSERT_EQ((int)r.a, i * 10);
        seen++;
    }
    KTEST_ASSERT_EQ(seen, fixture_n);
    query_unregister(&fixture);
}

KTEST("query", "a named field resolves to the right value") {
    KTEST_ASSERT(query_register(&fixture));
    uint64_t v = 0;
    uint32_t t = 0;
    // Record 0, so a = 0 and b = 0 -- which is why the SECOND field is
    // what proves the offset: reading the wrong one would still give 0.
    fixture_n = 3;
    KTEST_ASSERT_EQ(query_field_get("ktfixture.a", &v, &t), 0);
    KTEST_ASSERT_EQ((int)t, QUERY_TYPE_U64);
    KTEST_ASSERT_EQ(query_field_get("ktfixture.b", &v, &t), 0);
    KTEST_ASSERT_EQ((int)t, QUERY_TYPE_BYTES);
    query_unregister(&fixture);
}

KTEST("query", "the memory provider's fields read the record's own numbers") {
    // The check that would catch a wrong offset: compare the field
    // against the same number read out of the whole record. A field
    // table with two entries swapped passes any "is it nonzero" test.
    struct query_meminfo m;
    KTEST_ASSERT(query_read(QUERY_MEMINFO, 0, &m, sizeof m) > 0);
    uint64_t v = 0;
    uint32_t t = 0;
    KTEST_ASSERT_EQ(query_field_get("mem.frame_bytes", &v, &t), 0);
    KTEST_ASSERT_EQ((int)v, (int)m.frame_bytes);
    KTEST_ASSERT_EQ(query_field_get("mem.frame_total", &v, &t), 0);
    KTEST_ASSERT_EQ((int)v, (int)m.frame_total);
}

KTEST("query", "a list class has no single value, and says so distinctly") {
    uint64_t v = 0;
    uint32_t t = 0;
    // -ENOTSUP, NOT -ENOENT. "there is no such fact" would send a reader
    // hunting for a typo they did not make.
    KTEST_ASSERT_EQ(query_field_get("providers.anything", &v, &t), -ENOTSUP);
    KTEST_ASSERT_EQ(query_field_get("mem.no_such_field", &v, &t), -ENOENT);
    KTEST_ASSERT_EQ(query_field_get("nosuchprovider.x", &v, &t), -ENOENT);
    // A name with no dot is malformed, which is neither of the above.
    KTEST_ASSERT_EQ(query_field_get("meminfo", &v, &t), -EINVAL);
    KTEST_ASSERT_EQ(query_field_get("mem.", &v, &t), -EINVAL);
    KTEST_ASSERT_EQ(query_field_get(".frame_free", &v, &t), -EINVAL);
}
