// A window with an on_tick and NO tick_ms -- the default cadence's
// fixture (tools/uapp_test.py).
//
// Before UAPP_TICK_DEFAULT_MS an app like this polled: on_tick on every
// pass of a loop that never blocked, a hundred-plus calls a second at
// full CPU. Now it is a 33 ms timer. Nothing else in the tree reaches
// that default -- every real app names its rate, and DOOM polls on
// purpose -- so without this the default is a branch no test runs.
//
// It counts ticks and says how many arrived in each two-second window,
// which is the number a polling build cannot fake.
#include "rt/sys.h"
#include "ui/uapp.h"
#include "ui/ulog.h"

static unsigned g_ticks;
static uint64_t g_window_start;

static uint64_t now_ms(void) { return sys_monotonic_ns() / 1000000ull; }

static int on_tick(struct uapp *a) {
    (void)a;
    if (!g_window_start) g_window_start = now_ms();
    g_ticks++;
    uint64_t span = now_ms() - g_window_start;
    if (span >= 2000) {
        ulogf("tickclient: %u ticks in %u ms\n", g_ticks, (unsigned)span);
        g_ticks = 0;
        g_window_start = now_ms();
    }
    return 0;
}

int main(void) {
    struct uapp_desc desc = {
        .title   = "Tick Client",
        .w       = 200,
        .h       = 80,
        .on_tick = on_tick,
    };
    return uapp_run(&desc);
}
