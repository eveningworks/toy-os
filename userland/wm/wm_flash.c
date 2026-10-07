// See wm_flash.h.
#include "wm_internal.h"
#include "wm_flash.h"
#include "wm_anim.h"     // WM_ANIM_FRAME_MS
#include "rt/sys.h"
#include "ui/ugfx.h"

#define FLASH_NS 250000000ull
#define FLASH_PEAK 200          // the first frame's white, out of 255

static uint64_t g_until;

void wm_flash_start(void) { g_until = sys_monotonic_ns() + FLASH_NS; }

int wm_flash_open(void) { return g_until && sys_monotonic_ns() < g_until; }

void wm_flash_draw(int mx, int my) {
    (void)mx; (void)my;
    uint64_t now = sys_monotonic_ns();
    if (!g_until || now >= g_until) return;
    struct ugfx_surface *s = wm_surface();
    uint8_t a = (uint8_t)(FLASH_PEAK * (g_until - now) / FLASH_NS);
    uint32_t white = ugfx_rgb(255, 255, 255);
    for (int y = 0; y < s->h; y++) {
        uint32_t *row = s->pixels + (size_t)y * s->w;
        for (int x = 0; x < s->w; x++) row[x] = ugfx_blend(row[x], white, a);
    }
}

int wm_flash_wait_ms(void) { return wm_flash_open() ? WM_ANIM_FRAME_MS : -1; }
