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

#include "edid.h"

// The transcoder registers for the canned panel in edid_test.c
// (1920x1080, h 48/32/160, v 3/5/31): each field is (value - 1), the
// end in the high half. A decoded timing must round-trip to the EDID's.
KTEST("intel-display", "the transcoder timing registers decode to an EDID timing") {
    struct intel_trans_regs r = {
        .htotal = ((2080 - 1) << 16) | (1920 - 1),
        .hblank = ((2080 - 1) << 16) | (1920 - 1),
        .hsync  = ((2000 - 1) << 16) | (1968 - 1),
        .vtotal = ((1111 - 1) << 16) | (1080 - 1),
        .vblank = ((1111 - 1) << 16) | (1080 - 1),
        .vsync  = ((1088 - 1) << 16) | (1083 - 1),
    };
    struct edid_timing t;
    intel_display_timing_from_regs(&r, &t);
    KTEST_ASSERT_EQ(t.hactive, 1920u);
    KTEST_ASSERT_EQ(t.hblank, 160u);
    KTEST_ASSERT_EQ(t.hsync_off, 48u);
    KTEST_ASSERT_EQ(t.hsync_w, 32u);
    KTEST_ASSERT_EQ(t.vactive, 1080u);
    KTEST_ASSERT_EQ(t.vblank, 31u);
    KTEST_ASSERT_EQ(t.vsync_off, 3u);
    KTEST_ASSERT_EQ(t.vsync_w, 5u);
    struct edid_timing e = t;
    KTEST_ASSERT_EQ(intel_display_timing_same(&t, &e), 1);
    e.hsync_w = 44;
    KTEST_ASSERT_EQ(intel_display_timing_same(&t, &e), 0);
}

KTEST("intel-display", "the dot clock is the port clock scaled by link M/N") {
    KTEST_ASSERT_EQ(intel_display_port_clock_khz(2u << 29), 162000u);   // LCPLL 810
    KTEST_ASSERT_EQ(intel_display_port_clock_khz(1u << 29), 270000u);   // LCPLL 1350
    KTEST_ASSERT_EQ(intel_display_port_clock_khz(0), 540000u);          // LCPLL 2700
    KTEST_ASSERT_EQ(intel_display_port_clock_khz(4u << 29), 0u);        // WRPLL: not a DP tap
    // 138.5 MHz over a 2.7 Gbps link: m/n = 138500/270000 -> 0x8000 * 138500 / 270000
    KTEST_ASSERT_EQ(intel_display_dotclock_khz(270000, 138500, 270000), 138500u);
    KTEST_ASSERT_EQ(intel_display_dotclock_khz(270000, 0x40000000u | 138500, 270000), 138500u); // TU bits ignored
    KTEST_ASSERT_EQ(intel_display_dotclock_khz(270000, 1, 0), 0u);
}

#include "setting.h"

KTEST("intel-display", "the cycle tunable is unavailable, and refused, without the hardware") {
    if (intel_display_active()) KTEST_SKIP("this machine has the Intel display");
    const struct setting *s = setting_find("intel_cycle");
    KTEST_ASSERT(s != 0);
    KTEST_ASSERT(s->unavailable && s->unavailable() != 0);
    KTEST_ASSERT_EQ(intel_display_pipe_cycle(), 0);
    KTEST_ASSERT_EQ(intel_display_link_retrain(), 0);
    KTEST_ASSERT_EQ(intel_display_native(), 0);
}

