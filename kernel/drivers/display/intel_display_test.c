// KTESTs for the Intel display driver. The hardware is never in QEMU,
// so the live-driver tests SKIP there and the encodings are tested as
// pure functions -- the sign-magnitude cursor position and the PWM
// duty are the two things a wrong bit would get silently wrong on the
// one machine that has the hardware.
#include "intel_display.h"
#include "display.h"
#include "string.h"
#include "gfx.h"
#include "win_surface.h" // win_surface_holder() -- skip under a compositor
#include "ktest.h"

KTEST("intel-display", "CURPOS is sign-magnitude, 13 bits per axis") {
    KTEST_ASSERT_EQ(intel_display_curpos_field(0), 0u);
    KTEST_ASSERT_EQ(intel_display_curpos_field(100), 100u);
    KTEST_ASSERT_EQ(intel_display_curpos_field(-1), 0x8001u);
    KTEST_ASSERT_EQ(intel_display_curpos_field(-7), 0x8007u);
    // The hotspot pulls a pointer at (0,0) to a negative plane origin,
    // so the sign bit is the everyday case, not an edge.
    KTEST_ASSERT_EQ(intel_display_curpos_field(8191), 0x1FFFu);
}

KTEST("intel-display", "a duty is the period scaled, and never 0 for a live percent") {
    KTEST_ASSERT_EQ(intel_display_duty(937, 100), 937u);
    KTEST_ASSERT_EQ(intel_display_duty(937, 50), 469u);   // 468.5 rounds up
    KTEST_ASSERT_EQ(intel_display_duty(937, 0), 0u);
    KTEST_ASSERT_EQ(intel_display_duty(10, 1), 1u);       // would round to 0
    KTEST_ASSERT_EQ(intel_display_duty(937, 200), 937u);  // clamped
    // And the percent read back is what was asked for.
    for (int pct = 5; pct <= 100; pct += 5)
        KTEST_ASSERT_EQ(intel_display_percent(937, intel_display_duty(937, pct)), pct);
    KTEST_ASSERT_EQ(intel_display_percent(0, 0), -1);
}

KTEST("intel-display", "without the hardware the driver declines and the backlight is absent") {
    if (intel_display_active()) KTEST_SKIP("this machine has the Intel display");
    const struct display_driver *d = display_active();
    KTEST_ASSERT(d != 0);
    KTEST_ASSERT(k_strcmp(d->name, "intel-display") != 0);
    KTEST_ASSERT(!display_has(DISPLAY_CAP_BACKLIGHT));
    KTEST_ASSERT_EQ(display_backlight_get(), -1);
    KTEST_ASSERT_EQ(display_backlight_set(50), 0);
}

KTEST("intel-display", "the scanout count is 1 without the flip capability, and index 0 is the surface") {
    int n = display_scanout_count();
    KTEST_ASSERT_EQ(n, display_has(DISPLAY_CAP_FLIP) ? 3 : 1);
    struct display_surface a, b;
    display_get_surface(&a);
    display_scanout_at(0, &b);
    KTEST_ASSERT_EQ(a.addr, b.addr);
    if (n == 1) {
        KTEST_ASSERT_EQ(display_flip(1), 0);   // refused, not a crash
        KTEST_ASSERT_EQ(display_flip(0), 1);   // the trivial flip
    }
}

KTEST("intel-display", "on the hardware a flip round trip lands in DSPSURFLIVE") {
    if (!intel_display_active()) KTEST_SKIP("no Intel display on this machine");
    if (intel_display_scanout_count() < 3) KTEST_SKIP("no extra scanouts");
    if (win_surface_holder()) KTEST_SKIP("a compositor holds the screen");
    struct display_surface s1;
    display_scanout_at(1, &s1);
    KTEST_ASSERT(s1.addr != 0);
    KTEST_ASSERT_EQ(s1.pitch, (uint32_t)gfx_framebuffer_pitch());
    // Copy the console's frame across so the screen does not blink,
    // then flip there and wait (bounded) for LIVE to follow.
    k_memcpy((void *)(uintptr_t)s1.addr, (void *)(uintptr_t)gfx_framebuffer_phys(),
             (unsigned)(s1.pitch * s1.height));
    KTEST_ASSERT_EQ(display_flip(1), 1);
    int live = 0;
    for (int i = 0; i < 4000000 && (live = display_scanout_live()) != 1; i++) { }
    KTEST_ASSERT_EQ(live, 1);
    KTEST_ASSERT_EQ(display_flip(0), 1);
    for (int i = 0; i < 4000000 && (live = display_scanout_live()) != 0; i++) { }
    KTEST_ASSERT_EQ(live, 0);
}

KTEST("intel-display", "on the hardware the backlight reads back what was set") {
    if (!intel_display_active()) KTEST_SKIP("no Intel display on this machine");
    KTEST_ASSERT(display_has(DISPLAY_CAP_BACKLIGHT));
    int was = display_backlight_get();
    KTEST_ASSERT(was >= 0);
    KTEST_ASSERT_EQ(display_backlight_set(60), 1);
    KTEST_ASSERT_EQ(display_backlight_get(), 60);
    KTEST_ASSERT_EQ(display_backlight_set(was), 1);
    KTEST_ASSERT_EQ(display_backlight_get(), was);
}
