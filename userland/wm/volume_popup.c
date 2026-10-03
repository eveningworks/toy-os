// See volume_popup.h for what this is and why the panel owns it. The
// slider row, the debounced write and the overlay verbs are
// tray_slider_popup.c's; what is here is only the volume's own -- mute,
// the device rows, and the speaker icon that follows the level.
#include "wm_internal.h"
#include "wm_overlay.h"
#include "volume_popup.h"
#include "tray_slider_popup.h"
#include "wm_shadow.h"   // the damage a shadowed panel actually needs
#include "wm_flyout.h"
#include "wm_tray.h"
#include "ui/uui.h"
#include "ui/utheme.h"
#include "kapi.h"
#include "rt/sys.h"
#include "lib/usetting.h"
#include "lib/uconf.h"       // /etc/sound.conf, over the shared parser
#include "ui/uui_scale.h"
#include "sound_abi.h"       // the daemon's roster, in its beacon
#include "syscall_abi.h"

#define VOLUME_SETTING "system.volume"
#define DEVICE_SETTING "system.audio_device"

// One wheel notch, and one arrow-sized step. Five is what KDE, GNOME
// and Windows all move per notch.
#define VOLUME_STEP 5

int volume_open = 0;

static struct tray_slider_popup g_popup;
static const char *g_tray_icon;
static int g_premute_level = 100;      // what unmuting goes back to

// The device list, adopted from the setting's CHOICE list. Copied
// rather than re-read per frame: drawing runs on every mouse move over
// an open panel, and each row would otherwise be a syscall.
static char g_dev_value[VOLUME_MAX_DEVICES][SETTING_ABI_VALUE_MAX];
static char g_dev_label[VOLUME_MAX_DEVICES][SETTING_ABI_LABEL_MAX];
static int  g_dev_count;
static int  g_dev_selected;            // which row carries the tick

// The overlay registry's hover token: the shared row's, then the
// per-app rows, then a device row. Compared by the core, read by the
// draw -- see wm_overlay.h.
#define HOVER_APP(i) (TRAY_SLIDER_HOVER_OWNER + (i))
#define HOVER_DEV(i) (TRAY_SLIDER_HOVER_OWNER + SND_ROSTER_MAX + (i))
#define HOVER_MUTEALL (TRAY_SLIDER_HOVER_OWNER + SND_ROSTER_MAX + VOLUME_MAX_DEVICES)
#define HOVER_GEAR    (HOVER_MUTEALL + 1)
static int g_hover;

// --- the per-application sliders ---------------------------------------
//
// WHAT IS BEING MIXED comes from soundd's BEACON, which carries a
// roster (abi/sound_abi.h). The daemon is the only writer and this maps
// it READ-ONLY -- the alternative, opening each client's own ring to
// read its name, would make the panel a reader of every app's audio
// buffer for the sake of a label.
//
// THE GAIN IS NOT A SETTING, so none of tray_slider_popup.c's machinery
// applies to these rows: the registry is a fixed catalogue of
// build-time knobs with a System Settings row each, and these keys
// appear one per program ever played. They live in /etc/sound.conf and
// are written here with uconf_set().
static volatile struct snd_roster *g_roster_page;
static struct snd_roster g_roster;              // a SETTLED copy
static struct uui_scale g_app_scale[SND_ROSTER_MAX];
static int g_app_count;

// Debounced exactly as the master level is, and for the same reason: a
// write rewrites the whole document, so a dragged slider without the
// delay is a hundred filesystem writes.
static int g_app_pending = -1;                  // which row owes a write
static unsigned long long g_app_pending_at;
static int g_app_drag = -1;                     // which row the press is on

