// The Leave page -- see leave_page.h for what it is and why it asks apps
// to close first.
//
// **THE BACKDROP IS ONE SNAPSHOT**, blurred and dimmed when the page
// first draws and copied each frame after: a full-screen blur is far too
// slow to redo per frame in software (docs/decisions/gui.md's
// transparency entry measured a cached Start menu alone at 3.4 ms), and
// the page asks for full-screen damage while it is up.
#include "leave_page.h"
#include "close_batch.h"
#include "wm_internal.h"
#include "wm_overlay.h"
#include "wm_log.h"
#include "context_menu.h"
#include "lib/icon_cache.h"
#include "lib/ubootmenu.h"
#include "ui/uui.h"
#include "ui/utheme.h"
#include "kapi.h"
#include "keyboard.h"   // KEY_ARROW_*
#include "rt/sys.h"     // sys_poweroff()
#include <stdint.h>
#include <stdlib.h>

int leave_page_open = 0;

enum phase { PH_CHOOSE, PH_CLOSING, PH_STUCK };
static enum phase g_phase;
static int g_focus;                 // CHOOSE: an action; STUCK: 0 Cancel, 1 anyway
static int g_action;                // what was chosen
static int g_dry;                   // `gui leave dry`: log instead of acting

// Restart into a GRUB entry (lib/ubootmenu.h): one boot only.
static struct ubootmenu g_boot;
static int g_boot_ok;               // two entries or more, and a one-shot slot
static int g_into = -1;             // the entry picked, or -1 for the default
static char g_into_label[UBOOTMENU_MAX][UBOOTMENU_TITLE + 12];
static struct context_menu_item g_into_item[UBOOTMENU_MAX];
static char g_into_link[UBOOTMENU_TITLE + 24];

// The apps asked to close (close_batch.h).
// MORE THAN THIS MANY WINDOWS AND THE PAGE WILL NOT ACT: an app it did
// not ask would lose its work unasked. Far past any real session here.
#define LEAVE_APPS 128
static struct close_batch_entry g_store[LEAVE_APPS];
static struct close_batch g_apps = { g_store, LEAVE_APPS, 0, 0 };

static uint32_t *g_snap;
static int g_snap_w, g_snap_h, g_snap_ok;
static char g_note[96];             // why the last action did not happen

enum { C_RESTART, C_SHUTDOWN, C_EXIT, C_INTO, C_CANCEL, C_ANYWAY, C_ROW0,
       CTLS = C_ROW0 + LEAVE_APPS };
struct rect { int x, y, w, h; };
static struct rect g_r[CTLS];
static int g_rows_shown;            // app rows that fit the screen
static int g_hot = -1, g_pressed = -1;

static const char *const NAME[LEAVE_ACTIONS] = { "Restart", "Shut down", "Exit to shell" };
static const char *const ANYWAY[LEAVE_ACTIONS] = { "Restart anyway", "Shut down anyway", "Exit anyway" };
static const char *const DOING[LEAVE_ACTIONS] = { "restarting", "shutting down", "exiting to the shell" };
static const char *const THEN[LEAVE_ACTIONS] = {
    "Then the computer restarts.", "Then the computer shuts down.",
    "Then toy-os returns to the text shell.",
};

// --- the apps ------------------------------------------------------------

static int listed(const struct window *w) {
    return wm_client_is_client_window(w) && !w->popup && !w->dialog;
}

static int remaining(void) { return close_batch_remaining(&g_apps); }

// --- acting -----------------------------------------------------------------

// Back to the choice with a reason on it: the action did not happen.
static void refused(const char *why) {
    k_strlcpy(g_note, why, sizeof g_note);
    wm_logf("leave: %s\n", why);
    g_phase = PH_CHOOSE;
    close_batch_reset(&g_apps);
    redraw_pending = 1;
}

