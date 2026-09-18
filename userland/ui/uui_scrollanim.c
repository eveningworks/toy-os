// See ui/uui_scrollanim.h.
#include "ui/uui_scrollanim.h"
#include "ui/uui_anim.h"

void uui_scrollanim_init(struct uui_scrollanim *a) {
    a->inited = 0;
    a->last_px = 0;
    a->armed = 0;
    a->enabled = 0;
    a->disp = 0;
    a->tw.active = 0;
    a->tw.to = 0;
    a->tw.from = 0;
    a->tw.t0_ns = a->tw.dur_ns = 0;
}

void uui_scrollanim_arm(struct uui_scrollanim *a) {
    // The setting is read HERE, once per user scroll, so a change in
    // System Settings applies to the next notch and costs nothing per
    // frame.
    a->enabled = uui_smooth_scroll_enabled();
    a->armed = 1;
}

void uui_scrollanim_cancel(struct uui_scrollanim *a) {
    a->armed = 0;
    a->tw.active = 0;
    a->tw.to = 0;
    a->disp = 0;
}

int uui_scrollanim_sync(struct uui_scrollanim *a, int pos_px) {
    unsigned long long now = uui_anim_now_ns();
    if (!a->inited) {
        a->inited = 1;
        a->last_px = pos_px;
        a->armed = 0;
    }
    int delta = pos_px - a->last_px;
    a->last_px = pos_px;
    if (delta != 0 && a->armed && a->enabled) {
        // Start where the content WAS: the residual glide, if one is
        // still running, plus the distance this change moved it. A
        // second notch mid-glide therefore extends the motion rather
        // than restarting it from a jump.
        int here = a->tw.active ? utween_value(&a->tw, now) : 0;
        utween_start(&a->tw, here + delta, 0, UUI_SCROLL_MS, now);
    } else if (delta != 0) {
        // An unarmed change (a reload, a re-clamp, a drag): draw at the
        // new place at once, and drop any glide that was in flight --
        // it belonged to a position that no longer exists.
        a->tw.active = 0;
        a->tw.to = 0;
    }
    a->armed = 0;
    a->disp = a->tw.active ? utween_value(&a->tw, now) : 0;
    if (a->tw.active) uui_anim_request();
    return a->disp;
}