// A TORN READ IS TOLERATED, NOT LOCKED OUT. `gen` is bumped either side
// of the daemon's rewrite, so an odd value or a changed one means "look
// again"; three tries, then keep the previous copy. A slider one frame
// stale is not worth a lock in a page a dying daemon can leave behind.
static void reload_roster(void) {
    if (!g_roster_page) {
        int fd = sys_shm_open(SND_SERVER_NAME, 0, 0);
        if (fd < 0) { g_app_count = 0; return; }   // no daemon: no rows
        void *p = sys_mmap(0, 4096, SYS_PROT_READ, SYS_MAP_SHARED, fd, 0);
        sys_close(fd);
        if (p == (void *)-1) { g_app_count = 0; return; }
        g_roster_page = p;
    }
    for (int try = 0; try < 3; try++) {
        uint32_t a = g_roster_page->gen;
        if (a & 1) continue;
        struct snd_roster tmp;
        k_memcpy(&tmp, (const void *)g_roster_page, sizeof tmp);
        if (g_roster_page->gen != a) continue;
        if (tmp.magic != SND_CTL_MAGIC) { g_app_count = 0; return; }
        g_roster = tmp;
        g_app_count = (int)(tmp.count > SND_ROSTER_MAX ? SND_ROSTER_MAX : tmp.count);
        return;
    }
}

int volume_app_row(int index, char *app, uint32_t app_size, int *gain) {
    if (index < 0 || index >= g_app_count) return 0;
    if (app) k_strlcpy(app, (const char *)g_roster.e[index].app, app_size);
    if (gain) *gain = (int)g_roster.e[index].gain;
    return 1;
}

// The label a row carries. An UNNAMED client is shown by pid rather
// than hidden: it is audible, so a mixer that omitted it would be
// lying about what is playing -- and it is the one row whose slider
// cannot be remembered, because "" is every unnamed client's key.
static const char *app_label(int i, char *buf, uint32_t size) {
    if (g_roster.e[i].app[0]) return (const char *)g_roster.e[i].app;
    k_snprintf(buf, size, "pid %d", (int)g_roster.e[i].pid);
    return buf;
}

// The roster, and a scale per row to match it. Damages on a COUNT
// change, because that resizes the panel.
//
// A HELD ROW (dragged, or owing a write) IS FOLLOWED BY PID AND NAME,
// not index: soundd compacts the roster, so a client ahead of it going
// away shifts it down one -- and the write would then land on whichever
// app moved into the old index.
static int relocate(int i, const struct snd_roster_entry *was) {
    if (i < 0) return -1;
    for (int j = 0; j < g_app_count; j++)
        if (g_roster.e[j].pid == was->pid &&
            k_strcmp((const char *)g_roster.e[j].app, (const char *)was->app) == 0)
            return j;
    return -1;   // it left: nothing to write to
}

static void refresh_apps(void) {
    int before = g_app_count;
    struct snd_roster_entry drag_was = {0}, pend_was = {0};
    struct uui_scale drag_sc = {0}, pend_sc = {0};
    if (g_app_drag >= 0) { drag_was = g_roster.e[g_app_drag]; drag_sc = g_app_scale[g_app_drag]; }
    if (g_app_pending >= 0) { pend_was = g_roster.e[g_app_pending]; pend_sc = g_app_scale[g_app_pending]; }
    reload_roster();
    g_app_drag = relocate(g_app_drag, &drag_was);
    g_app_pending = relocate(g_app_pending, &pend_was);
    for (int i = 0; i < g_app_count; i++) {
        if (i == g_app_drag || i == g_app_pending) continue;
        uui_scale_init(&g_app_scale[i], 0, 100, (long)g_roster.e[i].gain);
    }
    if (g_app_pending >= 0) g_app_scale[g_app_pending] = pend_sc;
    if (g_app_drag >= 0) g_app_scale[g_app_drag] = drag_sc;
    if (before != g_app_count) volume_damage();
}

// THE CORE CALLS THIS the moment the panel opens, before its first
// frame (wm_overlay.h's on_open). The roster is not refreshed while the
// panel is closed -- it is a shared-memory read per frame and nothing
// draws it -- so without this the first frame shows whatever was
// playing when it last closed.
void volume_opened(void) { refresh_apps(); }

static void commit_app(int i) {
    if (i < 0 || i >= g_app_count) return;
    if (!g_roster.e[i].app[0]) return;   // nothing stable to key on
    char v[8];
    k_snprintf(v, sizeof v, "%ld", uui_scale_value(&g_app_scale[i]));
    uconf_set(SND_CONFIG_FILE, (const char *)g_roster.e[i].app, v);
}