static void perform(void) {
    const char *into = g_into >= 0 && g_into != g_boot.def ? g_boot.title[g_into] : 0;
    if (g_dry) {
        wm_logf("leave: would %s%s%s\n", g_action == LEAVE_RESTART ? "restart"
                : g_action == LEAVE_SHUTDOWN ? "shut down" : "exit to the shell",
                into && g_action == LEAVE_RESTART ? " into " : "",
                into && g_action == LEAVE_RESTART ? into : "");
        leave_page_cancel();
        return;
    }
    wm_logf("leave: %s\n", NAME[g_action]);
    switch (g_action) {
    case LEAVE_SHUTDOWN:
        sys_poweroff(0);
        // RETURNING IS FAILURE (the firmware refused): say so, once,
        // rather than be asked again by every frame's poll.
        refused("The computer could not shut down.");
        break;
    case LEAVE_RESTART:
        // ONLY A PICKED ENTRY touches the boot choice: a plain Restart
        // keeps a one-shot Boot Manager or bootcfg left pending. Picking
        // the default clears one (`into` is then NULL). A choice that
        // cannot be saved is not a restart into the wrong system.
        if (g_into >= 0 && ubootmenu_set_next(into) < 0) {
            refused("Could not save the boot choice, so nothing restarted.");
            break;
        }
        sys_poweroff(1);
        if (g_into >= 0) ubootmenu_set_next(0);   // the choice is taken back
        refused("The computer could not restart.");
        break;
    default:
        leave_page_cancel();
        wm_exit_requested = 1;
        break;
    }
}

static void begin(int action) {
    g_action = action;
    close_batch_reset(&g_apps);
    g_note[0] = 0;
    int all = 0;
    for (int i = 0; i < window_count; i++) all += listed(&windows[i]);
    if (all > LEAVE_APPS) {
        refused("Too many windows are open to ask; close some first.");
        return;
    }
    for (int i = 0; i < window_count; i++)
        if (listed(&windows[i])) close_batch_add(&g_apps, i);
    wm_logf("leave: %s chosen, asking %d app(s) to close\n", NAME[action], g_apps.n);
    if (!g_apps.n) { perform(); return; }
    close_batch_ask(&g_apps);
    g_phase = PH_CLOSING;
    g_focus = 0;
    redraw_pending = 1;
}

void leave_page_poll(void) {
    if (!leave_page_open || g_phase == PH_CHOOSE) return;
    int changed = close_batch_update(&g_apps);
    if (!remaining()) { perform(); return; }
    if (g_phase == PH_CLOSING && close_batch_expired(&g_apps)) {
        g_phase = PH_STUCK;
        g_focus = 0;   // Cancel: the safe answer is the default one
        wm_logf("leave: %d app(s) did not close\n", remaining());
        changed = 1;
    }
    if (changed) redraw_pending = 1;
}

// --- opening and closing -------------------------------------------------------

static void into_pick(void *ctx) {
    g_into = (int)(intptr_t)ctx;
    begin(LEAVE_RESTART);
}

void leave_page_show(enum leave_action focus) {
    wm_overlay_close_others("leave");
    leave_page_open = 1;
    g_phase = PH_CHOOSE;
    g_focus = focus;
    g_into = -1;
    g_note[0] = 0;
    g_hot = g_pressed = -1;
    g_snap_ok = 0;   // taken at the first draw, of the scene as it is then
    g_boot_ok = ubootmenu_read(&g_boot, 0) >= 2 && g_boot.oneshot;
    if (g_boot_ok) {
        for (int i = 0; i < g_boot.count; i++) {
            k_snprintf(g_into_label[i], sizeof g_into_label[i], "%s%s", g_boot.title[i],
                       i == g_boot.def ? " (default)" : "");
            k_memset(&g_into_item[i], 0, sizeof g_into_item[i]);
            g_into_item[i].label = g_into_label[i];
            g_into_item[i].on_select = into_pick;
            g_into_item[i].ctx = (void *)(intptr_t)i;
        }
        // Named by the entry that is NOT the default -- the reason to
        // be here at all.
        int other = g_boot.def == 0 ? 1 : 0;
        k_snprintf(g_into_link, sizeof g_into_link, "Restart into: %s", g_boot.title[other]);
    }
    wm_logf("leave: page open, %s focused\n", NAME[focus]);
    redraw_pending = 1;
}

void leave_page_cancel(void) {
    if (!leave_page_open) return;
    leave_page_open = 0;
    close_batch_reset(&g_apps);
    // The snapshot is a whole screen: not kept between openings.
    free(g_snap);
    g_snap = 0;
    g_snap_w = g_snap_h = g_snap_ok = 0;
    wm_logf("leave: closed\n");
    redraw_pending = 1;
}

