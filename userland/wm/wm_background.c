// The background client -- see wm_background.h.
#include "wm_background.h"

#include "wm_internal.h"
#include "desktop.h"
#include "leave_page.h"
#include "wm_idle.h"
#include "wm_log.h"
#include "wm_watch.h"
#include "lib/ulivewall.h"
#include "lib/usetting.h"
#include "rt/sys.h"
#include "string.h"
#include <stdio.h>

// The surface, held in a struct window only so wm_client.c's buffer
// mapping serves it unchanged. NEVER in windows[] (the header says why).
static struct window g_win;
static int g_pid;                 // the client we spawned, or 0
static int g_have_frame;          // a frame at the screen's size is mapped
static char g_want[96];           // the program the settings ask for, "" for none
static char g_running[96];        // the program g_pid is
static char g_arg[64];            // its argument: the animated picture's name, or ""
static int g_pause_on = 1;        // `desktop.wallpaper_pause`
static int g_paused;
static unsigned g_frames;
static int g_failures;            // deaths before a first frame, in a row
static uint64_t g_retry_at;       // ticks; 0 = no restart pending

#define RETRY_TICKS 200           // 2 s between a crash and the restart
#define FAILURES_MAX 3            // then stop until the settings change

static void drop_surface(void) {
    if (g_win.open) wm_client_unmap(&g_win);
    memset(&g_win, 0, sizeof g_win);
    if (g_have_frame) {
        g_have_frame = 0;
        desktop_background_changed();   // the picture is back, glass resamples
    }
}

static void stop(void) {
    if (!g_pid) return;
    // KILLED, as a screensaver is: the background is the compositor's to
    // replace, and a client that could refuse would pin an old choice.
    sys_kill(g_pid, 9);
    wm_logf("wm: background %s stopped (pid %d)\n", g_running, g_pid);
    g_pid = 0;
    g_running[0] = '\0';
    drop_surface();
}

static void start(void) {
    int pid = sys_spawn(g_want, g_arg[0] ? g_arg : 0, -1);
    if (pid <= 0) {
        wm_logf("wm: background %s FAILED to start\n", g_want);
        g_failures = FAILURES_MAX;   // a missing program will not appear by retrying
        return;
    }
    g_pid = pid;
    k_strlcpy(g_running, g_want, sizeof g_running);
    wm_track_launched(pid);   // wm.c's reaper owns the zombie
    wm_logf("wm: background %s started as pid %d\n", g_running, pid);
}

// What the settings ask for: an effect's program, the player and the
// animated picture it plays, or nothing.
static void read_settings(void) {
    char type[16] = "picture", v[64];
    usetting_get("desktop.wallpaper_type", type, sizeof type);
    g_want[0] = '\0';
    g_arg[0] = '\0';
    k_strlcpy(v, LIVEWALL_DEFAULT, sizeof v);
    usetting_get("desktop.wallpaper_live", v, sizeof v);
    // A NAME, never a path: a value with a slash would reach outside the
    // directories whose contents are trusted with the role.
    if (!strcmp(type, "live") && v[0] && !strchr(v, '/') && v[0] != '.') {
        // An EFFECT when there is a program by that name, else an
        // animated picture or a video -- the choice list is both
        // directories.
        char path[128];
        struct sys_stat st;
        snprintf(path, sizeof path, "%s/%s", LIVEWALL_DIR, v);
        if (sys_stat(path, &st) == 0) {
            k_strlcpy(g_want, path, sizeof g_want);
        } else {
            snprintf(path, sizeof path, "%s/%s.gif", LIVEWALL_ANIMATED_DIR, v);
            if (sys_stat(path, &st) == 0) {
                k_strlcpy(g_want, LIVEWALL_GIF_PLAYER, sizeof g_want);
                k_strlcpy(g_arg, v, sizeof g_arg);
            }
            for (int i = 0; !g_want[0] && LIVEWALL_VIDEO_EXT[i]; i++) {
                snprintf(path, sizeof path, "%s/%s.%s", LIVEWALL_ANIMATED_DIR, v, LIVEWALL_VIDEO_EXT[i]);
                if (sys_stat(path, &st) == 0) {
                    k_strlcpy(g_want, LIVEWALL_VIDEO_PLAYER, sizeof g_want);
                    k_strlcpy(g_arg, v, sizeof g_arg);
                }
            }
        }
    }
    g_pause_on = !(usetting_get("desktop.wallpaper_pause", v, sizeof v) && !strcmp(v, "off"));
}

void wm_bg_poll(void) {
    static uint64_t seen;
    static int primed;
    uint64_t gen = wm_watch_config_gen() + wm_watch_gen(WM_TOPIC_DESKTOP);
    if (!primed || gen != seen) {
        primed = 1;
        seen = gen;
        char was[sizeof g_want], was_arg[sizeof g_arg];
        k_strlcpy(was, g_want, sizeof was);
        k_strlcpy(was_arg, g_arg, sizeof was_arg);
        read_settings();
        // ONLY A DIFFERENT PROGRAM REPLACES THE CLIENT. An option of the
        // same one is re-read by the client itself (ulivewall_main), and
        // the generation moves for every write anywhere -- restarting on
        // it would blank the background whenever anything saved a file.
        // The player is one program for every animated picture, so a
        // different picture is a different client too.
        if (strcmp(was, g_want) || strcmp(was_arg, g_arg) || (!g_pid && g_want[0])) {
            stop();
            g_failures = 0;
            g_retry_at = 0;
            if (g_want[0]) start();
        }
    }
    if (!g_pid && g_want[0] && g_retry_at && sys_ticks() >= g_retry_at) {
        g_retry_at = 0;
        start();
    }
}