// --- the settings behind it -------------------------------------------

// The device rows ARE the `audio_device` setting's choice list, which
// is why nothing here knows what a sound card is: the kernel's setting
// computes its choices from the registered drivers, so a card plugged
// in after boot turns up as a row.
//
// **THAT ONLY BECAME TRUE WHEN THE PLUG BUMPED THE GENERATION.** This
// runs on a setting-generation change, and the generation was bumped
// by a SET -- which registering a device is not -- so an unplugged DAC
// stayed in the list until something else wrote a setting.
// `sound_register`/`sound_unregister` call `setting_choices_changed()`
// now (api/setting.h).
static void reload_devices(void) {
    struct setting_msg m;
    int index = usetting_find(DEVICE_SETTING, &m);
    g_dev_count = 0;
    if (index < 0) return;

    char current[SETTING_ABI_VALUE_MAX];
    if (!usetting_get(DEVICE_SETTING, current, sizeof current))
        k_strlcpy(current, "auto", sizeof current);

    for (int c = 0; c < VOLUME_MAX_DEVICES; c++) {
        k_memset(&m, 0, sizeof m);
        m.op = SETTING_OP_CHOICE;
        m.index = index;
        m.choice = c;
        // **THE INDEX CAME FROM usetting_find(), SO THE CHOICE MUST
        // GO BACK THROUGH THE SAME REGISTRY.** They agree today only
        // because the merged list puts the kernel's settings first and
        // this is one of them; a declared setting's index means nothing
        // to the syscall.
        if (usetting_dispatch(&m) != 0) break;
        k_strlcpy(g_dev_value[g_dev_count], m.value, SETTING_ABI_VALUE_MAX);
        k_strlcpy(g_dev_label[g_dev_count], m.label, SETTING_ABI_LABEL_MAX);
        if (k_strcmp(m.value, current) == 0) g_dev_selected = g_dev_count;
        g_dev_count++;
    }
    if (g_dev_selected >= g_dev_count) g_dev_selected = 0;
}

// --- the tray item ----------------------------------------------------

static const char *icon_for(int level) {
    if (level <= 0) return "tray-volume-muted";
    if (level < 50) return "tray-volume-low";
    return "tray-volume-high";
}

void volume_tray_update(void) {
    if (g_popup.tray_id < 0) return;
    const char *want = icon_for(g_popup.level);
    if (want == g_tray_icon) return;
    g_tray_icon = want;
    tray_set_icon(g_popup.tray_id, want);
}

// After any change of level: remember what unmuting returns to, and
// let the speaker icon follow.
static void on_level(void) {
    if (g_popup.level > 0) g_premute_level = g_popup.level;
    volume_tray_update();
}

// THE HEADER NAMES WHAT IS PLAYING THROUGH: the card "Automatic"
// resolved to, else the one chosen. Its label is the choice list's, so
// nothing here knows what a sound card is.
static void hero(char *title, unsigned tcap, char *sub, unsigned scap) {
    int real = 0;
    for (int i = 0; i < g_dev_count; i++) if (k_strcmp(g_dev_value[i], "auto")) real++;
    if (!real) {
        k_strlcpy(title, "No sound device", tcap);
        k_strlcpy(sub, "Nothing plays until one is plugged in", scap);
        return;
    }
    const char *chosen = g_dev_value[g_dev_selected];
    if (k_strcmp(chosen, "auto")) {
        k_strlcpy(title, g_dev_label[g_dev_selected], tcap);
        k_snprintf(sub, scap, "Chosen output, %s", chosen);
        return;
    }
    // "Automatic (hda1)": the value in the brackets names the row.
    char want[SETTING_ABI_VALUE_MAX] = "";
    const char *open = g_dev_label[g_dev_selected];
    while (*open && *open != '(') open++;
    if (*open) {
        k_strlcpy(want, open + 1, sizeof want);
        for (char *c = want; *c; c++) if (*c == ')') { *c = 0; break; }
    }
    k_strlcpy(title, g_dev_label[g_dev_selected], tcap);
    for (int i = 0; i < g_dev_count; i++)
        if (want[0] && !k_strcmp(g_dev_value[i], want)) k_strlcpy(title, g_dev_label[i], tcap);
    k_snprintf(sub, scap, "Automatic%s%s", want[0] ? ", " : "", want);
}

