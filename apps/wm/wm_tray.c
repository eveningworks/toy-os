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
#include "wm_tray.h"
#include "kapi.h"

#define TRAY_MAX_ITEMS 6

struct tray_item {
    int active;
    char text[TRAY_TEXT_MAX];
};

static struct tray_item tray_items[TRAY_MAX_ITEMS];
static int clock_tray_id = -1;

// Deliberately does NOT call wm_damage_rect() to scope this to just the
// taskbar strip -- two real bugs came from an earlier version that did
// (see CHANGELOG.md): tray_init() runs before wm_run()'s main loop
// starts, so a pre-loop wm_damage_rect() call poisoned the very first
// frame's "no damage reported yet -- unknown, be safe, draw everything"
// full-screen fallback, narrowing it to just the taskbar and leaving
// the desktop/icons never drawn at all. And the once-a-second clock
// tick relying on that same full-screen fallback (as it always had
// before this file existed) turned out to be load-bearing for the
// mouse cursor's saved-pixels-underneath snapshot (draw_cursor_at()'s
// cursor_under, see wm_render.c) staying in sync with the real screen
// -- scoping the tick to a narrow rect removed that implicit
// once-a-second full resync. Matches every other still-unscoped piece
// of WM chrome (menus, dialogs -- see docs/roadmap.md's Milestone 9
// entry): safe and imprecise, never worse than before.
static void tray_damage(void) {
    redraw_pending = 1;
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
    clock_tray_id = tray_register("00:00:00");
}

void tray_update_clock(void) {
    if (clock_tray_id < 0) return;
    struct rtc_time t;
    rtc_read_local(&t); // local time for the selected `timezone`, not raw UTC

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
        int text_w = (int)k_strlen(tray_items[i].text) * gfx_char_w();
        cx -= text_w;
        int text_y = taskbar_y + (taskbar_h - gfx_char_h()) / 2;
        gfx_fill_rect(cx - 4, taskbar_y, text_w + 8, taskbar_h, bg);
        gfx_draw_string(cx, text_y, tray_items[i].text, fg, bg);
        cx -= 12;
    }
}