KTEST("intel-display", "the fitter window keeps the aspect, fills, or centres") {
    uint32_t x, y, w, h;
    intel_display_fit_window(DISPLAY_SCALING_ASPECT, 1280, 1024, 1920, 1080, &x, &y, &w, &h);
    KTEST_ASSERT_EQ(w, 1350u); KTEST_ASSERT_EQ(h, 1080u);
    KTEST_ASSERT_EQ(x, 285u);  KTEST_ASSERT_EQ(y, 0u);      // half the border, odd
    intel_display_fit_window(DISPLAY_SCALING_ASPECT, 800, 600, 1920, 1080, &x, &y, &w, &h);
    KTEST_ASSERT_EQ(w, 1440u); KTEST_ASSERT_EQ(h, 1080u);
    KTEST_ASSERT_EQ(x, 240u);  KTEST_ASSERT_EQ(y, 0u);
    intel_display_fit_window(DISPLAY_SCALING_ASPECT, 1280, 720, 1920, 1080, &x, &y, &w, &h);
    KTEST_ASSERT_EQ(w, 1920u); KTEST_ASSERT_EQ(h, 1080u);   // same aspect: the whole panel
    intel_display_fit_window(DISPLAY_SCALING_FULL, 1024, 768, 1920, 1080, &x, &y, &w, &h);
    KTEST_ASSERT_EQ(w, 1920u); KTEST_ASSERT_EQ(h, 1080u);
    KTEST_ASSERT_EQ(x, 0u);    KTEST_ASSERT_EQ(y, 0u);
    intel_display_fit_window(DISPLAY_SCALING_CENTER, 1280, 1024, 1920, 1080, &x, &y, &w, &h);
    KTEST_ASSERT_EQ(w, 1280u); KTEST_ASSERT_EQ(h, 1024u);
    KTEST_ASSERT_EQ(x, 320u);  KTEST_ASSERT_EQ(y, 28u);
    intel_display_fit_window(DISPLAY_SCALING_ASPECT, 1920, 1080, 1920, 1080, &x, &y, &w, &h);
    KTEST_ASSERT_EQ(w, 1920u); KTEST_ASSERT_EQ(h, 1080u);   // native: pass-through
    intel_display_fit_window(DISPLAY_SCALING_CENTER, 1366, 768, 1920, 1080, &x, &y, &w, &h);
    KTEST_ASSERT_EQ(w, 1366u); KTEST_ASSERT_EQ(h, 768u);    // the skewed case: 2*276+1366 was 1918
    KTEST_ASSERT_EQ(x, 277u);  KTEST_ASSERT_EQ(y, 156u);
    intel_display_fit_window(DISPLAY_SCALING_ASPECT, 1366, 768, 1920, 1080, &x, &y, &w, &h);
    KTEST_ASSERT_EQ(w, 1920u); KTEST_ASSERT_EQ(h, 1080u);   // 1079 rounds UP, never to 1078 at y 0
    KTEST_ASSERT_EQ(y, 0u);
}

// The hardware rule behind every case above (i915's
// intel_pch_pfit_check_dst_window): panel = 2 * position + size on
// each axis, and a position of 1 is forbidden. Every ladder mode below
// a 1080p panel, under every policy.
KTEST("intel-display", "every fitter window equals the pipe active area") {
    int lw, lh;
    for (int i = 0; display_ladder_mode(i, &lw, &lh); i++) {
        if (lw > 1920 || lh > 1080) continue;
        for (int sc = DISPLAY_SCALING_ASPECT; sc <= DISPLAY_SCALING_CENTER; sc++) {
            uint32_t x, y, w, h;
            intel_display_fit_window(sc, (uint32_t)lw, (uint32_t)lh, 1920, 1080, &x, &y, &w, &h);
            KTEST_ASSERT_EQ(2 * x + w, 1920u);
            KTEST_ASSERT_EQ(2 * y + h, 1080u);
            KTEST_ASSERT(x != 1 && y != 1);
            KTEST_ASSERT(!(w & 1) && !(h & 1));
        }
    }
}

KTEST("display", "the scaling policy is stored on every machine, and refused out of range") {
    int was = display_scaling();
    KTEST_ASSERT_EQ(display_set_scaling(DISPLAY_SCALING_FULL), 1);
    KTEST_ASSERT_EQ(display_scaling(), DISPLAY_SCALING_FULL);
    KTEST_ASSERT_EQ(display_set_scaling(7), 0);
    KTEST_ASSERT_EQ(display_scaling(), DISPLAY_SCALING_FULL);
    KTEST_ASSERT_EQ(display_set_scaling(was), 1);
}