void volume_tray_init(void) {
    g_popup.name = "volume";
    g_popup.hero = hero;
    g_popup.setting = VOLUME_SETTING;
    g_popup.step = VOLUME_STEP;
    g_popup.on_level = on_level;
    g_popup.on_reload = reload_devices;
    g_popup.damage = volume_damage;
    reload_devices();
    g_tray_icon = icon_for(100);
    tray_slider_init(&g_popup, g_tray_icon);
    volume_tray_update();   // the registered icon follows the level read
}

// --- geometry ---------------------------------------------------------

// The one geometry: the shared row from tray_slider_geometry(), sized
// for the device rows below it, and the rows themselves. `g` may be
// NULL when only the row is wanted.
static void geometry(struct tray_slider_geom *s, struct volume_geom *g) {
    struct wm_flyout_metrics m;
    wm_flyout_metrics(&m);
    int ch = ugfx_char_h();
    int rows = g_dev_count;
    int apps = g_app_count;
    int app_row_h = ch + 12;   // taller than a device row: it holds a track
    int dev_row_h = ch + 12;
    // Wide enough for the widest device label and its note rather than
    // a constant: "Automatic (usb-audio)" is longer than anything else
    // here, and a panel sized for the shorter case clips it.
    int want_w = ugfx_text_width("Automatic (usb-audio)") + ugfx_text_width("usb-audio") + 4 * m.pad;
    // The apps section only exists when something is playing, rule and
    // caption included -- an empty "Applications" box would be a control
    // that draws and says nothing.
    int extra_h = 1 + m.vpad + m.cap_h + rows * dev_row_h + m.vpad;
    if (apps) extra_h += 1 + m.vpad + m.cap_h + apps * app_row_h + m.vpad;
    tray_slider_geometry(&g_popup, want_w, extra_h, m.foot_h, s);
    if (!g) return;

    k_memset(g, 0, sizeof *g);
    g->x = s->x; g->y = s->y; g->w = s->w; g->h = s->h;
    g->tray_x = s->tray_x; g->tray_y = s->tray_y; g->tray_w = s->tray_w; g->tray_h = s->tray_h;
    g->mute_x = s->icon_x; g->mute_y = s->icon_y; g->mute_w = s->icon_w; g->mute_h = s->icon_h;
    g->slider_x = s->slider_x; g->slider_y = s->slider_y;
    g->slider_w = s->slider_w; g->slider_h = s->slider_h;
    g->app_x = s->x + m.pad;
    g->list_x = s->x + m.pad - 6;
    g->apps = apps;
    g->app_row_h = app_row_h;

    int y = s->below_y;
    if (apps) {
        g->app_rule_y = y;
        g->app_cap_y = y + 1 + m.vpad;
        g->app_y = g->app_cap_y + m.cap_h;
        y = g->app_y + apps * app_row_h + m.vpad;
    }
    g->list_rule_y = y;
    g->list_cap_y = y + 1 + m.vpad;
    g->list_y = g->list_cap_y + m.cap_h;
    g->row_h = dev_row_h;
    g->rows = rows;

    // The track sits right of the widest label this panel will draw, so
    // every row's slider starts at the same x -- a ragged left edge on
    // a column of sliders reads as a layout fault.
    int label_w = ugfx_text_width("MMMMMMMM") + m.pad / 2;
    g->app_slider_x = g->app_x + label_w;
    g->app_slider_w = (s->x + s->w - m.pad) - g->app_slider_x
                      - ugfx_text_width("100%") - m.pad / 2;
    for (int i = 0; i < apps; i++) {
        if (g->app_slider_w > 0)
            uui_scale_set_geometry(&g_app_scale[i], g->app_slider_x,
                                   g->app_y + i * app_row_h + (app_row_h - ch) / 2,
                                   g->app_slider_w, ch);
    }

    g->foot_y = s->foot_y;
    g->foot_h = s->foot_h;
    g->btn_h = m.btn_h;
    g->btn_y = g->foot_y + (g->foot_h - m.btn_h) / 2;
    g->muteall_x = s->x + m.pad - 6;
    g->muteall_w = wm_flyout_button_w("Mute all", "tray-volume-muted");
    g->gear_w = m.btn_h;
    g->gear_x = s->x + s->w - (m.pad - 6) - g->gear_w;

    g->level = g_popup.level;
    g->muted = (g_popup.level == 0);
    g->selected_row = g_dev_selected;
}

