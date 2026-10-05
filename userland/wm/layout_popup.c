// See layout_popup.h. The tray's keyboard-layout item and its switcher.
#include "wm_internal.h"
#include "layout_popup.h"
#include "wm_flyout.h"
#include "wm_tray.h"
#include "wm_overlay.h"
#include "wm_log.h"
#include "ui/uui.h"
#include "ui/utheme.h"
#include "kapi.h"
#include "rt/sys.h"
#include "lib/usetting.h"
#include "lib/uconf.h"
#include "wm/wm_conf.h"   // wm_setting_generation()

#define SET_LIST   "system.keyboard_layouts"
#define SET_ACTIVE "system.keyboard_layout"
#define NAMES_FILE "/etc/settings.d/system.keyboard_layout"
#define FOOT_LABEL "Keyboard settings..."

int layout_open = 0;

static int g_tray_id = -1;
static uint32_t g_seen_generation;
static char g_codes[LAYOUT_MAX][16];
static char g_names[LAYOUT_MAX][SETTING_ABI_VALUE_MAX];
static int g_n;
static int g_active = -1;
static char g_tray_text[16];

// The flyout's two modes: a click opened it (rows act on a click), or
// Super+Space did (the pick follows Space, Super's release commits).
static int g_switching;
static int g_pick = -1;
static int g_hover = -1;          // a row, LAYOUT_MAX for the footer button

// --- reading ------------------------------------------------------------

static void read_state(void) {
    char list[SETTING_ABI_VALUE_MAX], cur[SETTING_ABI_VALUE_MAX];
    if (!usetting_get(SET_LIST, list, sizeof list)) list[0] = 0;
    if (!usetting_get(SET_ACTIVE, cur, sizeof cur)) cur[0] = 0;
    g_n = 0;
    g_active = -1;
    for (const char *p = list; *p && g_n < LAYOUT_MAX; ) {
        const char *e = p;
        while (*e && *e != ',') e++;
        unsigned n = (unsigned)(e - p);
        if (n && n < sizeof g_codes[0]) {
            k_memcpy(g_codes[g_n], p, n);
            g_codes[g_n][n] = 0;
            char key[32];
            k_snprintf(key, sizeof key, "Choice.%s", g_codes[g_n]);
            if (!uconf_get(NAMES_FILE, key, g_names[g_n], sizeof g_names[g_n]))
                k_strlcpy(g_names[g_n], g_codes[g_n], sizeof g_names[g_n]);
            if (!k_strcmp(g_codes[g_n], cur)) g_active = g_n;
            g_n++;
        }
        p = *e ? e + 1 : e;
    }
    // The tray says the ACTIVE one, upper-case as a keycap would.
    const char *code = g_active >= 0 ? g_codes[g_active] : cur;
    unsigned i = 0;
    for (; code[i] && i + 1 < sizeof g_tray_text; i++)
        g_tray_text[i] = (char)(code[i] >= 'a' && code[i] <= 'z' ? code[i] - 32 : code[i]);
    g_tray_text[i] = 0;
}

static void apply_visibility(void) {
    tray_set_text(g_tray_id, g_tray_text);
    tray_set_hidden(g_tray_id, !tray_want_shown("layout", g_n > 1));
}

void layout_tray_init(void) {
    read_state();
    g_tray_id = tray_register(g_tray_text);
    g_seen_generation = wm_setting_generation();
    apply_visibility();
}

// A switch from anywhere -- this flyout, Settings, `config set` -- moves
// the registry generation, which is the one signal there is.
void layout_poll(void) {
    uint32_t gen = wm_setting_generation();
    if (gen == g_seen_generation) return;
    g_seen_generation = gen;
    read_state();
    apply_visibility();
    if (g_n < 2 && layout_open) layout_close();
    if (layout_open) layout_damage();
}

int layout_tray_hidden(void) { return tray_is_hidden(g_tray_id); }
int layout_count(void) { return g_n; }
int layout_active(void) { return g_active; }
int layout_pick(void) { return g_switching ? g_pick : -1; }
const char *layout_code(int i) { return i >= 0 && i < g_n ? g_codes[i] : ""; }

// --- geometry -------------------------------------------------------------

