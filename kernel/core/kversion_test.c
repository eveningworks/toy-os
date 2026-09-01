// The kernel's own build identity, as a queryable fact.
//
// WHAT THIS IS FOR, and it is not "does snprintf work": the strings are
// compiled in, so the only ways they can be wrong are a provider that
// answers the wrong class, a record that does not match what ring 3
// copies out, or a field that silently truncates. The last is the real
// one -- build_id is 24 bytes and a tag-plus-dirty build id is not
// obviously shorter than that.
#include "ktest.h"
#include "query.h"
#include "kversion.h"
#include "string.h"
#include "version.h"

KTEST("kversion", "the provider answers with this build's identity") {
    struct query_version v;
    k_memset(&v, 0, sizeof v);
    KTEST_ASSERT_EQ(query_read(QUERY_VERSION, 0, &v, sizeof v),
                    (int)sizeof v);

    KTEST_ASSERT(k_strcmp(v.version, TOYOS_VERSION) == 0);
    KTEST_ASSERT(k_strcmp(v.build_id, TOYOS_BUILD_ID) == 0);
    // The stamp is generated, so it cannot be compared against a
    // constant -- but "YYYY-MM-DD HH:MM:SS" is 19 characters and a
    // truncation would show up as a short string.
    KTEST_ASSERT_EQ((int)k_strlen(v.stamp), 19);
}

KTEST("kversion", "nothing was truncated into the record") {
    // Each field must have room for its value PLUS the NUL. A silent
    // truncation here would report a build id that is a prefix of the
    // real one -- which compares unequal to userland's and would raise
    // a mismatch warning on a machine that has none.
    KTEST_ASSERT(k_strlen(TOYOS_VERSION)  < 16);
    KTEST_ASSERT(k_strlen(TOYOS_BUILD_ID) < 24);

    struct query_version v;
    k_memset(&v, 0, sizeof v);
    query_read(QUERY_VERSION, 0, &v, sizeof v);
    KTEST_ASSERT_EQ((int)k_strlen(v.build_id), (int)k_strlen(TOYOS_BUILD_ID));
}

KTEST("kversion", "the banner names the version, the build and the time") {
    const char *b = kversion_banner();
    KTEST_ASSERT(b && b[0]);
    KTEST_ASSERT(k_strstr(b, TOYOS_VERSION) != 0);
    KTEST_ASSERT(k_strstr(b, TOYOS_BUILD_ID) != 0);
    // "built " then a date -- the part a captured log is read for.
    KTEST_ASSERT(k_strstr(b, "built 20") != 0);
}