void volume_geometry(struct volume_geom *g) {
    struct tray_slider_geom s;
    geometry(&s, g);
}

static void slider_geom(struct tray_slider_geom *s) { geometry(s, 0); }

int volume_row(int index, char *value, uint32_t value_size,
               char *label, uint32_t label_size) {
    if (index < 0 || index >= g_dev_count) return 0;
    if (value) k_strlcpy(value, g_dev_value[index], value_size);
    if (label) k_strlcpy(label, g_dev_label[index], label_size);
    return 1;
}

// --- state ------------------------------------------------------------

int volume_rect(int *x, int *y, int *w, int *h) {
    struct volume_geom g;
    volume_geometry(&g);
    *x = g.x; *y = g.y; *w = g.w; *h = g.h;
    return 1;
}

// THE PANEL CHANGES SIZE -- it grows a row per audio stream, and it is
// anchored above the taskbar, so gaining one moves its TOP UP. Covering
// the rect it has left is the core's job now (wm_overlay.h), which is
// what makes that true of every overlay rather than of this one.
void volume_damage(void) { wm_overlay_damage("volume"); }

static int row_at(const struct volume_geom *g, int mx, int my) {
    for (int i = 0; i < g->rows; i++)
        if (uui_hit(g->list_x, g->list_y + i * g->row_h,
                    g->w - (g->list_x - g->x) * 2, g->row_h - 2, mx, my))
            return i;
    return -1;
}

int volume_hover_at(int mx, int my) {
    struct tray_slider_geom s;
    slider_geom(&s);
    g_hover = tray_slider_hover_at(&g_popup, &s, mx, my);
    if (g_hover == TRAY_SLIDER_HOVER_NONE && g_popup.open) {
        struct volume_geom g;
        volume_geometry(&g);
        // THE WHOLE ROW, not just the track: a slider whose row
        // highlights only over eight pixels of thumb reads as dead.
        for (int i = 0; i < g.apps; i++)
            if (uui_hit(g.app_x, g.app_y + i * g.app_row_h,
                        g.w - (g.app_x - g.x) * 2, g.app_row_h, mx, my))
                return (g_hover = HOVER_APP(i));
        int row = row_at(&g, mx, my);
        if (row >= 0) g_hover = HOVER_DEV(row);
        if (uui_hit(g.muteall_x, g.btn_y, g.muteall_w, g.btn_h, mx, my)) g_hover = HOVER_MUTEALL;
        if (uui_hit(g.gear_x, g.btn_y, g.gear_w, g.btn_h, mx, my)) g_hover = HOVER_GEAR;
    }
    return g_hover;
}

void volume_open_now(void) {
    tray_slider_open(&g_popup);
    volume_open = g_popup.open;
    g_hover = TRAY_SLIDER_HOVER_NONE;
}

void volume_close(void) {
    tray_slider_close(&g_popup);
    volume_open = g_popup.open;
    g_hover = TRAY_SLIDER_HOVER_NONE;
}

void volume_poll_config(void) {
    tray_slider_poll(&g_popup);

    // The roster only while the panel is OPEN: it is a shared-memory
    // read per frame, and nothing draws it otherwise.
    if (g_popup.open) refresh_apps();

    if (g_app_pending >= 0 &&
        sys_monotonic_ns() / 1000000ull - g_app_pending_at >= TRAY_SLIDER_COMMIT_MS &&
        g_app_drag < 0) {
        commit_app(g_app_pending);
        g_app_pending = -1;
    }
}

// --- input ------------------------------------------------------------

