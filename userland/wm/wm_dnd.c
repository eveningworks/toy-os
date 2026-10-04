// Drag-and-drop BETWEEN windows: the compositor's half.
//
// A client's toolkit runs its own drag session inside its window
// (ui/uui_route.h's third rule) and cannot see past its edges. So when
// one starts it says so (WIN_REQ_DRAG_START), having put the files in
// the clipboard page's DRAG SLOT (lib/uclip.h), and this file offers
// the drag to whatever the held pointer is over that is NOT the source:
// WIN_EV_DRAG_OVER while it hovers, DRAG_LEAVE when it moves on, DROP
// on the release. The desktop is a target too (files land in
// /home/desktop) and a source (a file icon dragged onto a window).
//
// This is Wayland's wl_data_device shape -- the compositor brokers,
// the data rides shared memory, the source and target never meet --
// minus the type negotiation, since there is one kind. X11's XDND does
// it with client messages between the two windows and a selection,
// which is the design every compositor since has moved away from.
//
// THE SOURCE KEEPS ITS OWN SESSION: it still receives MOUSE_MOVE and
// MOUSE_UP with the button held (content_pressed), so a drop back in
// its own window is its own toolkit's, and a drop elsewhere reaches it
// only as a release nothing accepted -- the file having moved anyway.
#include "wm_internal.h"
#include "wm_dnd.h"
#include "desktop.h"
#include "ui/uui.h"
#include "ui/utheme.h"
#include "lib/uclip.h"
#include "kapi.h"
#include "rt/sys.h"

static int g_active;
static int g_count;
static int g_src_pid;      // the dragging client, 0 for the desktop
static int g_over;         // window index under the pointer, or -1
static int g_took_drop;    // set on the tick a drop went to a window
static char g_label[32];

int wm_dnd_active(void) { return g_active; }
int wm_dnd_took_drop(void) { return g_took_drop; }

static void begin(int pid, int count) {
    g_active = 1;
    g_count = count;
    g_src_pid = pid;
    g_over = -1;
    g_took_drop = 0;
    if (count == 1) k_snprintf(g_label, sizeof g_label, "1 item");
    else k_snprintf(g_label, sizeof g_label, "%d items", count);
}

void wm_dnd_start(int pid, int count) { begin(pid, count > 0 ? count : 1); }
void wm_dnd_start_desktop(int count) { begin(0, count > 0 ? count : 1); }

static void leave_current(void) {
    if (g_over >= 0 && g_over < window_count)
        wm_client_send_mouse(&windows[g_over], WIN_EV_DRAG_LEAVE, 0, 0, 0);
    g_over = -1;
}

void wm_dnd_end(int pid) {
    if (!g_active || (g_src_pid && pid != g_src_pid)) return;
    leave_current();
    g_active = 0;
    redraw_pending = 1;
}

// The topmost window under the pointer whose CONTENT the pointer is
// over, excluding the source's own windows -- its toolkit has those.
// THREE ANSWERS: an index; NOWHERE, the desktop background; and
// SOURCE, covered by the dragging client's own window (or a title bar,
// which takes nothing). The release cares about the difference: a
// drop over the source is its toolkit's, over nowhere the desktop's --
// the first version folded both into -1 and moved a file dropped
// inside its own pane onto the desktop.
#define NOWHERE (-1)
#define SOURCE  (-2)
static int target_under(int mx, int my) {
    for (int i = window_count - 1; i >= 0; i--) {
        struct window *w = &windows[i];
        if (w->state == WIN_MINIMIZED) continue;
        if (!uui_hit(w->x, w->y, w->w, w->h, mx, my)) continue;
        if (g_src_pid && w->client_pid == g_src_pid) return SOURCE;
        if (!w->popup && my < w->y + WM_TITLEBAR_H) return SOURCE;
        return wm_client_is_client_window(w) ? i : SOURCE;
    }
    return NOWHERE;
}

// Over something that will take NOTHING from a release here: another
// window's title bar, or a window that is not a client's (the WM's own
// popups). The source's own windows are excluded -- its toolkit decides
// there -- and so is the desktop background, which takes files.
int wm_dnd_refused_at(int mx, int my) {
    if (!g_active) return 0;
    for (int i = window_count - 1; i >= 0; i--) {
        struct window *w = &windows[i];
        if (w->state == WIN_MINIMIZED) continue;
        if (!uui_hit(w->x, w->y, w->w, w->h, mx, my)) continue;
        if (g_src_pid && w->client_pid == g_src_pid) return 0;
        return target_under(mx, my) == SOURCE;
    }
    return 0;
}

void wm_dnd_motion(int mx, int my, uint8_t buttons) {
    g_took_drop = 0;
    if (!g_active) return;
    int t = target_under(mx, my);
    if (buttons & 0x1) {
        int over = t >= 0 ? t : -1;
        if (over != g_over) { leave_current(); g_over = over; }
        if (t >= 0) wm_client_send_mouse(&windows[t], WIN_EV_DRAG_OVER, mx, my, 0);
        redraw_pending = 1;   // the ghost moved
        return;
    }
    // The release. Over a window: its drop. Over the desktop
    // BACKGROUND: the desktop's, unless the desktop is the source (its
    // own icon drag). Over the source: nothing -- its toolkit has it.
    if (t >= 0) {
        wm_client_send_mouse(&windows[t], WIN_EV_DRAG_OVER, mx, my, 0);
        wm_client_send_mouse(&windows[t], WIN_EV_DROP, mx, my, 0);
        g_took_drop = 1;
    } else if (t == NOWHERE && g_src_pid != 0 && desktop_drop_here(mx, my)) {
        g_took_drop = 1;
    }
    g_over = -1;
    g_active = 0;
    // THE SLOT IS NOT CLEARED HERE. The target reads it when it gets
    // round to the DROP event, which is after this tick; clearing it on
    // the release handed the File Manager an empty payload (measured:
    // the pane accepted mid-drag and nothing moved). The next drag's
    // begin overwrites it, and nothing reads it between drags.
    redraw_pending = 1;
}

// The ghost, drawn by the compositor once the pointer is outside the
// source: a client's own ghost stops at its window edge. A label
// rather than artwork -- what is carried is a count of files, and the
// artwork would be one program's guess at another's icon.
void wm_dnd_draw(int mx, int my) {
    if (!g_active) return;
    int inside_source = 0;
    for (int i = 0; i < window_count && g_src_pid; i++) {
        const struct window *w = &windows[i];
        if (w->client_pid == g_src_pid && w->state != WIN_MINIMIZED &&
            uui_hit(w->x, w->y, w->w, w->h, mx, my)) inside_source = 1;
    }
    if (inside_source) return;
    int tw = ugfx_text_width(g_label);
    int x = mx + 14, y = my + 14, w = tw + 12, h = ugfx_char_h() + 6;
    ugfx_fill_rect(wm_surface(), x, y, w, h, UTHEME_PANEL_BG);
    ugfx_draw_rect(wm_surface(), x, y, w, h, UTHEME_BORDER);
    ugfx_draw_string_clipped(wm_surface(), x + 6, y + 3, tw, g_label, UTHEME_TEXT, UTHEME_PANEL_BG);
    wm_damage_rect(x - 1, y - 1, w + 2, h + 2);
}

void wm_dnd_windows_moving(int idx, int to_front) {
    g_over = wm_index_after_move(g_over, idx, to_front);
}
