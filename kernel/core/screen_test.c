// KTESTs for the runtime mode change. The change itself is driven by
// tools/modeset_test.py through the real setting under a live desktop
// (a QMP screendump's own size is the oracle); these cover the contract
// the drivers keep for it.
#include "screen.h"
#include "display.h"
#include "gfx.h"
#include "ktest.h"

KTEST("screen", "the current mode is in the list, and the list is empty only without MODESET") {
    int n = display_mode_count();
    if (!display_has(DISPLAY_CAP_MODESET)) {
        KTEST_ASSERT_EQ(n, 0);
        KTEST_ASSERT_EQ(screen_mode_listed((uint32_t)gfx_width(), (uint32_t)gfx_height()), 0);
        return;
    }
    KTEST_ASSERT(n > 0);
    KTEST_ASSERT(screen_mode_listed((uint32_t)gfx_width(), (uint32_t)gfx_height()));
    KTEST_ASSERT_EQ(screen_mode_listed(7, 7), 0);
}

KTEST("screen", "setting the current mode is a no-op that succeeds, and an unlisted one is refused") {
    int w = gfx_width(), h = gfx_height();
    KTEST_ASSERT_EQ(screen_set_mode((uint32_t)w, (uint32_t)h), 1);
    KTEST_ASSERT_EQ(screen_set_mode(7, 7), 0);
    KTEST_ASSERT_EQ(gfx_width(), w);
    KTEST_ASSERT_EQ(gfx_height(), h);
}