int volume_handle_click(int mx, int my) {
    struct tray_slider_geom s;
    slider_geom(&s);
    enum tray_slider_click what = tray_slider_click(&g_popup, &s, mx, my);
    volume_open = g_popup.open;

    switch (what) {
    case TRAY_SLIDER_CLICK_NONE:
        return 0;
    case TRAY_SLIDER_CLICK_DISMISSED:
        // A dismissing click on the TASKBAR falls through, so the Start
        // button acts on the same click that closed this -- and so a
        // second click on our own tray item closes rather than
        // reopening. Anything else is swallowed, as for any open menu.
        return my < screen_h - taskbar_h;
    case TRAY_SLIDER_CLICK_ICON:
        // MUTE IS A LEVEL OF ZERO, not a separate flag: the registry
        // holds one number and a second piece of state would have to
        // persist beside it. The pre-mute level is remembered here, so
        // unmuting returns to it within this session -- a reboot while
        // muted comes back at zero, which is the honest cost.
        tray_slider_set_level(&g_popup,
                              g_popup.level == 0 ? (g_premute_level ? g_premute_level : VOLUME_STEP)
                                                 : 0, 1);
        return 1;
    case TRAY_SLIDER_CLICK_INSIDE: {
        struct volume_geom g;
        volume_geometry(&g);
        if (uui_hit(g.muteall_x, g.btn_y, g.muteall_w, g.btn_h, mx, my)) {
            tray_slider_set_level(&g_popup,
                                  g_popup.level == 0 ? (g_premute_level ? g_premute_level : VOLUME_STEP)
                                                     : 0, 1);
            return 1;
        }
        if (uui_hit(g.gear_x, g.btn_y, g.gear_w, g.btn_h, mx, my)) {
            volume_close();
            sys_spawn("/bin/wm/system/settings", VOLUME_SETTING, -1);
            return 1;
        }
        // A PRESS ARMS THE DRAG, and the value follows the pointer from
        // volume_update_press() -- on_click fires on button-DOWN, so a
        // control that committed here could never be cancelled
        // (docs/gui-guidelines.md).
        for (int a = 0; a < g.apps; a++) {
            if (!uui_scale_hit(&g_app_scale[a], mx, my)) continue;
            g_app_drag = a;
            uui_scale_set_value(&g_app_scale[a],
                                uui_scale_value_at(&g_app_scale[a], mx));
            g_roster.e[a].gain = (uint32_t)uui_scale_value(&g_app_scale[a]);
            g_app_pending = a;
            g_app_pending_at = sys_monotonic_ns() / 1000000ull;
            volume_damage();
            return 1;
        }
        int i = row_at(&g, mx, my);
        if (i >= 0 && usetting_set(DEVICE_SETTING, g_dev_value[i]) > 0) {
            g_dev_selected = i;
            // The labels move with the choice: "Automatic (ac97)" names
            // what auto resolved to, and that changes when the pick does
            // -- so the panel's WIDTH moves with it, and this has to
            // damage rather than merely repaint.
            reload_devices();
            volume_damage();
        }
        return 1;   // a click inside the panel never falls through
    }
    default:
        return 1;
    }
}

void volume_update_press(int mx, int my, uint8_t buttons) {
    (void)my;
    // A PER-APP DRAG FIRST, and it ends on RELEASE rather than on the
    // pointer leaving the track: a slider that let go the moment the
    // cursor slipped a pixel off the row is the complaint every
    // hand-rolled drag earns.
    if (g_app_drag >= 0) {
        struct volume_geom g;
        volume_geometry(&g);          // places the scales before reading
        if (g_app_drag < g.apps) {
            uui_scale_set_value(&g_app_scale[g_app_drag],
                                uui_scale_value_at(&g_app_scale[g_app_drag], mx));
            g_roster.e[g_app_drag].gain =
                (uint32_t)uui_scale_value(&g_app_scale[g_app_drag]);
            g_app_pending = g_app_drag;
            g_app_pending_at = sys_monotonic_ns() / 1000000ull;
            volume_damage();
        }
        if (!(buttons & 1)) {
            // COMMITTED AT ONCE ON RELEASE, as the master row is: the
            // debounce exists for the drag, not for the last value.
            commit_app(g_app_drag);
            g_app_pending = -1;
            g_app_drag = -1;
        }
        return;
    }
    if (!g_popup.scale.dragging) return;
    struct tray_slider_geom s;
    slider_geom(&s);   // places the scale before the drag reads it
    tray_slider_update_press(&g_popup, mx, buttons);
}