// --- layout -----------------------------------------------------------------

static int u(void) { return ugfx_char_h(); }
static int disc(void) { return u() * 6; }
static int btn_h(void) { return utheme_control_h() + u() / 2; }
static int btn_w(const char *t) { return ugfx_text_width(t) + 3 * u(); }

static void layout(void) {
    k_memset(g_r, 0, sizeof g_r);
    int W = screen_w, H = screen_h, D = disc(), gap = u() * 3;
    if (g_phase == PH_CHOOSE) {
        int total = LEAVE_ACTIONS * D + (LEAVE_ACTIONS - 1) * gap;
        int x0 = (W - total) / 2, top = H / 2 - D - u() * 2;
        for (int i = 0; i < LEAVE_ACTIONS; i++)
            g_r[C_RESTART + i] = (struct rect){ x0 + i * (D + gap) - u(), top, D + 2 * u(), D + 2 * u() };
        int y = top + D + u() * 4;
        if (g_boot_ok) {
            int lw = ugfx_text_width(g_into_link) + u();
            g_r[C_INTO] = (struct rect){ (W - lw) / 2, y + u() * 2, lw, u() + 4 };
        }
        int cw = btn_w("Cancel");
        g_r[C_CANCEL] = (struct rect){ (W - cw) / 2, y + u() * 5, cw, btn_h() };
        return;
    }
    int rw = u() * 28, rh = u() * 2, rg = u() / 2;
    // AS MANY ROWS AS FIT, the buttons always on screen; the rest are a
    // count under them (draw_closing).
    int fixed = D + u() * 5 + u() * 2 + btn_h() + u() * 2 + 2 * u();
    int fit = (H - fixed) / (rh + rg);
    int rows = g_apps.n < fit ? g_apps.n : fit < 1 ? 1 : fit;
    g_rows_shown = rows;
    int block = D + u() * 5 + rows * (rh + rg) + u() * 2 + btn_h();
    int top = (H - block) / 2;
    int y = top + D + u() * 5;
    for (int i = 0; i < rows; i++, y += rh + rg)
        g_r[C_ROW0 + i] = (struct rect){ (W - rw) / 2, y, rw, rh };
    y += u() * 2;
    int cw = btn_w("Cancel");
    if (g_phase == PH_STUCK) {
        int aw = btn_w(ANYWAY[g_action]), both = cw + u() + aw;
        g_r[C_CANCEL] = (struct rect){ (W - both) / 2, y, cw, btn_h() };
        g_r[C_ANYWAY] = (struct rect){ (W - both) / 2 + cw + u(), y, aw, btn_h() };
    } else {
        g_r[C_CANCEL] = (struct rect){ (W - cw) / 2, y, cw, btn_h() };
    }
}

// --- drawing ---------------------------------------------------------------------

#define WHITE ugfx_rgb(255, 255, 255)
#define SOFT  ugfx_rgb(184, 198, 214)

static void snapshot(struct ugfx_surface *s) {
    size_t n = (size_t)s->w * (size_t)s->h;
    if (!g_snap || g_snap_w != s->w || g_snap_h != s->h) {
        free(g_snap);
        g_snap = malloc(n * 4);
        g_snap_w = g_snap ? s->w : 0;
        g_snap_h = g_snap ? s->h : 0;
    }
    if (!g_snap) return;
    // Filled first: the blur writes nothing if it cannot get its own
    // scratch, and the page must never show uninitialised memory.
    for (size_t i = 0; i < n; i++) g_snap[i] = ugfx_rgb(10, 20, 35);
    ugfx_blur_rect(s, 0, 0, s->w, s->h, u(), g_snap);
    // Dimmed toward a dark blue-grey, the mockup's rgba(10,20,35,.72).
    for (size_t i = 0; i < n; i++) {
        uint32_t p = g_snap[i];
        uint32_t r = ((p >> 16) & 0xFF) * 28 / 100 + 7, g = ((p >> 8) & 0xFF) * 28 / 100 + 14,
                 b = (p & 0xFF) * 28 / 100 + 25;
        g_snap[i] = r << 16 | g << 8 | b;
    }
    g_snap_ok = 1;
}

