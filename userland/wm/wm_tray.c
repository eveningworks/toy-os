// See wm_tray.h for the WM-internal half of this API and wm.h for the
// app-facing half (tray_register()/tray_set_text()/tray_unregister()).
//
// A fixed array of slots rather than a linked list -- same "WM-global,
// small, fixed" shape as windows[] in wm_internal.h, and there's no
// real need for more items than fit in a taskbar strip anyway. Slot 0
// is always the built-in clock (registered by tray_init()); apps that
// register later get whatever slot is free, not necessarily in order,
// so `active` is what actually matters, not slot index continuity.
#include "wm_internal.h"
#include "rt/sys.h"
#include "wm_tray.h"
#include "kapi.h"

#define TRAY_MAX_ITEMS 6

struct tray_item {
    int active;
    char text[TRAY_TEXT_MAX];
};

static struct tray_item tray_items[TRAY_MAX_ITEMS];
static int clock_tray_id = -1;

// This DOES damage the taskbar strip now. It deliberately didn't, for
// two stated reasons, and both have since stopped applying -- the
// lifted-constraint pattern this project keeps hitting.
//
// Reason one was real and still is: tray_init() runs before wm_run()'s
// loop, and damaging from there poisoned the very first frame's "no
// damage reported yet -- draw everything" fallback, narrowing it to the
// taskbar and leaving the desktop never drawn. That's handled by
// `tray_ready` below rather than by refusing to damage at all.
//
// Reason two has expired. It said the once-a-second full-screen repaint
// was load-bearing for the cursor's saved-pixels-underneath snapshot
// staying in sync -- an implicit resync. wm_render_frame() restores the
// cursor before repainting now (see wm_render.c), so nothing depends on
// that accidental resync any more.
//
// And leaving it unscoped had a cost that only became visible once
// there was a way to see it: on any frame where something ELSE reported
// damage, the clock's repaint landed outside that rect. `gui damage
// verify on` reported it immediately -- "93 px changed outside the
// damage rect, first at (1255,699)" -- which is the clock.
static int tray_ready;

static void tray_damage(void) {
    redraw_pending = 1;
    // Before the main loop exists there is nothing to scope, and
    // damaging here would narrow the first frame -- see above.
    if (tray_ready) wm_damage_rect(0, screen_h - taskbar_h, screen_w, taskbar_h);
}

static void tray_copy_text(char *dst, const char *src) {
    int i = 0;
    for (; src[i] && i < TRAY_TEXT_MAX - 1; i++) dst[i] = src[i];
    dst[i] = '\0';
}

int tray_register(const char *initial_text) {
    for (int i = 0; i < TRAY_MAX_ITEMS; i++) {
        if (tray_items[i].active) continue;
        tray_items[i].active = 1;
        tray_copy_text(tray_items[i].text, initial_text);
        tray_damage();
        return i;
    }
    return -1;
}

void tray_set_text(int tray_id, const char *text) {
    if (tray_id < 0 || tray_id >= TRAY_MAX_ITEMS) return;
    if (!tray_items[tray_id].active) return;
    tray_copy_text(tray_items[tray_id].text, text);
    tray_damage();
}

void tray_unregister(int tray_id) {
    if (tray_id < 0 || tray_id >= TRAY_MAX_ITEMS) return;
    if (!tray_items[tray_id].active) return;
    tray_items[tray_id].active = 0;
    tray_damage();
}

void tray_init(void) {
    tray_ready = 0;
    clock_tray_id = tray_register("00:00:00");
    tray_ready = 1; // from here on, scope the clock tick to the taskbar
}

void tray_update_clock(void) {
    if (clock_tray_id < 0) return;
    struct rtc_time t;
    sys_gettime(&t); // local time for the selected `timezone`, not raw UTC

    char buf[9];
    buf[0] = '0' + (t.hour / 10);
    buf[1] = '0' + (t.hour % 10);
    buf[2] = ':';
    buf[3] = '0' + (t.minute / 10);
    buf[4] = '0' + (t.minute % 10);
    buf[5] = ':';
    buf[6] = '0' + (t.second / 10);
    buf[7] = '0' + (t.second % 10);
    buf[8] = '\0';
    tray_set_text(clock_tray_id, buf);
}

void draw_tray(int taskbar_y, uint32_t bg, uint32_t fg) {
    int cx = screen_w - 16;
    for (int i = TRAY_MAX_ITEMS - 1; i >= 0; i--) {
        if (!tray_items[i].active) continue;
        int text_w = (int)k_strlen(tray_items[i].text) * ugfx_char_w();
        cx -= text_w;
        int text_y = taskbar_y + (taskbar_h - ugfx_char_h()) / 2;
        ugfx_fill_rect(wm_surface(), cx - 4, taskbar_y, text_w + 8, taskbar_h, bg);
        ugfx_draw_string(wm_surface(), cx, text_y, tray_items[i].text, fg, bg);
        cx -= 12;
    }
}