int volume_handle_wheel(int mx, int my, int notches) {
    struct tray_slider_geom s;
    slider_geom(&s);
    return tray_slider_wheel(&g_popup, &s, mx, my, notches);
}

// --- drawing ----------------------------------------------------------

void volume_draw(int mx, int my) {
    if (!volume_open) return;
    // The TRACKED hover, not one derived from (mx, my): only that one
    // has damaged the panel when it changed (wm_overlay.h).
    (void)mx; (void)my;

    struct tray_slider_geom s;
    struct volume_geom g;
    geometry(&s, &g);
    int ch = ugfx_char_h();
    uint32_t ink = wm_flyout_ink();
    uint32_t hover_bg = uui_state_bg(wm_flyout_ground(), UUI_STATE_HOVER);

    tray_slider_draw(&g_popup, &s, icon_for(g.level), g_hover == TRAY_SLIDER_HOVER_ICON);

    // --- the applications, when any are playing ------------------------
    if (g.apps) {
        wm_flyout_rule(g.x, g.app_rule_y, g.w);
        wm_flyout_caption(g.app_x, g.app_cap_y, g.w - 2 * (g.app_x - g.x), "APPLICATIONS");
        for (int i = 0; i < g.apps; i++) {
            int ry = g.app_y + i * g.app_row_h;
            int hot = g_hover == HOVER_APP(i);
            if (hot) uui_fill_round_rect(wm_surface(), g.app_x - 6, ry, g.w - 2 * (g.app_x - 6 - g.x),
                                         g.app_row_h, 5, hover_bg);
            uint32_t row_bg = hot ? hover_bg : ink;
            char pidbuf[16];
            const char *label = app_label(i, pidbuf, sizeof pidbuf);
            // CLIPPED to where the track begins, never drawn over it.
            ugfx_draw_string_elided(wm_surface(), g.app_x, ry + (g.app_row_h - ch) / 2,
                                    g.app_slider_x - g.app_x - 6, label, UTHEME_TEXT, row_bg);
            if (g.app_slider_w > 0) uui_scale_draw(wm_surface(), &g_app_scale[i]);
            char pct[8];
            k_snprintf(pct, sizeof pct, "%ld%%", uui_scale_value(&g_app_scale[i]));
            ugfx_draw_string_clipped(wm_surface(), g.app_slider_x + g.app_slider_w + 6,
                                     ry + (g.app_row_h - ch) / 2,
                                     ugfx_text_width("100%") + 2, pct, UTHEME_TEXT, row_bg);
        }
    }

    // --- the outputs ---------------------------------------------------
    wm_flyout_rule(g.x, g.list_rule_y, g.w);
    wm_flyout_caption(g.app_x, g.list_cap_y, g.w - 2 * (g.app_x - g.x), "OUTPUT");
    for (int i = 0; i < g.rows; i++) {
        int ry = g.list_y + i * g.row_h;
        const char *note = k_strcmp(g_dev_value[i], "auto") ? g_dev_value[i] : "";
        wm_flyout_radio_row(g.list_x, ry, g.w - 2 * (g.list_x - g.x), g.row_h - 2,
                            i == g.selected_row, g_hover == HOVER_DEV(i), g_dev_label[i], note);
    }

    // --- the footer ----------------------------------------------------
    wm_flyout_button(g.muteall_x, g.btn_y, g.muted ? "Unmute" : "Mute all",
                     g.muted ? "tray-volume-high" : "tray-volume-muted", 1,
                     g_hover == HOVER_MUTEALL, 0);
    wm_flyout_icon_button(g.gear_x, g.btn_y, g.gear_w, "tb-gear", g_hover == HOVER_GEAR);
}
