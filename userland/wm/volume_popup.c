// See volume_popup.h for what this is, why the panel owns it, and why
// the setting write is debounced.
#include "wm_internal.h"
#include "volume_popup.h"
#include "wm_tray.h"
#include "start_menu.h"
#include "context_menu.h"
#include "calendar_popup.h"
#include "ui/uui.h"
#include "ui/utheme.h"
#include "lib/icon_cache.h"
#include "kapi.h"
#include "rt/sys.h"
#include "wm/wm_conf.h"   // struct setting_msg, SETTING_OP_*

#define VOLUME_SETTING "system.volume"
#define DEVICE_SETTING "system.audio_device"

// One wheel notch, and one arrow-sized step. Five is what KDE, GNOME
// and Windows all move per notch.
#define VOLUME_STEP 5

// How long the pointer must sit still before the level is written.
// A setting write PERSISTS -- one /etc write per call -- so a dragged
// slider without this is a hundred filesystem writes and the audio
// stuttering as they land.
#define VOLUME_COMMIT_MS 250

int volume_open = 0;

static int g_tray_id = -1;
static const char *g_tray_icon;

// The popup's own view of the level. Written by the slider and the
// wheel immediately, pushed to the setting once it settles.
static int g_level = 100;
static int g_pending;                  // a write is owed
static unsigned long long g_pending_at;
static int g_premute_level = 100;      // what unmuting goes back to

// The device list, adopted from the setting's CHOICE list. Copied
// rather than re-read per frame: drawing runs on every mouse move over
// an open panel, and each row would otherwise be a syscall.
static char g_dev_value[VOLUME_MAX_DEVICES][SETTING_ABI_VALUE_MAX];
static char g_dev_label[VOLUME_MAX_DEVICES][SETTING_ABI_LABEL_MAX];
static int  g_dev_count;
static int  g_dev_selected;            // which row carries the tick

static uint32_t g_seen_generation;
static int g_dragging;                 // the slider has the pointer

// --- the settings behind it -------------------------------------------

static void msg_clear(struct setting_msg *m) {
    for (unsigned i = 0; i < sizeof *m; i++) ((uint8_t *)m)[i] = 0;
}

static int setting_get(const char *name, char *out, uint32_t out_size) {
    struct setting_msg m;
    msg_clear(&m);
    m.op = SETTING_OP_GET;
    k_strlcpy(m.name, name, sizeof m.name);
    if (sys_setting(&m) != 0) return 0;
    k_strlcpy(out, m.value, out_size);
    return 1;
}

static int setting_set(const char *name, const char *value) {
    struct setting_msg m;
    msg_clear(&m);
    m.op = SETTING_OP_SET;
    k_strlcpy(m.name, name, sizeof m.name);
    k_strlcpy(m.value, value, sizeof m.value);
    if (sys_setting(&m) != 0) return 0;
    return m.result != SETTING_INVALID;
}

static int parse_int(const char *s) {
    int v = 0;
    if (!s || !s[0]) return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (*s - '0');
    }
    return v;
}

// The device rows ARE the `audio_device` setting's choice list, which
// is why nothing here knows what a sound card is: the kernel's setting
// computes its choices from the registered drivers, so a card plugged
// in after boot turns up as a row.
static void reload_devices(void) {
    struct setting_msg m;
    int index = -1, count = 0;

    msg_clear(&m);
    m.op = SETTING_OP_COUNT;
    if (sys_setting(&m) != 0) return;
    count = (int)m.count;

    for (int i = 0; i < count; i++) {
        msg_clear(&m);
        m.op = SETTING_OP_INFO;
        m.index = i;
        if (sys_setting(&m) != 0) continue;
        // Qualified, because a bare `audio_device` is only unique until
        // something else registers one.
        char qualified[SETTING_ABI_QUALIFIED_MAX];
        k_snprintf(qualified, sizeof qualified, "%s.%s", m.ns, m.name);
        if (k_strcmp(qualified, DEVICE_SETTING) == 0) { index = i; break; }
    }
    g_dev_count = 0;
    if (index < 0) return;

    char current[SETTING_ABI_VALUE_MAX] = "auto";
    setting_get(DEVICE_SETTING, current, sizeof current);

    for (int c = 0; c < VOLUME_MAX_DEVICES; c++) {
        msg_clear(&m);
        m.op = SETTING_OP_CHOICE;
        m.index = index;
        m.choice = c;
        if (sys_setting(&m) != 0) break;
        k_strlcpy(g_dev_value[g_dev_count], m.value, SETTING_ABI_VALUE_MAX);
        k_strlcpy(g_dev_label[g_dev_count], m.label, SETTING_ABI_LABEL_MAX);
        if (k_strcmp(m.value, current) == 0) g_dev_selected = g_dev_count;
        g_dev_count++;
    }
    if (g_dev_selected >= g_dev_count) g_dev_selected = 0;
}