static void text_c(struct ugfx_surface *s, int cx, int y, const char *t, uint32_t c) {
    int w = ugfx_text_width(t), max = screen_w - 2 * u();
    if (w > max) w = max;
    ugfx_draw_string_clipped(s, cx - w / 2, y, w + 1, t, c, UGFX_TRANSPARENT);
}

static void glyph(struct ugfx_surface *s, int action, int cx, int cy, uint32_t c) {
    int sz = u() * 2;
    if (action == LEAVE_EXIT) {
        // A terminal: a frame, a prompt and a cursor. No icon file has one.
        int w = sz, h = sz * 4 / 5, x = cx - w / 2, y = cy - h / 2;
        for (int k = 0; k < 2; k++) ugfx_draw_rect(s, x + k, y + k, w - 2 * k, h - 2 * k, c);
        int p = h / 4;
        ugfx_draw_line(s, x + p, y + p, x + 2 * p, y + 2 * p, c, GEOM_AA);
        ugfx_draw_line(s, x + 2 * p, y + 2 * p, x + p, y + 3 * p, c, GEOM_AA);
        ugfx_fill_rect(s, x + 2 * p + p / 2, y + 3 * p - 1, p * 2, 2, c);
        return;
    }
    const struct uimg *ico = icon_get(action == LEAVE_RESTART ? "tb-refresh" : "tb-power", sz);
    if (ico) ugfx_blit_tinted(s, cx - ico->w / 2, cy - ico->h / 2, ico->w, ico->h, ico->px, ico->w, c);
}

static void disc_at(struct ugfx_surface *s, int action, int x, int y, int lit, int hot) {
    int D = disc();
    if (lit) {
        uui_fill_round_rect(s, x, y, D, D, UUI_CAPSULE, WHITE);
        uui_fill_round_rect(s, x + 2, y + 2, D - 4, D - 4, UUI_CAPSULE, UTHEME_ACCENT);
    } else {
        uui_glass_round_rect(s, x, y, D, D, UUI_CAPSULE, WHITE, hot ? 60 : 36, 90);
    }
    glyph(s, action, x + D / 2, y + D / 2, WHITE);
}

static void glass_button(struct ugfx_surface *s, int id, const char *label, int danger, int focused) {
    const struct rect *r = &g_r[id];
    int hot = g_hot == id;
    if (danger) {
        uint32_t c = utheme_action(UTHEME_ACT_DANGER);
        uui_fill_round_rect(s, r->x, r->y, r->w, r->h, u() / 3, hot ? uui_state_bg(c, UUI_STATE_HOVER) : c);
    } else {
        uui_glass_round_rect(s, r->x, r->y, r->w, r->h, u() / 3, WHITE, hot ? 56 : 30, 130);
    }
    if (focused) {
        ugfx_draw_rect(s, r->x - 3, r->y - 3, r->w + 6, r->h + 6, WHITE);
    }
    text_c(s, r->x + r->w / 2, r->y + (r->h - u()) / 2, label, WHITE);
}

static void draw_choose(struct ugfx_surface *s) {
    int D = disc();
    for (int i = 0; i < LEAVE_ACTIONS; i++) {
        const struct rect *r = &g_r[C_RESTART + i];
        disc_at(s, i, r->x + u(), r->y, g_focus == i, g_hot == C_RESTART + i);
        text_c(s, r->x + r->w / 2, r->y + D + u() / 2, NAME[i], WHITE);
    }
    int y = g_r[C_RESTART].y + D + u() * 4;
    char line[160];
    int n = 0;
    const char *first = 0, *second = 0;
    for (int i = 0; i < window_count; i++) {
        if (!listed(&windows[i])) continue;
        if (n == 0) first = windows[i].title;
        else if (n == 1) second = windows[i].title;
        n++;
    }
    if (!n) k_snprintf(line, sizeof line, "No apps are open.");
    else if (n == 1) k_snprintf(line, sizeof line, "%s will be asked to close.", first);
    else if (n == 2) k_snprintf(line, sizeof line, "%s and %s will be asked to close.", first, second);
    else k_snprintf(line, sizeof line, "%s, %s and %d more will be asked to close.", first, second, n - 2);
    text_c(s, screen_w / 2, y, g_note[0] ? g_note : line,
           g_note[0] ? ugfx_rgb(255, 179, 173) : SOFT);
    if (g_boot_ok) {
        const struct rect *r = &g_r[C_INTO];
        uint32_t c = ugfx_rgb(207, 224, 245);
        ugfx_draw_string_clipped(s, r->x, r->y, r->w - u(), g_into_link, c, UGFX_TRANSPARENT);
        if (g_hot == C_INTO) ugfx_fill_rect(s, r->x, r->y + u(), r->w - u(), 1, c);
        int cx = r->x + r->w - u() / 2, cy = r->y + u() / 2, k = u() / 4;   // a down chevron
        ugfx_draw_line(s, cx - k, cy - k / 2, cx, cy + k / 2, c, GEOM_AA);
        ugfx_draw_line(s, cx, cy + k / 2, cx + k, cy - k / 2, c, GEOM_AA);
    }
    glass_button(s, C_CANCEL, "Cancel", 0, g_focus == LEAVE_ACTIONS);
}