int wm_bg_create(int pid, uint32_t id, int w, int h, int *accepted) {
    (void)w; (void)h;
    if (!pid || pid != g_pid) return 0;
    *accepted = 0;
    if (g_win.open) return 1;   // one surface; a second window is refused
    memset(&g_win, 0, sizeof g_win);
    g_win.open = 1;
    g_win.client_pid = pid;
    g_win.client_win = id;
    *accepted = 1;
    // THE SCREEN'S SIZE, proposed at once -- a configure, which the
    // client answers by presenting at it (lib/ulivewall.c opens small).
    wm_bg_screen_changed();
    return 1;
}

int wm_bg_present(int pid, uint32_t id, int front, uint32_t gen, int w, int h, uint32_t seq) {
    if (!g_win.open || pid != g_win.client_pid || id != g_win.client_win) return 0;
    if (front < 0 || front >= WIN_CLIENT_BUFS) return 1;
    if (!wm_client_map_buf(&g_win, front, gen, w, h)) return 1;
    int old = g_win.client_front;
    g_win.client_front = front;
    g_win.client_buf = g_win.client_px[front];
    g_win.client_seq[front] = seq;
    g_win.client_w = w;
    g_win.client_h = h;
    if (old != front) wm_client_release_buf(&g_win, old);
    int full = w == screen_w && h == screen_h;
    if (full && !g_have_frame) {
        g_have_frame = 1;
        g_failures = 0;
        // The glass that samples the wallpaper takes THIS frame, once:
        // re-blurring a moving background every frame is the cost
        // that kind exists to avoid (wm_glass.c).
        desktop_background_changed();
    }
    if (full) {
        g_frames++;
        wm_damage_rect(0, 0, screen_w, screen_h);
        redraw_pending = 1;
    }
    return 1;
}

int wm_bg_destroy(int pid, uint32_t id) {
    if (!g_win.open || pid != g_win.client_pid || id != g_win.client_win) return 0;
    drop_surface();
    wm_damage_rect(0, 0, screen_w, screen_h);
    redraw_pending = 1;
    return 1;
}

int wm_bg_timer(int pid, uint32_t id, unsigned ms) {
    if (!g_win.open || pid != g_win.client_pid || id != g_win.client_win) return 0;
    g_win.timer_period_ns = (uint64_t)ms * 1000000ull;
    g_win.timer_due_ns = ms ? sys_monotonic_ns() + g_win.timer_period_ns : 0;
    return 1;
}

void wm_bg_client_gone(int pid) {
    if (!pid || pid != g_pid) return;
    wm_logf("wm: background %s (pid %d) exited\n", g_running, pid);
    g_pid = 0;
    g_running[0] = '\0';
    drop_surface();
    wm_damage_rect(0, 0, screen_w, screen_h);
    redraw_pending = 1;
    // A CLIENT THAT KEEPS DYING BEFORE ITS FIRST FRAME is broken, not
    // unlucky: the picture stays until the settings change.
    if (++g_failures < FAILURES_MAX) g_retry_at = sys_ticks() + RETRY_TICKS;
    else wm_logf("wm: background %s keeps failing -- showing the picture\n", g_want);
}

// Whether the desktop can be seen. A maximized or fullscreen window, a
// screensaver, or the Leave page each leave nothing of it showing.
static int hidden(void) {
    if (wm_idle_saver_pid() || leave_page_covers()) return 1;
    for (int i = 0; i < window_count; i++) {
        const struct window *w = &windows[i];
        if (!w->open || w->popup || w->state == WIN_MINIMIZED) continue;
        if (w->fullscreen || w->state == WIN_MAXIMIZED) return 1;
    }
    return 0;
}

uint64_t wm_bg_timer_due(void) {
    if (!g_win.open || !g_win.timer_period_ns || g_paused) return 0;
    return g_win.timer_due_ns;
}

void wm_bg_check_timer(uint64_t now) {
    if (!g_win.open || !g_win.timer_period_ns) return;
    int paused = g_pause_on && hidden();
    if (paused != g_paused) {
        g_paused = paused;
        wm_logf("wm: background %s\n", paused ? "paused" : "resumed");
    }
    if (paused || now < g_win.timer_due_ns) return;
    struct win_event ev = {0};
    ev.type = WIN_EV_TIMER;
    ev.window = g_win.client_win;
    wm_client_push_event(g_win.client_pid, &ev);
    g_win.timer_due_ns += g_win.timer_period_ns;
    if (g_win.timer_due_ns <= now) g_win.timer_due_ns = now + g_win.timer_period_ns;
}

void wm_bg_screen_changed(void) {
    if (!g_win.open) return;
    struct win_event ev = {0};
    ev.type = WIN_EV_RESIZE;
    ev.window = g_win.client_win;
    ev.a = screen_w;
    ev.b = screen_h;
    wm_client_push_event(g_win.client_pid, &ev);
}

const uint32_t *wm_bg_frame(void) {
    if (!g_have_frame || !g_win.client_buf) return 0;
    if (g_win.client_w != screen_w || g_win.client_h != screen_h) return 0;
    return g_win.client_buf;
}

int wm_bg_describe(char *out, int cap) {
    return snprintf(out, (size_t)cap,
                    "{\"pid\":%d,\"program\":\"%s\",\"frames\":%u,\"paused\":%d,"
                    "\"pause\":\"%s\",\"shown\":%d}",
                    g_pid, g_running, g_frames, g_paused, g_pause_on ? "on" : "off",
                    wm_bg_frame() != 0);
}