static void reload_level(void) {
    char buf[SETTING_ABI_VALUE_MAX];
    if (!setting_get(VOLUME_SETTING, buf, sizeof buf)) return;
    int v = parse_int(buf);
    if (v >= 0 && v <= 100) {
        g_level = v;
        if (v > 0) g_premute_level = v;
    }
}

// --- the tray item ----------------------------------------------------

static const char *icon_for(int level) {
    if (level <= 0) return "tray-volume-muted";
    if (level < 50) return "tray-volume-low";
    return "tray-volume-high";
}

void volume_tray_init(void) {
    reload_level();
    reload_devices();
    g_tray_icon = icon_for(g_level);
    g_tray_id = tray_register_icon(g_tray_icon);
}

void volume_tray_update(void) {
    if (g_tray_id < 0) return;
    const char *want = icon_for(g_level);
    if (want == g_tray_icon) return;
    g_tray_icon = want;
    tray_set_icon(g_tray_id, want);
}

// --- geometry ---------------------------------------------------------

void volume_geometry(struct volume_geom *g) {
    for (unsigned i = 0; i < sizeof *g; i++) ((uint8_t *)g)[i] = 0;

    int ch = ugfx_char_h();
    int pad = ch / 2 + 2;
    int row_h = ch + 8;
    // Wide enough for the widest device label rather than a constant:
    // "Automatic (usb-audio)" is longer than anything else here, and a
    // panel sized for the shorter case clips it.
    int w = ugfx_text_width("Automatic (usb-audio)") + pad * 4;
    int min_w = ugfx_char_w() * 22;
    if (w < min_w) w = min_w;

    int rows = g_dev_count;
    int h = pad + row_h                 // the level row
            + pad / 2 + 1 + pad / 2     // the rule
            + ch + pad / 2              // "Output device"
            + rows * row_h + pad;

    int tx = 0, ty = 0, tw = 0, th = 0;
    if (!tray_item_rect(g_tray_id, &tx, &ty, &tw, &th)) {
        tx = screen_w - 32; ty = screen_h - taskbar_h; tw = 16; th = taskbar_h;
    }
    // Right-aligned to the tray item and clamped on-screen, which is
    // what the calendar does -- a panel wider than its anchor otherwise
    // hangs off the edge on the one screen nobody tested.
    int x = tx + tw - w;
    if (x + w > screen_w - 8) x = screen_w - 8 - w;
    if (x < 4) x = 4;
    int y = screen_h - taskbar_h - h - 4;
    if (y < 4) y = 4;

    g->x = x; g->y = y; g->w = w; g->h = h;
    g->tray_x = tx; g->tray_y = ty; g->tray_w = tw; g->tray_h = th;

    g->mute_x = x + pad;
    g->mute_y = y + pad;
    g->mute_w = row_h;
    g->mute_h = row_h;

    g->slider_x = g->mute_x + g->mute_w + pad;
    g->slider_y = y + pad + (row_h - ch) / 2;
    g->slider_h = ch;
    // The reading sits at the right end, so the track stops short of it.
    g->slider_w = x + w - pad - ugfx_text_width("100%") - pad - g->slider_x;

    g->list_x = x + pad;
    g->list_y = y + pad + row_h + pad + 1 + ch + pad / 2;
    g->row_h = row_h;
    g->rows = rows;
    g->level = g_level;
    g->muted = (g_level == 0);
    g->selected_row = g_dev_selected;
}

int volume_row(int index, char *value, uint32_t value_size,
               char *label, uint32_t label_size) {
    if (index < 0 || index >= g_dev_count) return 0;
    if (value) k_strlcpy(value, g_dev_value[index], value_size);
    if (label) k_strlcpy(label, g_dev_label[index], label_size);
    return 1;
}

// --- state ------------------------------------------------------------

void volume_open_now(void) {
    if (start_menu_open) { start_menu_open = 0; start_menu_damage(); }
    calendar_close();
    context_menu_close();
    reload_level();
    reload_devices();
    volume_open = 1;
    redraw_pending = 1;
}