static void draw_closing(struct ugfx_surface *s) {
    int D = disc(), cx = screen_w / 2;
    int top = g_r[C_ROW0].y - D - u() * 5;
    disc_at(s, g_action, cx - D / 2, top, 1, 0);
    char t[128];
    int left = remaining();
    if (g_phase == PH_STUCK) {
        k_snprintf(t, sizeof t, "%s keeping toy-os from %s", left > 1 ? "Apps are" : "An app is",
                   DOING[g_action]);
        text_c(s, cx, top + D + u(), t, WHITE);
        k_snprintf(t, sizeof t, "Cancel to go back and save your work, or %s anyway and lose it.",
                   g_action == LEAVE_RESTART ? "restart" : g_action == LEAVE_SHUTDOWN ? "shut down" : "exit");
        text_c(s, cx, top + D + u() * 2 + u() / 2, t, SOFT);
    } else {
        text_c(s, cx, top + D + u(), "Closing apps...", WHITE);
        text_c(s, cx, top + D + u() * 2 + u() / 2, THEN[g_action], SOFT);
    }
    for (int i = 0; i < g_rows_shown; i++) {
        const struct rect *r = &g_r[C_ROW0 + i];
        int gone = g_apps.e[i].gone;
        uui_glass_round_rect(s, r->x, r->y, r->w, r->h, u() / 2, WHITE,
                             gone ? 12 : g_hot == C_ROW0 + i ? 40 : 26, gone ? 40 : 90);
        const char *st = gone ? "closed" : g_phase == PH_STUCK ? "still open" : "closing...";
        uint32_t sc = gone ? SOFT : g_phase == PH_STUCK ? ugfx_rgb(255, 179, 173) : SOFT;
        int sw = ugfx_text_width(st);
        int ty = r->y + (r->h - u()) / 2;
        ugfx_draw_string_elided(s, r->x + u(), ty, r->w - 3 * u() - sw, g_apps.e[i].title,
                                gone ? SOFT : WHITE, UGFX_TRANSPARENT);
        ugfx_draw_string_clipped(s, r->x + r->w - u() - sw, ty, sw + 1, st, sc, UGFX_TRANSPARENT);
    }
    if (g_rows_shown < g_apps.n) {
        k_snprintf(t, sizeof t, "and %d more", g_apps.n - g_rows_shown);
        const struct rect *last = &g_r[C_ROW0 + g_rows_shown - 1];
        text_c(s, cx, last->y + last->h + u() / 2, t, SOFT);
    }
    glass_button(s, C_CANCEL, "Cancel", 0, g_phase == PH_STUCK && g_focus == 0);
    if (g_phase == PH_STUCK) glass_button(s, C_ANYWAY, ANYWAY[g_action], 1, g_focus == 1);
}

void leave_page_draw(int mx, int my) {
    (void)mx; (void)my;
    if (!leave_page_open) return;
    struct ugfx_surface *s = wm_surface();
    if (!g_snap_ok || g_snap_w != s->w || g_snap_h != s->h) snapshot(s);
    if (g_snap_ok) ugfx_blit(s, 0, 0, g_snap_w, g_snap_h, g_snap, g_snap_w);
    else ugfx_fill_rect(s, 0, 0, s->w, s->h, ugfx_rgb(10, 20, 35));
    layout();
    if (g_phase == PH_CHOOSE) draw_choose(s);
    else draw_closing(s);
}