struct layout_geom {
    int x, y, w, h;
    int tray_x, tray_y, tray_w, tray_h;
    int rows_y, row_h, foot_h;
    int btn_x, btn_y, btn_w, btn_h;
};

static void geometry(struct layout_geom *g) {
    k_memset(g, 0, sizeof *g);
    struct wm_flyout_metrics m;
    wm_flyout_metrics(&m);
    int text_w = 0;
    for (int i = 0; i < g_n; i++) {
        int tw = ugfx_text_width(g_names[i]) + ugfx_text_width("WWW");
        if (tw > text_w) text_w = tw;
    }
    int w = m.pad * 2 + m.row_h + text_w + m.pad;
    int min_w = wm_flyout_button_w(FOOT_LABEL, 0) + m.pad * 2;
    if (w < min_w) w = min_w;
    if (w < ugfx_char_w() * 22) w = ugfx_char_w() * 22;
    g->row_h = m.row_h;
    g->foot_h = m.foot_h;
    int h = m.vpad + g_n * m.row_h + m.vpad + m.foot_h;

    int tx = 0, ty = 0, tw = 0, th = 0;
    if (!tray_item_rect(g_tray_id, &tx, &ty, &tw, &th)) {
        tx = screen_w - 32; ty = screen_h - taskbar_h; tw = 16; th = taskbar_h;
    }
    int x, y;
    wm_popup_place(tx + tw - w, screen_h - taskbar_h - h, w, h, &x, &y);
    g->x = x; g->y = y; g->w = w; g->h = h;
    g->tray_x = tx; g->tray_y = ty; g->tray_w = tw; g->tray_h = th;
    g->rows_y = y + m.vpad;
    g->btn_h = m.btn_h;
    g->btn_w = wm_flyout_button_w(FOOT_LABEL, 0);
    g->btn_x = x + w - m.pad - g->btn_w;
    g->btn_y = y + h - m.foot_h + (m.foot_h - m.btn_h) / 2;
}

int layout_rect(int *x, int *y, int *w, int *h) {
    struct layout_geom g;
    geometry(&g);
    *x = g.x; *y = g.y; *w = g.w; *h = g.h;
    return 1;
}

void layout_damage(void) { wm_overlay_damage("layout"); }

int layout_tray_center(int *x, int *y) {
    struct layout_geom g;
    geometry(&g);
    *x = g.tray_x + g.tray_w / 2; *y = g.tray_y + g.tray_h / 2;
    return !tray_is_hidden(g_tray_id);
}

int layout_row_center(int i, int *x, int *y) {
    if (i < 0 || i >= g_n) return 0;
    struct layout_geom g;
    geometry(&g);
    *x = g.x + g.w / 2; *y = g.rows_y + i * g.row_h + g.row_h / 2;
    return 1;
}

int layout_button_center(int *x, int *y) {
    struct layout_geom g;
    geometry(&g);
    *x = g.btn_x + g.btn_w / 2; *y = g.btn_y + g.btn_h / 2;
    return 1;
}

static void open_now(int switching) {
    wm_overlay_close_others("layout");
    read_state();
    layout_open = 1;
    g_switching = switching;
    g_hover = -1;
    layout_damage();
}

void layout_close(void) {
    if (!layout_open) return;
    layout_open = 0;   // where it was drawn is damaged by the core (wm_overlay.h)
    g_switching = 0;
    g_pick = -1;
    g_hover = -1;
}

static void switch_to(int i) {
    if (i < 0 || i >= g_n || i == g_active) return;
    usetting_set(SET_ACTIVE, g_codes[i]);
    wm_logf("layout: switched to %s\n", g_codes[i]);
    // The poll re-reads it on the generation the set moved; reading now
    // keeps the tray from showing the old code for one frame.
    read_state();
    apply_visibility();
}

// --- the switcher ---------------------------------------------------------

int layout_switch_step(int dir) {
    if (g_n < 2) return 0;
    if (!layout_open || !g_switching) {
        open_now(1);
        g_pick = g_active;
    }
    int from = g_pick >= 0 ? g_pick : 0;
    g_pick = ((from + (dir < 0 ? -1 : 1)) % g_n + g_n) % g_n;
    layout_damage();
    return 1;
}

int layout_switching(void) { return layout_open && g_switching; }

void layout_switch_commit(void) {
    int pick = g_pick;
    layout_close();
    switch_to(pick);
}

