// /tests/regex_test -- tolibc's <regex.h> in ring 3.
//
// Runs userland/tests/regex_cases.h, the same table a HOST harness runs
// against this same regex.c (see tools/regex_hostcheck.py, which also
// runs it against glibc as an independent oracle). The host run is what
// makes iterating on the engine bearable; THIS run is what proves the
// same source behaves the same way compiled for ring 3, against
// tolibc's own malloc/ctype/string rather than the host's -- the gap
// kfmt_cases.h exists to close, and the one where a shared-source file
// has actually drifted before.
//
// It also exercises what the host cannot: this engine allocating from
// the ring-3 heap, under a 2 KiB frame budget, in a process that a
// leak would eventually kill.
#include <regex.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "regex_cases.h"

static int failures;

static void fail(const char *fmt, const char *a, const char *b) {
    printf("FAIL: ");
    printf(fmt, a, b);
    printf("\n");
    failures++;
}

int main(void) {
    int run = 0;

    for (int i = 0; i < RX_CASE_COUNT; i++) {
        const struct rx_case *c = &RX_CASES[i];
        regex_t re;
        int cf = REG_EXTENDED | (c->icase ? REG_ICASE : 0);
        int rc = regcomp(&re, c->pattern, cf);
        run++;
        if (rc != 0) {
            char eb[96];
            regerror(rc, &re, eb, sizeof eb);
            fail("/%s/ did not compile: %s", c->pattern, eb);
            continue;
        }
        regmatch_t m[10];
        int got = regexec(&re, c->input, 10, m, 0) == 0;
        if (got != c->expect) {
            fail("/%s/ vs \"%s\": wrong match/no-match", c->pattern, c->input);
        } else if (got && c->so >= 0 &&
                   (m[0].rm_so != c->so || m[0].rm_eo != c->eo)) {
            char buf[64];
            snprintf(buf, sizeof buf, "[%d,%d) want [%d,%d)",
                     (int)m[0].rm_so, (int)m[0].rm_eo, c->so, c->eo);
            fail("/%s/ span %s", c->pattern, buf);
        }
        regfree(&re);
    }

    // Every pattern the header says is invalid must be REFUSED. A
    // regex engine that accepts nonsense matches something, and what
    // it matches is nobody's intent.
    for (int i = 0; i < RX_BAD_COUNT; i++) {
        regex_t re;
        run++;
        if (regcomp(&re, RX_BAD[i].pattern, REG_EXTENDED) == 0) {
            fail("/%s/ compiled but should be refused (%s)",
                 RX_BAD[i].pattern, RX_BAD[i].why);
            regfree(&re);
        }
    }

    // BRE, which the table above does not cover because grep uses ERE.
    // The point is only that the dialect switch works: in BRE `+` is a
    // literal and grouping is spelled \( \).
    {
        regex_t re;
        run++;
        if (regcomp(&re, "a+b", 0) != 0) {
            fail("BRE /%s/ did not compile%s", "a+b", "");
        } else {
            regmatch_t m[1];
            if (regexec(&re, "a+b", 1, m, 0) != 0)
                fail("BRE /%s/ should match a literal plus%s", "a+b", "");
            if (regexec(&re, "aab", 1, m, 0) == 0)
                fail("BRE /%s/ must NOT be a repetition%s", "a+b", "");
            regfree(&re);
        }
        run++;
        if (regcomp(&re, "\\(ab\\)*c", 0) != 0) {
            fail("BRE /%s/ did not compile%s", "\\(ab\\)*c", "");
        } else {
            regmatch_t m[2];
            if (regexec(&re, "ababc", 2, m, 0) != 0)
                fail("BRE grouping %s failed%s", "\\(ab\\)*c", "");
            regfree(&re);
        }
    }

    // REG_NOSUB and the eflags, which the table does not reach.
    {
        regex_t re;
        run++;
        if (regcomp(&re, "^abc", REG_EXTENDED) == 0) {
            regmatch_t m[1];
            if (regexec(&re, "abc", 1, m, REG_NOTBOL) == 0)
                fail("REG_NOTBOL should stop ^ matching at offset 0%s%s", "", "");
            regfree(&re);
        }
        run++;
        if (regcomp(&re, "c$", REG_EXTENDED) == 0) {
            regmatch_t m[1];
            if (regexec(&re, "abc", 1, m, REG_NOTEOL) == 0)
                fail("REG_NOTEOL should stop $ matching at the end%s%s", "", "");
            regfree(&re);
        }
    }

    // THE LEAK CHECK, and the reason it is here rather than on the
    // host: regcomp() allocates three blocks and regexec() five more,
    // every call. A miss would not show on a single-shot host run and
    // would kill a long grep. Compiling and freeing many times must not
    // grow the process.
    {
        run++;
        void *before = malloc(1);
        free(before);
        for (int i = 0; i < 200; i++) {
            regex_t re;
            if (regcomp(&re, "(a|b)+[0-9]{2,3}c*$", REG_EXTENDED) != 0) {
                fail("leak loop: compile failed%s%s", "", "");
                break;
            }
            regmatch_t m[4];
            regexec(&re, "abab42ccc", 4, m, 0);
            regfree(&re);
        }
        void *after = malloc(1);
        free(after);
        // Not an equality check -- the allocator may legitimately have
        // grown a region. What must not happen is the heap marching
        // away, so compare against a generous bound rather than
        // pretending to measure exactly.
        long drift = (long)((char *)after - (char *)before);
        if (drift < 0) drift = -drift;
        if (drift > 64 * 1024) {
            char buf[64];
            snprintf(buf, sizeof buf, "%ld bytes", drift);
            fail("200 compile/exec/free cycles moved the heap by %s%s", buf, "");
        }
    }

    if (failures) {
        printf("regex_test: FAILED -- %d of %d checks\n", failures, run);
        return 1;
    }
    printf("regex_test: all %d checks passed\n", run);
    return 0;
}
