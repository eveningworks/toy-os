// lib/uduration.h and /bin/sleep.
//
// The parser against a table whose answers were worked out by hand (and
// agree with GNU sleep's reading of each operand), then /bin/sleep end
// to end: it waits at least as long as asked, adds its operands, and
// refuses junk with exit 1 and no wait.
#include <stdarg.h>
#include <stdio.h>
#include "rt/sys.h"
#include "lib/utest.h"
#include "lib/uduration.h"

static const struct { const char *in; int ok; uint64_t ms; } CASES[] = {
    { "0",        1, 0 },
    { "2",        1, 2000 },
    { "0.25",     1, 250 },
    { ".5",       1, 500 },
    { "1.",       1, 1000 },
    { "90s",      1, 90000 },
    { "1.5m",     1, 90000 },
    { "2h",       1, 7200000 },
    { "1d",       1, 86400000 },
    { "0.001",    1, 1 },
    { "0.0001",   1, 1 },          // below a millisecond rounds UP
    { "0.0000000000001", 1, 1 },   // ...even past nine fraction digits
    { "1.0000000000",    1, 1000 },// trailing zeros are not a remainder
    { "0.1m",     1, 6000 },
    { "",         0, 0 },
    { ".",        0, 0 },
    { "s",        0, 0 },
    { "-1",       0, 0 },
    { "+1",       0, 0 },
    { "1e3",      0, 0 },
    { "inf",      0, 0 },
    { "5ms",      0, 0 },
    { "1m30s",    0, 0 },
    { "1 s",      0, 0 },
    { "1x",       0, 0 },
    { "99999999999999999999", 0, 0 },   // past 64 bits
    { "999999999999999d",     0, 0 },   // the unit overflows it
};

// A check whose detail -- shown only on a failure -- is formatted.
__attribute__((format(printf, 3, 4)))
static void checkd(int ok, const char *what, const char *fmt, ...) {
    char detail[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof detail, fmt, ap);
    va_end(ap);
    utest_check_detail(ok, what, detail);
}

// Runs /bin/sleep with `args`; returns its exit code, *ms the wall time.
static int run_sleep(const char *args, uint64_t *ms) {
    uint64_t t0 = sys_monotonic_ns();
    int pid = sys_spawn("/bin/sleep", args, -1);
    if (pid <= 0) return -1000;
    int code = -1;
    sys_waitpid(pid, &code);
    *ms = (sys_monotonic_ns() - t0) / 1000000;
    return code;
}

int main(void) {
    utest_begin("duration_test", "lib/uduration.h and /bin/sleep", UTEST_VERDICT_FILE);

    for (unsigned i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        uint64_t got = 12345;
        int ok = uduration_parse_ms(CASES[i].in, &got);
        char what[96];
        snprintf(what, sizeof what, "\"%s\" %s", CASES[i].in,
                 CASES[i].ok ? "parses" : "is refused");
        if (CASES[i].ok)
            checkd(ok && got == CASES[i].ms, what, "ok %d got %llu want %llu", ok,
                         (unsigned long long)got, (unsigned long long)CASES[i].ms);
        else
            checkd(!ok && got == 12345, what, "ok %d got %llu", ok,
                         (unsigned long long)got);
    }

    // The upper bounds are generous: a spawn and a scheduler rotation on
    // a loaded TCG guest. The LOWER bound is the property.
    uint64_t ms;
    int code = run_sleep("0.3", &ms);
    checkd(code == 0 && ms >= 300 && ms < 3000, "sleep 0.3 waits 300 ms, then exits 0",
                 "exit %d after %llu ms", code, (unsigned long long)ms);
    code = run_sleep("0.2 0.2", &ms);
    checkd(code == 0 && ms >= 400 && ms < 3000, "sleep 0.2 0.2 adds its operands",
                 "exit %d after %llu ms", code, (unsigned long long)ms);
    code = run_sleep("1x", &ms);
    checkd(code == 1 && ms < 1000, "sleep 1x is refused with exit 1, without waiting",
                 "exit %d after %llu ms", code, (unsigned long long)ms);
    code = run_sleep("", &ms);
    checkd(code == 1, "sleep with no operand is a usage error", "exit %d", code);

    return utest_end();
}
