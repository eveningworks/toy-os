// See ui/uui_anim.h.
#include "ui/uui_anim.h"
#include "ui/ugfx.h"
#include "lib/usetting.h"
#include "rt/sys.h"

static int g_frame_wanted;

void uui_anim_request(void) { g_frame_wanted = 1; }

int uui_anim_take(void) {
    int w = g_frame_wanted;
    g_frame_wanted = 0;
    return w;
}

int uui_anim_pending(void) { return g_frame_wanted; }

unsigned long long uui_anim_now_ns(void) { return sys_monotonic_ns(); }

int uui_smooth_scroll_enabled(void) {
    char v[8];
    // ON when the registry has nothing to say (an older kernel, or a
    // failed call): the default the setting itself declares.
    if (!usetting_get("desktop.smooth_scroll", v, sizeof v)) return 1;
    return !(v[0] == 'o' && v[1] == 'f' && v[2] == 'f');
}

int uui_wheel_step_px(void) {
    int h = ugfx_char_h();
    return 3 * (h > 0 ? h : 14);
}