// --- input ---------------------------------------------------------------------

static int ctl_at(int x, int y) {
    for (int i = 0; i < CTLS; i++) {
        const struct rect *r = &g_r[i];
        if (r->w > 0 && x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h) return i;
    }
    return -1;
}

static void activate(int id) {
    if (id >= C_RESTART && id <= C_EXIT) { begin(id - C_RESTART); return; }
    if (id == C_INTO) {
        const struct rect *r = &g_r[C_INTO];
        // A child of this page, as a row's menu is of Start.
        wm_overlay_set_parent("leave");
        context_menu_open_at(r->x, r->y + r->h, g_into_item, g_boot.count);
        return;
    }
    if (id == C_CANCEL) { leave_page_cancel(); return; }
    if (id == C_ANYWAY) { perform(); return; }
    if (id >= C_ROW0 && id < C_ROW0 + g_rows_shown) {
        // An app still open: back to it, with the page gone -- whatever it
        // is asking is behind the page.
        int idx = close_batch_window(&g_apps, id - C_ROW0);
        leave_page_cancel();
        if (idx >= 0) raise_with_dialogs(idx);
    }
}

int leave_page_handle_click(int mx, int my) {
    if (!leave_page_open) return 0;
    layout();
    g_pressed = ctl_at(mx, my);   // armed here, committed on release
    redraw_pending = 1;
    return 1;                     // modal: nothing under it hears the click
}

void leave_page_update_press(int mx, int my, uint8_t buttons) {
    if (!leave_page_open || (buttons & 1)) return;
    int id = g_pressed;
    g_pressed = -1;
    layout();
    if (id >= 0 && ctl_at(mx, my) == id) activate(id);
}

int leave_page_hover_at(int mx, int my) {
    if (!leave_page_open) return 0;
    layout();
    g_hot = ctl_at(mx, my);
    return g_hot + 1;
}

void leave_page_damage(void) { redraw_pending = 1; }

int leave_page_key(int key, uint8_t mods) {
    (void)mods;
    if (!leave_page_open) return 0;
    int enter = key == '\n' || key == '\r';
    if (key == 0x1B) { leave_page_cancel(); return 1; }
    if (g_phase == PH_CHOOSE) {
        // The three actions and Cancel, in a ring: Left/Right, Tab.
        int n = LEAVE_ACTIONS + 1;
        if (key == KEY_ARROW_RIGHT || key == '\t') g_focus = (g_focus + 1) % n;
        else if (key == KEY_ARROW_LEFT) g_focus = (g_focus + n - 1) % n;
        else if (enter) {
            if (g_focus == LEAVE_ACTIONS) leave_page_cancel();
            else begin(g_focus);
            return 1;
        }
        redraw_pending = 1;
        return 1;
    }
    if (g_phase == PH_STUCK) {
        if (key == KEY_ARROW_RIGHT || key == KEY_ARROW_LEFT || key == '\t') g_focus = !g_focus;
        else if (enter) { if (g_focus) perform(); else leave_page_cancel(); return 1; }
        redraw_pending = 1;
    }
    return 1;   // modal: keys go nowhere else while it is up
}

// --- for the debug console -----------------------------------------------------------

const char *leave_page_phase(void) {
    if (!leave_page_open) return "closed";
    return g_phase == PH_CHOOSE ? "choose" : g_phase == PH_CLOSING ? "closing" : "stuck";
}

int leave_page_focus(void) { return g_focus; }

int leave_page_control(int i, const char **name, int *x, int *y, int *w, int *h) {
    static const char *const CN[] = { "restart", "shutdown", "exit", "into", "cancel", "anyway" };
    if (!leave_page_open || i < 0 || i >= C_ROW0) return 0;
    layout();
    if (g_r[i].w <= 0) return 0;
    *name = CN[i];
    *x = g_r[i].x; *y = g_r[i].y; *w = g_r[i].w; *h = g_r[i].h;
    return 1;
}

int leave_page_app(int i, const char **title, int *gone) {
    if (i < 0 || i >= g_apps.n) return 0;
    *title = g_apps.e[i].title;
    *gone = g_apps.e[i].gone;
    return 1;
}

void leave_page_set_dry_run(int on) { g_dry = on; }

int leave_page_covers(void) { return leave_page_open && g_snap_ok; }
