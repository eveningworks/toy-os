// The lease policy: which client, if any, is handed the display's own
// scanouts to draw into directly (docs/scanout-design.md).
//
// Decided once per rendered frame, as a Wayland compositor assigns
// surfaces to planes each frame and falls back to composition the
// moment the rule stops holding. The rule: the topmost toplevel is
// FULLSCREEN and has ADOPTED the screen's size, declared itself
// write-only (WIN_HINT_SCANOUT), nothing is drawn above it (no popup,
// no overlay), and the pointer is on the hardware cursor plane -- with
// this compositor not presenting, its software cursor would vanish.
//
// THE INVARIANT: while a lease stands this compositor presents NOTHING
// (wm_render_frame() returns before drawing), because its frame is not
// what is on screen. Ending a lease forgets what the buffers hold
// (ugfx_screen_forget) and repaints everything.
#include "wm.h"
#include "wm_internal.h"
#include "wm_overlay.h"
#include "wm_log.h"
#include "ui/ugfx.h"
#include "win_proto.h"
#include "rt/sys.h"

static int g_lessee_pid;   // 0 = no lease
static int g_unmap_pid;    // an ex-lessee whose pages are still mapped

int wm_scanout_active(void) { return g_lessee_pid != 0; }
int wm_scanout_lessee(void) { return g_lessee_pid; }

static int any_popup(void) {
    for (int i = 0; i < window_count; i++)
        if (windows[i].popup && windows[i].state != WIN_MINIMIZED) return 1;
    return 0;
}

// The window that should hold the lease right now, or -1.
static int wanted(void) {
    int f = wm_focus_index();
    if (f < 0) return -1;
    const struct window *w = &windows[f];
    if (!w->fullscreen || !w->scanout_ok || !wm_client_is_client_window(w)) return -1;
    if (w->x != 0 || w->y != 0 || w->w < screen_w || w->h < screen_h) return -1; // not adopted yet
    if (w->client_w < screen_w || w->client_h < screen_h) return -1;
    if (any_popup() || wm_overlay_any_open()) return -1;
    if (!wm_hwcursor_active()) return -1;
    return f;
}

static void lease_end(void) {
    if (!g_lessee_pid) return;
    struct win_request_msg q;
    for (unsigned i = 0; i < sizeof q; i++) ((uint8_t *)&q)[i] = 0;
    q.type = WIN_REQ_FB_LEASE;
    q.a = 0;
    int back = sys_win_request(&q) == 0 ? (int)q.window : -1;
    for (int i = 0; i < window_count; i++)
        if (windows[i].client_pid == g_lessee_pid && !windows[i].popup)
            wm_client_send_scanout(&windows[i], 0, 0, 0, 0);
    wm_logf("wm: lease from pid %d ended\n", g_lessee_pid);
    g_unmap_pid = g_lessee_pid;
    g_lessee_pid = 0;
    ugfx_screen_forget(&g_wm_screen, back);
    wm_damage_rect(0, 0, screen_w, screen_h);
    redraw_pending = 1;
}

static void lease_start(int idx) {
    struct window *w = &windows[idx];
    struct win_request_msg q;
    for (unsigned i = 0; i < sizeof q; i++) ((uint8_t *)&q)[i] = 0;
    q.type = WIN_REQ_FB_LEASE;
    q.a = w->client_pid;
    if (sys_win_request(&q) != 0) {
        wm_logf("wm: lease to pid %d refused\n", w->client_pid);
        return;
    }
    g_lessee_pid = w->client_pid;
    wm_client_send_scanout(w, 1, (uint32_t)q.c, (int)q.mods, (int)q.window);
    wm_logf("wm: leased the display to pid %d (%d scanouts, pitch %d)\n",
            w->client_pid, (int)q.mods, q.c);
}

// The ex-lessee presented from its own buffer: it has switched, and
// the display's pages can come off it now.
void wm_scanout_client_presented(int pid) {
    if (!g_unmap_pid || pid != g_unmap_pid) return;
    struct win_request_msg q;
    for (unsigned i = 0; i < sizeof q; i++) ((uint8_t *)&q)[i] = 0;
    q.type = WIN_REQ_FB_LEASE;
    q.a = pid;
    q.b = 1;
    sys_win_request(&q);
    g_unmap_pid = 0;
}

void wm_scanout_update(void) {
    int want = wanted();
    int want_pid = want >= 0 ? windows[want].client_pid : 0;
    if (want_pid == g_lessee_pid) return;
    if (g_lessee_pid) lease_end();
    if (want >= 0) lease_start(want);
}
