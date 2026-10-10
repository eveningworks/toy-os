// lib/uclock.h: the clock's observed granularity is a real figure.
//
// The trap it covers is the tick clock (`clocksource=pit`): 64 adjacent
// reads land inside one tick, and the answer used to come back 0 --
// "perfect precision" for the coarsest clock there is. On the default
// clocksources the first pass answers and the fallback never runs, so
// this test only discriminates on a `clocksource=pit` boot.
#include <stdio.h>
#include "rt/sys.h"
#include "lib/utest.h"
#include "lib/uclock.h"

int main(void) {
    utest_begin("uclock_test", "lib/uclock.h", UTEST_VERDICT_FILE);

    uint64_t g = uclock_granularity_ns();
    char detail[64];
    snprintf(detail, sizeof detail, "%llu ns", (unsigned long long)g);
    utest_check_detail(g != 0, "the granularity is never reported as 0", detail);
    utest_check_detail(g != UCLOCK_NEVER_MOVED, "the clock was seen to advance", detail);
    // The PIT's tick is the coarsest clocksource registered.
    utest_check_detail(g <= 20000000ull, "it is no coarser than a PIT tick", detail);
    utest_notef("granularity %llu ns", (unsigned long long)g);

    return utest_end();
}
