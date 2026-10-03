// QUERY_CMDLINE: the slices must reassemble to exactly the tag's string.
// The boundary is the trap -- a slice that drops or repeats the byte at
// a multiple of QUERY_CMDLINE_PART reads fine on every short line.
#include "ktest.h"
#include "query.h"
#include "multiboot.h"
#include "string.h"
#include "errno.h"

KTEST("cmdline", "the slices reassemble to the multiboot command line") {
    const char *want = multiboot_cmdline();
    int len = want ? (int)k_strlen(want) : 0;
    struct query_cmdline q;
    int got = 0, i = 0;
    while (query_read(QUERY_CMDLINE, i, &q, sizeof q) == (int)sizeof q) {
        int n = (int)k_strlen(q.part);
        KTEST_ASSERT(n > 0 && n <= QUERY_CMDLINE_PART);
        KTEST_ASSERT(got + n <= len);
        KTEST_ASSERT(k_memcmp(q.part, want + got, (size_t)n) == 0);
        got += n;
        i++;
    }
    KTEST_ASSERT_EQ(got, len);
    KTEST_ASSERT_EQ(query_read(QUERY_CMDLINE, i, &q, sizeof q), -ERANGE);
}