void volume_close(void) {
    if (!volume_open) return;
    volume_open = 0;
    g_dragging = 0;
    redraw_pending = 1;
}

// Applies a level to the popup's own state. The SETTING is written by
// volume_poll_config() once the pointer settles -- see the header.
static void set_level(int pct, int commit_now) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (pct == g_level && !commit_now) return;
    if (pct > 0) g_premute_level = pct;
    g_level = pct;
    g_pending = 1;
    g_pending_at = sys_monotonic_ns();
    if (commit_now) g_pending_at = 0;
    volume_tray_update();
    redraw_pending = 1;
}

void volume_poll_config(void) {
    if (g_pending) {
        unsigned long long now = sys_monotonic_ns();
        if (g_pending_at == 0 ||
            now - g_pending_at >= (unsigned long long)VOLUME_COMMIT_MS * 1000000ull) {
            char buf[16];
            k_snprintf(buf, sizeof buf, "%d", g_level);
            setting_set(VOLUME_SETTING, buf);
            g_pending = 0;
            // Our own write bumps the generation; adopting it here stops
            // the reload below from immediately re-reading what we just
            // wrote.
            g_seen_generation = wm_setting_generation();
        }
        return;
    }
    uint32_t gen = wm_setting_generation();
    if (gen == g_seen_generation) return;
    g_seen_generation = gen;
    // Something else changed a setting -- System Settings, `config set`,
    // or a sound card appearing. Both halves are re-read: the device
    // list because a card may have arrived, the level because the
    // Settings app has its own slider for it.
    reload_level();
    reload_devices();
    volume_tray_update();
    if (volume_open) redraw_pending = 1;
}

// --- input ------------------------------------------------------------

static int level_from_x(const struct volume_geom *g, int mx) {
    if (g->slider_w <= 0) return g_level;
    int rel = mx - g->slider_x;
    if (rel < 0) rel = 0;
    if (rel > g->slider_w) rel = g->slider_w;
    return rel * 100 / g->slider_w;
}

int volume_handle_click(int mx, int my) {
    struct volume_geom g;
    volume_geometry(&g);

    if (!volume_open) {
        if (uui_hit(g.tray_x, g.tray_y, g.tray_w, g.tray_h, mx, my)) {
            volume_open_now();
            return 1;
        }
        return 0;
    }

    if (!uui_hit(g.x, g.y, g.w, g.h, mx, my)) {
        volume_close();
        // A dismissing click on the TASKBAR falls through, so the Start
        // button acts on the same click that closed this -- and so a
        // second click on our own tray item closes rather than
        // reopening. Anything else is swallowed, as for any open menu.
        return my < screen_h - taskbar_h;
    }

    if (uui_hit(g.mute_x, g.mute_y, g.mute_w, g.mute_h, mx, my)) {
        // MUTE IS A LEVEL OF ZERO, not a separate flag: the registry
        // holds one number and a second piece of state would have to
        // persist beside it. The pre-mute level is remembered here, so
        // unmuting returns to it within this session -- a reboot while
        // muted comes back at zero, which is the honest cost.
        set_level(g_level == 0 ? (g_premute_level ? g_premute_level : VOLUME_STEP)
                               : 0, 1);
        return 1;
    }
    if (uui_hit(g.slider_x - 4, g.y, g.slider_w + 8, g.mute_h + 8, mx, my)) {
        g_dragging = 1;
        set_level(level_from_x(&g, mx), 0);
        return 1;
    }
    for (int i = 0; i < g.rows; i++) {
        if (!uui_hit(g.list_x, g.list_y + i * g.row_h, g.w - (g.list_x - g.x) * 2,
                     g.row_h, mx, my))
            continue;
        if (setting_set(DEVICE_SETTING, g_dev_value[i])) {
            g_dev_selected = i;
            // The labels move with the choice: "Automatic (ac97)" names
            // what auto resolved to, and that changes when the pick does.
            reload_devices();
            redraw_pending = 1;
        }
        return 1;
    }
    return 1;   // a click inside the panel never falls through
}

int volume_update_press(int mx, int my, uint8_t buttons) {
    (void)my;
    if (!g_dragging) return 0;
    if (!(buttons & 0x1)) {
        g_dragging = 0;
        // Committed here rather than on the next timer: releasing the
        // slider is the moment the user means, and waiting out the
        // debounce for it would be a quarter second of the old volume.
        if (g_pending) g_pending_at = 0;
        return 0;
    }
    struct volume_geom g;
    volume_geometry(&g);
    int want = level_from_x(&g, mx);
    if (want == g_level) return 0;
    set_level(want, 0);
    return 1;
}

