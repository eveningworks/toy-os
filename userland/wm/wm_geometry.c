// Per-application window geometry across launches -- see wm_geometry.h
// for the key and the save point, which are the two decisions here.
#include "wm_geometry.h"
#include "wm_internal.h"
#include "wm_conf.h"
#include "gui_apps.h"
#include "string.h"
#include "knum.h"
#include "kfmt.h"

#define WINDOWS_CONF_PATH "/etc/windows.conf"

// READ FRESH ON EVERY RESTORE, deliberately not cached.
//
// The first version cached the parsed file and invalidated it only when
// this module's own save rewrote it -- which makes the WM blind to any
// other writer, including a person editing /etc/windows.conf and the
// test that writes a known geometry before opening a window. It failed
// exactly there, and the general shape is worth naming: a cache
// invalidated only by its owner's writes is correct only while its
// owner is the only writer, and nothing said that it was.
//
// The cost is one whole-file read per window OPENED, which is rare and
// already expensive. wm_conf.h's warning is about re-reading per KEY in
// a loop (the nine-entry desktop reload that cost 54 reads); one read
// per window is not that.
static struct etc_config_buf g_cfg;

static void geom_load(void) {
    wm_conf_load(WINDOWS_CONF_PATH, &g_cfg);  // missing file: every get says 0
}

// The app's stable identifier, or NULL for a window that has none.
//
// A client and a kernel-space app keep it in different places -- a
// client window has no `gui_app` by construction (wm.h) -- and neither
// is the taskbar's grouping int. "" is treated as absent: a client that
// named nothing must not share a slot with every other client that
// named nothing.
static const char *geom_key(const struct window *win) {
    if (!win) return 0;
    if (win->app && win->app->app_id && win->app->app_id[0]) return win->app->app_id;
    if (win->app_id[0]) return win->app_id;
    return 0;
}

// Does this app want its geometry remembered? Looked up in the registry
// rather than read off the window, because a client window has no
// `gui_app` to carry the flag and both kinds are launched from the same
// `.desktop` entry. An app with no registry entry at all (a window from
// something the launcher never heard of) is remembered -- the default
// is on, and an unknown app is not a reason to refuse.
static int remembers(const char *key) {
    for (int i = 0; i < gui_app_registry_count; i++) {
        const struct gui_app *a = &gui_app_registry[i];
        if (a->app_id && k_strcmp(a->app_id, key) == 0) return a->remember_geometry;
    }
    return 1;
}

// "x,y,w,h" -- four signed decimals. Returns 0 unless all four parse,
// so a hand-edited or truncated line leaves the default placement
// rather than a window at a partly-parsed position.
static int parse_geom(const char *s, int *x, int *y, int *w, int *h) {
    int *out[4] = { x, y, w, h };
    for (int i = 0; i < 4; i++) {
        if (!*s) return 0;
        int neg = 0;
        if (*s == '-') { neg = 1; s++; }
        if (*s < '0' || *s > '9') return 0;
        int v = 0;
        while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
        *out[i] = neg ? -v : v;
        if (i < 3) { if (*s != ',') return 0; s++; }
    }
    return *s == '\0';
}

void wm_geometry_save(const struct window *win) {
    if (win->popup) return; // placed by the compositor every time, never remembered
    const char *key = geom_key(win);
    if (!key || !remembers(key)) return;

    // A MAXIMIZED WINDOW SAVES ITS RESTORE RECT, not the screen-sized
    // one it currently fills. Saving the maximized geometry would
    // reopen the app at exactly screen size but NOT maximized -- a
    // window that looks maximized, has no restore size to go back to,
    // and covers the taskbar. The restore rect is already maintained
    // for the un-maximize path (wm_input.c), so it costs nothing.
    int x = win->x, y = win->y, w = win->w, h = win->h;
    if (win->state == WIN_MAXIMIZED) {
        x = win->saved_x; y = win->saved_y;
        w = win->saved_w; h = win->saved_h;
    }
    // A MINIMIZED window keeps its real geometry -- minimizing does not
    // move it -- so there is nothing to special-case there.
    if (w <= 0 || h <= 0) return;

    char value[40];
    k_snprintf(value, sizeof value, "%d,%d,%d,%d", x, y, w, h);
    wm_conf_set(WINDOWS_CONF_PATH, key, value);
}

void wm_geometry_restore(int idx) {
    if (idx < 0 || idx >= window_count) return;
    struct window *win = &windows[idx];
    const char *key = geom_key(win);
    if (!key || !remembers(key)) return;

    geom_load();
    char value[40];
    if (!etc_config_buf_get(&g_cfg, key, value, sizeof value)) return;

    int x, y, w, h;
    if (!parse_geom(value, &x, &y, &w, &h)) return;

    // A SAVED SIZE FROM A BIGGER SCREEN MUST NOT BE RESTORED WHOLE.
    // The display can change between runs -- a different monitor, a
    // `video=` boot flag, a runtime mode switch -- and a window wider
    // than the screen has a title bar whose buttons are off the right
    // edge, i.e. one that cannot be closed with the mouse.
    int max_w = screen_w;
    int max_h = screen_h - taskbar_h;
    if (w > max_w) w = max_w;
    if (h > max_h) h = max_h;
    if (w < 1 || h < 1) return;

    // AND THE POSITION IS CLAMPED FULLY ON-SCREEN, which is a STRICTER
    // rule than the WM's general one and deliberately so. A window may
    // be DRAGGED off the left, right and bottom edges on purpose
    // (docs/gui-guidelines.md), so wm_ensure_reachable() only guarantees
    // a sliver stays grabbable -- restoring 4000x3000 at (5000,5000)
    // through it produced a 1280-wide window at x=1216, i.e. 64 pixels
    // of it on screen. That is the right answer for a drag the user
    // meant and the wrong one for a position they never chose: the
    // screen may simply be smaller than it was last time.
    // Against the SCREEN, not the work area: a window may legitimately
    // sit over the taskbar (docs/gui-guidelines.md allows dragging one
    // there), so clamping to the work area would quietly MOVE a window
    // the user had deliberately placed -- 200,150 came back as 200,120
    // the first time this was tried. Off the screen entirely is the
    // only thing being prevented here.
    if (x > screen_w - w) x = screen_w - w;
    if (y > screen_h - h) y = screen_h - h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;   // the title bar must be reachable

    win->x = x; win->y = y;

    // A CLIENT OWNS ITS OWN BUFFER, so its size is a REQUEST, not an
    // assignment: the WM cannot widen a window whose pixels the client
    // allocated, and **a client may DECLINE** -- a fixed-size app
    // always does. So the frame keeps the size it has until a present
    // answers, which carries the buffer's real dimensions and is
    // adopted in on_window_present() for an accepted resize and a
    // refused one alike.
    //
    // Assigning it here as well was the bug: a declined proposal left
    // the frame at the saved size with the client still drawing its
    // own, i.e. a small window painted into the top-left corner of a
    // big one, and nothing afterwards could correct it -- the present
    // that would have carried the truth matched `client_w/h`, which
    // this had not touched.
    if (wm_client_is_client_window(win)) {
        int content_w = w - 2;
        int content_h = h - WM_TITLEBAR_H - 2;
        if (content_w > 0 && content_h > 0 &&
            (content_w != win->client_w || content_h != win->client_h)) {
            wm_client_send_resize(win, content_w, content_h);
        }
    } else {
        win->w = w; win->h = h;
    }

    // Belt and braces: the clamp above is arithmetic on this window,
    // wm_ensure_reachable() is the WM's own invariant. If they ever
    // disagree the WM's wins.
    wm_ensure_reachable(idx);
}
