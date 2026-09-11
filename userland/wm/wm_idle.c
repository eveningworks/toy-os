// See wm_idle.h.
#include "wm_internal.h"
#include "wm_idle.h"
#include "wm/wm_conf.h"
#include "wm_log.h"
#include "wm_rawin.h"
#include "rt/sys.h"
#include "screensaver_config.h"
#include "string.h"
#include <stdio.h>

#define DESKTOP_CONF "/etc/desktop.conf"

static uint64_t g_last_input;      // ticks
static int g_saver_pid;
static uint32_t g_seen_generation;
static int g_idle_minutes = SCREENSAVER_IDLE_DEFAULT;
static char g_saver[64] = SCREENSAVER_DEFAULT;

// PIT_HZ ticks a second (api/timer.h), which is what sys_ticks() counts.
#define TICKS_PER_SEC 100u

static void adopt_settings(void) {
    char v[64];
    if (wm_conf_get(DESKTOP_CONF, "screensaver", v, sizeof v) && v[0])
        strlcpy(g_saver, v, sizeof g_saver);
    if (wm_conf_get(DESKTOP_CONF, "screensaver_idle", v, sizeof v) && v[0]) {
        int n = 0;
        for (const char *p = v; *p >= '0' && *p <= '9'; p++) n = n * 10 + (*p - '0');
        if (n >= 0 && n <= SCREENSAVER_IDLE_MAX) g_idle_minutes = n;
    }
}

static void stop_saver(void) {
    if (!g_saver_pid) return;
    // KILLED, NOT ASKED. wm_request_close() lets a client refuse, which
    // is right for a document with unsaved work and wrong for this: the
    // user has touched the keyboard and wants the screen back, and a
    // saver that could decline would be a way to lose the machine.
    //
    // The ZOMBIE is wm.c's reaper's, because start_saver() handed it the
    // pid -- one reaper for everything the desktop launches, rather than
    // a second one here racing it for the same child.
    sys_kill(g_saver_pid, 9);
    wm_logf("wm: screensaver stopped (pid %d)\n", g_saver_pid);
    g_saver_pid = 0;
}

// THE SPRITE THAT IS ALREADY ON SCREEN. draw_cursor_at() skips the
// pointer while a saver runs, but skipping is not erasing: the arrow
// drawn before the saver started stays in the framebuffer until
// something repaints that region, and on a quiet machine nothing does.
// So a saver starting damages where the pointer is and asks for a
// frame -- the pair CLAUDE.md insists on, because damage alone does not
// request one.
static void forget_cursor(void) {
    int mx, my;
    uint8_t buttons;
    wm_rawin_mouse(&mx, &my, &buttons);
    wm_damage_rect(mx - 4, my - 4, 40, 48);
    redraw_pending = 1;
}

static void start_saver(void) {
    char path[128];
    snprintf(path, sizeof path, "%s/%s", SCREENSAVER_DIR, g_saver);
    int pid = sys_spawn(path, 0, -1);
    if (pid > 0) {
        g_saver_pid = pid;
        wm_track_launched(pid);   // so wm.c's reaper owns the zombie
        forget_cursor();
        wm_logf("wm: screensaver %s started as pid %d\n", g_saver, pid);
        return;
    }
    // A NAME WITH NO PROGRAM BEHIND IT. The registry enumerates this
    // directory, so it should be impossible -- but a binary deleted
    // under a running machine is not, and a saver that silently never
    // starts is indistinguishable from an idle timer that does not
    // work. Say so once, then stop trying until the setting changes.
    wm_logf("wm: screensaver FAILED to start -- no %s\n", path);
    g_idle_minutes = 0;
}

void wm_idle_poll(int active) {
    uint32_t gen = wm_setting_generation();
    if (gen != g_seen_generation) {
        g_seen_generation = gen;
        char was[sizeof g_saver];
        strlcpy(was, g_saver, sizeof was);
        int was_minutes = g_idle_minutes;
        adopt_settings();
        // ONLY A CHANGE TO THIS FEATURE'S OWN SETTINGS INTERRUPTS A
        // RUNNING SAVER, and the generation cannot say which setting
        // moved -- every setting in the system shares one. Stopping on
        // any change killed the saver whenever anything else was
        // written, INCLUDING the write that selected it, so choosing a
        // saver and starting it in the same breath never worked.
        //
        // The idle clock is deliberately NOT reset here either: writing
        // an unrelated setting from a script is not a person at the
        // keyboard, and treating it as one would keep the screen awake
        // for as long as anything was polling.
        if (g_saver_pid && (strcmp(was, g_saver) != 0 ||
                            (was_minutes && !g_idle_minutes)))
            stop_saver();
    }

    if (active) {
        g_last_input = sys_ticks();
        stop_saver();
        return;
    }

    // The saver has gone away on its own -- it crashed, or something
    // killed it. Treat that as the user having woken the screen rather
    // than restarting it in a loop.
    //
    // A pid wm.c's reaper already collected answers here too: waitpid
    // on it is an error, which is not SYS_RETRY, which is "gone". Both
    // readings are the one this wants.
    int code;
    if (g_saver_pid && sys_waitpid_nohang(g_saver_pid, &code) != SYS_RETRY) {
        wm_logf("wm: screensaver (pid %d) exited on its own\n", g_saver_pid);
        g_saver_pid = 0;
        g_last_input = sys_ticks();
        return;
    }

    if (!g_idle_minutes || g_saver_pid) return;
    uint64_t quiet = sys_ticks() - g_last_input;
    if (quiet >= (uint64_t)g_idle_minutes * 60u * TICKS_PER_SEC) start_saver();
}

int wm_idle_saver_pid(void) { return g_saver_pid; }

uint32_t wm_idle_seconds(void) {
    uint64_t quiet = sys_ticks() - g_last_input;
    return (uint32_t)(quiet / TICKS_PER_SEC);
}

const char *wm_idle_saver_name(void) { return g_saver; }
int wm_idle_minutes(void) { return g_idle_minutes; }

int wm_idle_force_start(void) {
    if (g_saver_pid) return 1;
    // The settings may never have been read: nothing has changed the
    // generation on a machine where nobody has touched a setting, so the
    // adopt in wm_idle_poll() has not run.
    adopt_settings();
    // IT SKIPS THE WAIT, NOT THE SETTING. A timeout of zero means the
    // feature is off, and a lever that started a saver anyway would
    // leave "off" untestable -- which is the half of a setting most
    // likely to be wrong.
    if (!g_idle_minutes) return 0;
    start_saver();
    return g_saver_pid != 0;
}

void wm_idle_force_stop(void) {
    stop_saver();
    g_last_input = sys_ticks();
}

void wm_idle_adopt_saver(int pid) {
    if (pid <= 0) return;
    // ALREADY OURS -- the idle clock spawned it and is tracking it, so
    // the window it has just opened is the one we were waiting for.
    if (pid == g_saver_pid) return;
    // Somebody else's. Stop whatever we had, because two savers on
    // screen is not a state with a right answer, and take this one.
    stop_saver();
    g_saver_pid = pid;
    g_last_input = sys_ticks();   // it is on screen NOW, not in a minute
    forget_cursor();
    wm_logf("wm: adopted screensaver pid %d (started elsewhere)\n", pid);
}