int volume_handle_wheel(int mx, int my, int notches) {
    if (!notches) return 0;
    struct volume_geom g;
    volume_geometry(&g);
    int over = uui_hit(g.tray_x, g.tray_y, g.tray_w, g.tray_h, mx, my) ||
               (volume_open && uui_hit(g.x, g.y, g.w, g.h, mx, my));
    if (!over) return 0;
    set_level(g_level + notches * VOLUME_STEP, 0);
    return 1;
}

// --- drawing ----------------------------------------------------------

void volume_draw(int mx, int my) {
    if (!volume_open) return;

    struct volume_geom g;
    volume_geometry(&g);

    uint32_t bg = UTHEME_PANEL_BG, border = UTHEME_BORDER, fg = UTHEME_TEXT;
    uint32_t accent = UTHEME_ACCENT;
    // Derived from the panel's own colour, never hand-picked: on this
    // near-white theme "hover" has to DARKEN, and uui_state_bg() is what
    // decides that from gfx_luminance() (docs/gui-guidelines.md).
    uint32_t hover_bg = uui_state_bg(bg, UUI_STATE_HOVER);

    ugfx_fill_rect(wm_surface(), g.x, g.y, g.w, g.h, bg);

    // --- the mute button ---------------------------------------------
    int over_mute = uui_hit(g.mute_x, g.mute_y, g.mute_w, g.mute_h, mx, my);
    if (over_mute)
        ugfx_fill_rect(wm_surface(), g.mute_x, g.mute_y, g.mute_w, g.mute_h,
                       hover_bg);
    {
        const struct uimg *ico = icon_get(icon_for(g.level), g.mute_w - 6);
        if (ico)
            ugfx_blit_alpha(wm_surface(), g.mute_x + (g.mute_w - ico->w) / 2,
                            g.mute_y + (g.mute_h - ico->h) / 2,
                            ico->w, ico->h, ico->px, ico->w);
    }

    // --- the level ----------------------------------------------------
    int track_y = g.slider_y + g.slider_h / 2 - 2;
    ugfx_fill_rect(wm_surface(), g.slider_x, track_y, g.slider_w, 4, border);
    int filled = g.slider_w * g.level / 100;
    ugfx_fill_rect(wm_surface(), g.slider_x, track_y, filled, 4, accent);
    // The knob is the accent too, and drawn LAST so it sits over the
    // track it marks the end of.
    ugfx_fill_rect(wm_surface(), g.slider_x + filled - 2, g.slider_y - 2,
                   5, g.slider_h + 4, accent);

    char pct[8];
    k_snprintf(pct, sizeof pct, "%d%%", g.level);
    ugfx_draw_string_clipped(wm_surface(), g.x + g.w - ugfx_text_width("100%") - 8,
                             g.slider_y, ugfx_text_width("100%") + 4, pct, fg, bg);

    // --- the rule and the heading -------------------------------------
    int rule_y = g.y + (g.mute_y - g.y) + g.mute_h + 4;
    ugfx_fill_rect(wm_surface(), g.x + 8, rule_y, g.w - 16, 1, border);
    ugfx_draw_string_clipped(wm_surface(), g.list_x, rule_y + 6,
                             g.w - 16, "Output device", border, bg);

    // --- the devices ---------------------------------------------------
    for (int i = 0; i < g.rows; i++) {
        int ry = g.list_y + i * g.row_h;
        int rw = g.w - (g.list_x - g.x) * 2;
        int over = uui_hit(g.list_x, ry, rw, g.row_h, mx, my);
        uint32_t row_bg = bg, row_fg = fg;
        if (i == g.selected_row) { row_bg = accent; row_fg = UTHEME_ACCENT_TEXT; }
        else if (over)           { row_bg = hover_bg; }
        ugfx_fill_rect(wm_surface(), g.list_x, ry, rw, g.row_h, row_bg);
        ugfx_draw_string_clipped(wm_surface(), g.list_x + 6,
                                 ry + (g.row_h - ugfx_char_h()) / 2,
                                 rw - 12, g_dev_label[i], row_fg, row_bg);
    }

    // The border last, so nothing above has painted over it.
    ugfx_draw_rect(wm_surface(), g.x, g.y, g.w, g.h, border);
}