// --- input ------------------------------------------------------------------

static int row_at(const struct layout_geom *g, int mx, int my) {
    if (mx < g->x || mx >= g->x + g->w || my < g->rows_y) return -1;
    int r = (my - g->rows_y) / g->row_h;
    return r < g_n ? r : -1;
}

int layout_handle_click(int mx, int my) {
    struct layout_geom g;
    geometry(&g);
    if (!layout_open) {
        if (tray_is_hidden(g_tray_id)) return 0;
        if (uui_hit(g.tray_x, g.tray_y, g.tray_w, g.tray_h, mx, my)) {
            open_now(0);
            return 1;
        }
        return 0;
    }
    if (uui_hit(g.tray_x, g.tray_y, g.tray_w, g.tray_h, mx, my)) {
        layout_close();      // a second click on the item closes it
        return 1;
    }
    if (uui_hit(g.btn_x, g.btn_y, g.btn_w, g.btn_h, mx, my)) {
        layout_close();
        int pid = sys_spawn("/bin/wm/system/settings", SET_LIST, -1);
        if (pid > 0) wm_track_launched(pid);   // reaped by the poll
        return 1;
    }
    int r = row_at(&g, mx, my);
    if (r >= 0) {
        layout_close();
        switch_to(r);
        return 1;
    }
    if (uui_hit(g.x, g.y, g.w, g.h, mx, my)) return 1;   // inert, but ours
    layout_close();
    // A click on the TASKBAR is still the taskbar's.
    return my < screen_h - taskbar_h;
}

int layout_hover_at(int mx, int my) {
    if (!layout_open) return 0;
    struct layout_geom g;
    geometry(&g);
    int h = row_at(&g, mx, my);
    if (h < 0 && uui_hit(g.btn_x, g.btn_y, g.btn_w, g.btn_h, mx, my)) h = LAYOUT_MAX;
    if (h != g_hover) { g_hover = h; layout_damage(); }
    return uui_hit(g.x, g.y, g.w, g.h, mx, my);
}

// While open the keyboard is the flyout's: Esc closes it (and cancels a
// switch), Up/Down move, Enter takes the highlighted row.
int layout_key(int key, uint8_t mods) {
    (void)mods;
    if (!layout_open) return 0;
    if (key == 0x1B) { layout_close(); return 1; }
    int cur = g_switching ? g_pick : (g_hover >= 0 && g_hover < g_n ? g_hover : g_active);
    if (key == KEY_ARROW_UP || key == KEY_ARROW_DOWN) {
        int next = ((cur < 0 ? 0 : cur) + (key == KEY_ARROW_UP ? -1 : 1) + g_n) % g_n;
        if (g_switching) g_pick = next; else g_hover = next;
        layout_damage();
        return 1;
    }
    if ((key == '\n' || key == '\r') && cur >= 0) {
        layout_close();
        switch_to(cur);
        return 1;
    }
    return 0;
}

// --- drawing ----------------------------------------------------------------

void layout_draw(int mx, int my) {
    (void)mx; (void)my;
    struct layout_geom g;
    geometry(&g);
    wm_flyout_card(g.x, g.y, g.w, g.h, g.foot_h);
    struct wm_flyout_metrics m;
    wm_flyout_metrics(&m);
    for (int i = 0; i < g_n; i++) {
        // The radio and the accent mark the ACTIVE layout -- or, while
        // Super+Space walks, the PICK: where Super's release will land is
        // what has to read, and a hover wash was too faint for it.
        int sel = g_switching ? i == g_pick : i == g_active;
        int hot = !g_switching && i == g_hover;
        char code[16];
        unsigned k = 0;
        for (; g_codes[i][k] && k + 1 < sizeof code; k++)
            code[k] = (char)(g_codes[i][k] >= 'a' && g_codes[i][k] <= 'z' ? g_codes[i][k] - 32 : g_codes[i][k]);
        code[k] = 0;
        wm_flyout_radio_row(g.x + m.pad / 2, g.rows_y + i * g.row_h, g.w - m.pad, g.row_h,
                            sel, hot, g_names[i], code);
    }
    wm_flyout_button(g.btn_x, g.btn_y, FOOT_LABEL, 0, 0, g_hover == LAYOUT_MAX, 0);
}
