// The taskbar strip's layout and click routing -- see wm_taskbar.h for
// what it does and why it is one file rather than three walks.
#include "wm/wm_watch.h" // taskbar_poll_config()'s change counter
#include "wm_internal.h"
#include "wm_taskbar.h"
#include "lib/usetting.h" // the MERGED registry -- see setting_get()
#include "wm_anim.h"
#include "wm_tray.h"
#include "context_menu.h"
#include "ui/uui.h"
#include "kapi.h"
#include "rt/sys.h"
#include "wm/wm_conf.h"   // struct setting_msg, SETTING_OP_*
#include "wm_tooltip.h"
#include "start_menu.h"   // start_menu_open
#include "wm_peek.h"
#include "wm_dnd.h"
#include "lib/utween.h"
#include "wm_shadow.h"
#include "ui/utheme.h"

#define TB_GAP 4

// The narrowest a window button is allowed to get before grouping takes
// over. Font-derived, like every other measurement on this strip: three
// characters plus the button's own padding is the point below which a
// label stops telling you anything, which is roughly where Windows and
// KDE both stop shrinking too. An icon-only button never shrinks.
static int btn_floor(void) { return 3 * ugfx_char_w() + 2 * TB_PAD; }

static int g_hidden;

int taskbar_hidden(void) { return g_hidden; }

// --- the Start button's appearance ------------------------------------
//
// See wm_taskbar.h. Read through the SETTINGS REGISTRY rather than
// straight out of /etc/desktop.conf, which is the note
// desktop.c's wallpaper_reload() carries and the reason is the same:
// the registry knows the default and the legal values, so reading the
// file here would put a second copy of the default in a second place
// and the two would disagree the first time either moved.
static enum start_button_mode g_start_mode = START_BUTTON_BOTH;

enum start_button_mode taskbar_start_mode(void) { return g_start_mode; }

int start_icon_size(void) {
    if (g_start_mode == START_BUTTON_TEXT) return 0;
    return taskbar_icon_size();
}

int taskbar_default_h(void) { return TASKBAR_H_DEFAULT; }

// --- style, theme and the panel's geometry ------------------------------

static enum taskbar_buttons g_buttons = TASKBAR_BUTTONS_LABELLED;
static int g_float;
static int g_align_center;
static int g_start_center;
static int g_dark = 1;
static int g_bar_h = TASKBAR_H_DEFAULT;
static int g_deflated;   // a floating panel filling its band, see below

enum taskbar_buttons taskbar_buttons(void) { return g_buttons; }
int taskbar_align_centered(void) { return g_align_center; }
int taskbar_dark(void) { return g_dark; }
int taskbar_bar_h(void) { return g_bar_h; }
int taskbar_float_on(void) { return g_float; }
int taskbar_start_centered(void) { return g_start_center; }
int taskbar_floating(void) { return g_float && !g_deflated; }

static struct taskbar_palette g_pal;
static int g_pal_for = -1;   // which theme g_pal holds; -1 = not yet filled

static uint32_t hex(uint32_t rgb) {
    return ugfx_rgb((uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb);
}

// FILLED AT RUNTIME, as utheme's is: ugfx_rgb() knows the surface's
// pixel format and a static initialiser cannot.
const struct taskbar_palette *taskbar_palette(void) {
    if (g_pal_for == g_dark) return &g_pal;
    struct taskbar_palette *p = &g_pal;
    uint32_t accent = UTHEME_ACCENT;
    if (g_dark) {
        p->bar_rgb = 0x1E1E22; p->edge_rgb = 0x2E2E35;
        p->focus = hex(0x383842);
        p->focus_edge = 0;
        p->text_rgb = 0xE6E6E6; p->dim = hex(0x9C9CA6);
        p->running = hex(0x8A8A94);
        p->accent = ugfx_blend(accent, hex(0xFFFFFF), 77);
    } else {
        p->bar_rgb = 0xECECEC; p->edge_rgb = 0xCDCDD2;
        p->focus = hex(0xFFFFFF);
        p->focus_edge = hex(0xC8C8D0);
        p->text_rgb = 0x141414; p->dim = hex(0x62626A);
        p->running = hex(0x8C8C96);
        p->accent = accent;
    }
    p->bar = hex(p->bar_rgb);
    p->edge = hex(p->edge_rgb);
    p->text = hex(p->text_rgb);
    // Derived, never picked: uui_state_bg() lightens a dark ground and
    // darkens a light one, as every control's hover does.
    p->hover = uui_state_bg(p->bar, UUI_STATE_HOVER);
    g_pal_for = g_dark;
    return p;
}

void taskbar_geom(struct taskbar_geom *g) {
    int band_y = screen_h - taskbar_h;
    if (taskbar_floating()) {
        g->px = TASKBAR_FLOAT_GAP;
        g->pw = screen_w - 2 * TASKBAR_FLOAT_GAP;
        g->py = band_y;
        g->ph = g_bar_h;
        // Breeze's proportion -- half the line height -- the radius
        // this desktop's windows already use.
        g->radius = ugfx_char_h() / 2;
    } else {
        g->px = 0; g->pw = screen_w;
        g->py = band_y; g->ph = taskbar_h;
        g->radius = 0;
    }
    if (g->radius > g->ph / 2) g->radius = g->ph / 2;
    int inset = g_bar_h / 12;             // 4 at the 48px default
    if (inset < 2) inset = 2;
    g->btn_h = g_bar_h - 2 * inset;
    g->btn_y = g->py + (g->ph - g->btn_h) / 2;
    // Nested corners stay concentric: the panel's radius less the inset.
    g->btn_r = g->radius > inset ? g->radius - inset : 4;
}

// A floating panel DEFLATES while any window is maximized -- the one
// case where a gap under the panel would be a strip of wallpaper
// between the window and the bar. Asked once a frame (taskbar_poll_config)
// so the drawing and the hit tests of one frame agree.
static int want_deflated(void) {
    for (int i = 0; i < window_count; i++)
        if (windows[i].state == WIN_MAXIMIZED && !windows[i].popup) return 1;
    return 0;
}

// The band, and the shadow a floating panel casts above it.
static void damage_strip(void) {
    int m = wm_shadow_margin();
    redraw_pending = 1;
    wm_damage_rect(0, screen_h - taskbar_h - m, screen_w, taskbar_h + m);
}

// **THROUGH usetting_get(), NOT sys_setting().** These settings are
// DECLARED by files in /etc/settings.d rather than registered in the
// kernel, so the syscall does not know them -- it answers for the
// kernel's half alone, and asking it reads every one of them as unset.
static void setting_get(const char *name, struct setting_msg *msg) {
    for (unsigned i = 0; i < sizeof *msg; i++) ((uint8_t *)msg)[i] = 0;
    if (!usetting_get(name, msg->value, sizeof msg->value)) msg->value[0] = 0;
}

// `desktop.taskbar_height` as the registry answers it, or the default
// if the registry could not be asked. The registry bounds a WRITTEN
// value; the clamp here covers a hand-edited file, since a 4px strip
// has no button left to recover from.
static int read_height(void) {
    struct setting_msg msg;
    setting_get("desktop.taskbar_height", &msg);
    int64_t v;
    if (!msg.value[0] || !k_parse_i64(msg.value, &v)) return taskbar_default_h();
    if (v < TASKBAR_H_MIN) v = TASKBAR_H_MIN;
    if (v > TASKBAR_H_MAX) v = TASKBAR_H_MAX;
    return (int)v;
}

void taskbar_poll_config(void) {
    int defl = g_float && want_deflated();
    if (defl != g_deflated) {
        g_deflated = defl;
        damage_strip();
    }

    static uint64_t seen_gen;
    static int primed;
    uint64_t gen = wm_watch_config_gen();
    if (primed && gen == seen_gen) return;
    seen_gen = gen;
    primed = 1;

    // THE HEIGHT FIRST, because everything below the strip moves with
    // it: the desktop grid, every maximized window, the Start menu's
    // anchor. wm_layout_changed() re-derives all of that, the same walk
    // a screen-size change makes.
    struct setting_msg msg;
    setting_get("desktop.taskbar_buttons", &msg);
    enum taskbar_buttons buttons = k_strcmp(msg.value, "icons") == 0
        ? TASKBAR_BUTTONS_ICONS : TASKBAR_BUTTONS_LABELLED;
    setting_get("desktop.taskbar_align", &msg);
    int ac = k_strcmp(msg.value, "center") == 0;
    setting_get("desktop.taskbar_float", &msg);
    int fl = k_strcmp(msg.value, "on") == 0;
    setting_get("desktop.start_position", &msg);
    int sc = k_strcmp(msg.value, "center") == 0;
    setting_get("desktop.taskbar_theme", &msg);
    int dark = k_strcmp(msg.value, "light") != 0;
    setting_get("desktop.taskbar_peek", &msg);
    wm_peek_set_mode(k_strcmp(msg.value, "off") == 0 ? WM_PEEK_OFF :
                     k_strcmp(msg.value, "highlight") == 0 ? WM_PEEK_HIGHLIGHT :
                     WM_PEEK_PREVIEW);

    int look_changed = buttons != g_buttons || ac != g_align_center ||
                       sc != g_start_center || dark != g_dark;
    g_buttons = buttons;
    g_align_center = ac;
    g_float = fl;
    g_start_center = sc;
    g_dark = dark;
    g_deflated = g_float && want_deflated();

    // The BAND grows by the float gap while the panel floats, so a
    // float change can move the work area as a height change does.
    g_bar_h = read_height();
    int h = g_bar_h + (g_float ? TASKBAR_FLOAT_GAP : 0);
    if (h != taskbar_h) {
        taskbar_h = h;
        wm_layout_changed();
    } else if (look_changed) {
        damage_strip();
    }

    // The default when nothing is written; it must match Default= in
    // /etc/settings.d/desktop.start_button, which is what the registry
    // answers with and what System Settings shows.
    //
    // **EVERY MODE IS NAMED, including the default one.** This tested
    // only `icon` and `both` and let `text` fall through to the
    // initialiser, which was silently the same value -- so moving the
    // default made `text` unreachable while both other modes still
    // worked, and the taskbar simply ignored the setting for one of its
    // three values. Found by icons_test, which had been passing that
    // case for the wrong reason.
    enum start_button_mode want = START_BUTTON_ICON;
    setting_get("desktop.start_button", &msg);
    if (k_strcmp(msg.value, "text") == 0)      want = START_BUTTON_TEXT;
    else if (k_strcmp(msg.value, "icon") == 0) want = START_BUTTON_ICON;
    else if (k_strcmp(msg.value, "both") == 0) want = START_BUTTON_BOTH;
    if (want == g_start_mode) return;

    g_start_mode = want;
    // THE WHOLE STRIP MOVES, not just the button: every window button
    // starts to the right of start_btn_w(), so a mode change relays all
    // of them. Damaging the strip rather than the screen because that
    // is all that can have changed -- the Start MENU is closed-height
    // zero when it is not open, and opening it damages its own rect.
    damage_strip();
}

// Do these two windows belong to the same application?
//
// By app_identity: an opaque number the KERNEL derived from each owning
// process's spawn path (wm.h), so two Notepad processes group and
// Notepad plus Calculator cannot -- whatever either of them declares
// about itself.
//
// It used to compare `app_id`, the name an app gives itself, and that
// cannot be made safe: two apps declaring the same string would merge
// into one taskbar button, and no check at this end could tell that
// from the legitimate case of two copies of one program, which MUST
// merge. An identity the app cannot influence has no such case.
//
// -1 means "no identity" -- a kernel-space app window, or a client with
// no scheduler slot. Those fall back to the client pid, which groups a
// single process's own windows and can never merge two unrelated ones;
// pid 0 is nobody's peer, hence the explicit `!= 0`, without which
// every such window would collapse into one button.
static int same_app(int a, int b) {
    int ia = windows[a].app_identity, ib = windows[b].app_identity;
    if (ia >= 0 || ib >= 0) return ia == ib;
    return windows[a].client_pid != 0 &&
            windows[a].client_pid == windows[b].client_pid;
}

// A menu is not a window the strip lists -- and neither is a DIALOG,
// which belongs to the window that owns it (Win32 gives an owned window
// no button of its own either). Both are `unlisted`.
static int unlisted(int i) { return windows[i].popup || windows[i].dialog; }

// THE STRIP LISTS IN `task_rank` ORDER, never windows[] order:
// windows[] is z-order, a click raises, and a raise moves the window to
// the end -- so a button listed by index jumped to the end of the strip
// every time it was clicked. Walked by next_in_order(), which needs no
// buffer for a table that can grow. Ranks are unique: every one comes
// from wm_next_open_seq().
static int next_in_order(uint32_t *rank) {
    int best = -1;
    for (int j = 0; j < window_count; j++)
        if (windows[j].task_rank > *rank &&
            (best < 0 || windows[j].task_rank < windows[best].task_rank))
            best = j;
    if (best >= 0) *rank = windows[best].task_rank;
    return best;
}

// Every window is stamped where it is created; a path that forgot
// would make its window invisible to next_in_order(), so one that
// arrives unstamped is stamped here rather than lost.
static void stamp_unopened(void) {
    for (int j = 0; j < window_count; j++) {
        if (!windows[j].open_seq) windows[j].open_seq = wm_next_open_seq();
        // A new window joins at the END of the strip.
        if (!windows[j].task_rank) windows[j].task_rank = wm_next_open_seq();
    }
}

// Window i starts a group iff no window of its app RANKS before it, so
// a group sits where its first window does, whichever member is raised.
static int starts_group(int i) {
    if (unlisted(i)) return 0;
    for (int j = 0; j < window_count; j++)
        if (j != i && !unlisted(j) && same_app(i, j) &&
            windows[j].task_rank < windows[i].task_rank)
            return 0;
    return 1;
}

// How many windows the strip has to fit -- see unlisted().
static int listed_count(void) {
    int n = 0;
    for (int i = 0; i < window_count; i++) if (!unlisted(i)) n++;
    return n;
}

static int group_count(void) {
    int n = 0;
    for (int i = 0; i < window_count; i++) if (starts_group(i)) n++;
    return n;
}

// The frontmost member of i's group -- windows[] is back-to-front, so
// that is the highest index. This is the window a plain click acts on,
// which is what makes a grouped button behave like the app rather than
// like whichever of its windows happened to open first.
static int group_front(int i) {
    int front = i;
    for (int j = 0; j < window_count; j++)
        if (j > front && !unlisted(j) && same_app(i, j)) front = j;
    return front;
}

static int group_size(int i) {
    int n = 1;
    for (int j = 0; j < window_count; j++)
        if (j != i && !unlisted(j) && same_app(i, j)) n++;
    return n;
}

// The widest each of `n` buttons may be to fit `avail` pixels with a gap
// between them: n*w + (n-1)*gap <= avail. Never wider than natural.
static int fit_width(int avail, int n, int natural) {
    if (n <= 0) return natural;
    int w = (avail + TB_GAP) / n - TB_GAP;
    return w > natural ? natural : w;
}

// Copies as much of `src` as fits in a button `w` wide, then appends
// " (N)" when the button stands for more than one window.
//
// TRUNCATED HERE even though uui_button_draw() clips, because it CENTRES
// the label first: an over-long one starts left of the button and is cut
// off on the right, so it loses characters at both ends and reads as
// neither the name nor a truncation of it. Trimming to what fits means
// the text that IS shown is the start of the name, centred.
// The icon square inside a button, derived from the strip's height so it
// scales with the font like everything else on it.
int taskbar_icon_size(void) {
    // From the BAR, not the band -- a floating panel's gap is not room
    // for a bigger icon. Half of it beside a label (24 in the 48px
    // default, Windows 11's size); two thirds when the icon IS the
    // button, since 24 read as "really small" on a 1080p laptop panel.
    int s = g_buttons == TASKBAR_BUTTONS_ICONS ? g_bar_h * 2 / 3 : g_bar_h / 2;
    // Capped: the masters are 64px and a thick strip would otherwise
    // ask for an upscaled icon.
    if (s > TASKBAR_ICON_MAX) s = TASKBAR_ICON_MAX;
    return s < 8 ? 0 : s;
}

// Returns 1 when the name was cut short -- and then it ends in "..",
// ugfx_draw_string_elided()'s mark (the font has no U+2026), so a cut
// title reads as cut rather than as a shorter title.
static int make_label(char *dst, int cap, const char *src, int w, int count) {
    // Matches win_btn_w()'s own padding, so a full-width label sits
    // inside the button rather than touching its edges.
    int avail = w - 2 * TB_PAD;
    if (avail < 0) avail = 0;

    char suffix[8];
    int sn = 0;
    if (count > 1) {
        suffix[sn++] = ' '; suffix[sn++] = '(';
        if (count > 99) { suffix[sn++] = '9'; suffix[sn++] = '9'; suffix[sn++] = '+'; }
        else {
            if (count >= 10) suffix[sn++] = (char)('0' + count / 10);
            suffix[sn++] = (char)('0' + count % 10);
        }
        suffix[sn++] = ')';
    }
    suffix[sn] = '\0';

    // **THE NAME IS MEASURED, NOT COUNTED IN CELLS.** This divided the
    // button's width by ugfx_char_w() -- the WIDEST advance -- which is
    // right only on a monospace face; with a proportional interface
    // face it cut "Terminal" to "Termina" on a button with room to
    // spare. The suffix is measured too, and reserved from the width
    // rather than from a character count, because "(12)" is not four
    // cells wide in a face where it is not four cells wide.
    int sufw = sn ? ugfx_text_width(suffix) : 0;
    int nameavail = avail - sufw;
    if (nameavail < 0) nameavail = 0;

    int len = src ? (int)k_strlen(src) : 0;
    int nameroom = src ? ugfx_text_fit_chars(src, nameavail) : 0;
    int elided = nameroom < len;
    if (elided) {
        nameroom = ugfx_text_fit_chars(src, nameavail - ugfx_text_width(".."));
        if (nameroom < 1) nameroom = 1;   // one letter and the mark beats the mark alone
    }
    if (nameroom > cap - 3 - sn) { nameroom = cap - 3 - sn; elided = 1; }
    if (nameroom < 0) nameroom = 0;

    int n = 0;
    for (; src && src[n] && n < nameroom; n++) dst[n] = src[n];
    if (elided) { dst[n++] = '.'; dst[n++] = '.'; }
    for (int k = 0; k < sn && n < cap - 1; k++) dst[n++] = suffix[k];
    dst[n] = '\0';
    return elided;
}

int taskbar_button_rect_for(int idx, int *x, int *y, int *w, int *h) {
    // STATIC: 3.5 KB of buttons on a 16 KiB ring-3 stack is what the
    // frame budget exists to catch, and the WM has one thread.
    static struct taskbar_button b[64];
    int n = taskbar_layout(b, 64);
    for (int k = 0; k < n; k++) {
        if (b[k].first != idx && !same_app(b[k].first, idx)) continue;
        struct taskbar_geom g;
        taskbar_geom(&g);
        *x = b[k].x; *w = b[k].w;
        *y = g.btn_y; *h = g.btn_h;
        return 1;
    }
    return 0;
}

// The label and icon for button `slot`, standing for `members` windows
// from `first`, `w` pixels wide. `labelled` is 0 for an icon-only
// button, whose label is kept whole for the tooltip and the debug report.
static void fill_button(struct taskbar_button *b, int starter, int first, int members,
                        int x, int w, int labelled) {
    b->key = windows[starter].open_seq;
    b->dragging = 0;
    b->x = x;
    b->w = w;
    b->first = first;
    b->count = members;
    // A GROUPED button is named after the APPLICATION, an ungrouped
    // one after its window -- which is what Windows and KDE both
    // show, and the only naming that stays true when the group's
    // frontmost window changes underneath it. Falls back to the
    // title for a window whose client declared no app id.
    const char *name = windows[first].title;
    if (members > 1 && windows[first].app_id[0]) name = windows[first].app_id;
    b->icon = wm_window_icon_name(first);
    if (!labelled) {
        b->elided = 0;
        k_strlcpy(b->label, name, sizeof b->label);
        return;
    }
    // The label gets what the ICON does not take. Reserved from the
    // same taskbar_icon_size() the renderer blits with, so the two
    // cannot disagree about where the text starts.
    int label_w = w - (b->icon ? taskbar_icon_size() + TB_ICON_GAP : 0);
    b->elided = make_label(b->label, (int)sizeof b->label, name, label_w, members);
}

// The Start button's left edge, which a centred Start derives from the
// buttons beside it -- so one function answers both.
static int g_start_x;

// A PRESS ON A WINDOW BUTTON, and whether it has become a drag. Held by
// the button's `key`, never by index or position -- both move under it.
static struct {
    int armed, dragging;
    uint32_t key;
    int press_x, press_y;
    int grab_dx;          // pointer x minus the button's x at the press
    int mx;               // the pointer now
} g_drag;
#define TB_DRAG_START 4   // px before a press becomes a drag: Windows' SM_CXDRAG

// THE LIVE REORDER: the dragged button leaves its slot and follows the
// pointer, clamped to the row; the slot it is nearest is where it would
// land, and the others close up around that slot. Done HERE, in the one
// layout, so drawing, the drop and `gui taskbar` all see the same row.
static void apply_drag(struct taskbar_button *out, int count, int w) {
    int d = -1;
    for (int k = 0; k < count; k++) if (out[k].key == g_drag.key) d = k;
    if (d < 0 || count < 2) return;
    int pitch = w + TB_GAP, x0 = out[0].x;
    int vis = g_drag.mx - g_drag.grab_dx;
    if (vis < x0) vis = x0;
    if (vis > x0 + (count - 1) * pitch) vis = x0 + (count - 1) * pitch;
    int t = (vis - x0 + pitch / 2) / pitch;
    struct taskbar_button moved = out[d];
    if (t > d) for (int k = d; k < t; k++) out[k] = out[k + 1];
    else       for (int k = d; k > t; k--) out[k] = out[k - 1];
    out[t] = moved;
    for (int k = 0; k < count; k++) out[k].x = x0 + k * pitch;
    out[t].x = vis;
    out[t].dragging = 1;
}

// ONE LAYOUT FOR EVERY COMBINATION of the settings: labelled or icon
// buttons, the buttons left or centred, Start left or centred. Start is
// placed first and takes its room out of the strip; the buttons are
// sized for what is left, then placed by alignment.
//
//   Start left     the corner; the buttons get everything after it
//   Start centre   Start leads the centred group (Windows 11). The
//                  buttons cannot be left of a centred Start: the
//                  declaration's Requires=/Otherwise= makes
//                  `desktop.taskbar_align` read `center` meanwhile.
int taskbar_layout(struct taskbar_button *out, int max) {
    g_hidden = 0;
    stamp_unopened();
    struct taskbar_geom g;
    taskbar_geom(&g);
    int icons = g_buttons == TASKBAR_BUTTONS_ICONS;
    int sw = start_btn_w();
    int lo = g.px + 4, hi = tray_left() - 8;
    int with_group = g_start_center;

    if (!g_start_center) {
        g_start_x = lo;
        lo += sw + TB_GAP;
    } else {
        g_start_x = screen_w / 2 - sw / 2;   // alone until there are buttons
    }
    int avail = hi - lo - (with_group ? sw + TB_GAP : 0);

    int listed = listed_count();
    if (!out || max <= 0 || listed <= 0 || avail <= 0) { g_hidden = listed; return 0; }

    // THE WIDTH, and whether to group. Labelled buttons shrink first and
    // group only if shrinking is not enough -- two windows of one app
    // stay two buttons while there is room, since grouping is a response
    // to pressure, not a policy. An icon button never shrinks; it is
    // already the icon.
    int n = listed, grouped = 0, w;
    if (icons) {
        w = g.btn_h + 8;                       // 48 at the default: the icon is the button
        if (n * (w + TB_GAP) - TB_GAP > avail) { n = group_count(); grouped = 1; }
    } else {
        int natural = win_btn_w(), floor_w = btn_floor();
        if (avail < floor_w) { g_hidden = listed; return 0; }
        w = fit_width(avail, n, natural);
        if (w < floor_w) {
            n = group_count();
            grouped = 1;
            w = fit_width(avail, n, natural);
        }
        if (w < floor_w) w = floor_w;          // past this the extras simply do not fit
        // A GROUPED button may be wider than a plain one: there are few of
        // them and its label carries a count as well as a name ("notepad
        // (32)" at the plain width is "not (32)"). The icon is part of the
        // allowance, since make_label() is handed w minus the icon column.
        if (grouped) {
            int icon = taskbar_icon_size();
            int cap = natural + ugfx_text_width(" (99+)") + (icon ? icon + TB_ICON_GAP : 0);
            int wide = fit_width(avail, n, cap);
            if (wide > w) w = wide;
        }
    }
    int fit = (avail + TB_GAP) / (w + TB_GAP);
    if (n > fit) n = fit;
    if (n > max) n = max;

    // THE PLACE. A centred group is centred on the SCREEN, as Windows'
    // is, and pushed aside only by what it would otherwise overlap.
    int row = n > 0 ? n * (w + TB_GAP) - TB_GAP : 0;
    int group = row + (with_group ? sw + (n > 0 ? TB_GAP : 0) : 0);
    int x = lo;
    if (g_align_center || with_group) {
        x = screen_w / 2 - group / 2;
        if (x + group > hi) x = hi - group;
        if (x < lo) x = lo;
    }
    if (with_group) {
        g_start_x = x;
        x += sw + TB_GAP;
    }

    int count = 0;
    uint32_t seq = 0;
    for (int i; (i = next_in_order(&seq)) >= 0; ) {
        if (unlisted(i)) continue;
        if (grouped && !starts_group(i)) continue;
        int members = grouped ? group_size(i) : 1;
        if (count >= n) { g_hidden += members; continue; }
        fill_button(&out[count], i, grouped ? group_front(i) : i, members,
                    x + count * (w + TB_GAP), w, !icons);
        count++;
    }
    if (g_drag.dragging) apply_drag(out, count, w);
    return count;
}

void taskbar_start_rect(int *x, int *y, int *w, int *h) {
    static struct taskbar_button b[64];   // static: see taskbar_button_rect_for()
    taskbar_layout(b, 64);                // places g_start_x
    struct taskbar_geom g;
    taskbar_geom(&g);
    *x = g_start_x; *y = g.btn_y;
    *w = start_btn_w(); *h = g.btn_h;
}

void taskbar_start_hit_rect(int *x, int *y, int *w, int *h) {
    int sx, sy, sw, sh;
    taskbar_start_rect(&sx, &sy, &sw, &sh);
    struct taskbar_geom g;
    taskbar_geom(&g);
    // The screen's corner is the Start button whenever nothing sits
    // between them -- Windows' rule since 95.
    if (sx <= g.px + 4) { sw += sx; sx = 0; }
    *x = sx; *w = sw;
    *y = screen_h - taskbar_h; *h = taskbar_h;
}

// ---- hover -----------------------------------------------------------

static int g_hover = -1;
static int g_tip_ours;   // the panel tooltip is showing OUR text, so ours to cancel

int taskbar_hover(void) { return g_hover; }

void taskbar_update_hover(int mx, int my, uint8_t buttons) {
    // A held button keeps what it had -- the rule wm_overlay_hover()
    // applies to every overlay.
    if (buttons & 0x1) return;
    int want = -1;
    const char *tip = 0;
    int tx = 0, tw = 0;
    int peek_wins[8], peek_n = 0, peek_x = 0, peek_w = 0;
    int band = my >= screen_h - taskbar_h && my < screen_h && !wm_top_covers_screen();
    if (band) {
        int sx, sy, sw, sh;
        taskbar_start_hit_rect(&sx, &sy, &sw, &sh);
        if (uui_hit(sx, sy, sw, sh, mx, my)) {
            want = TASKBAR_HOVER_START;
        } else {
            static struct taskbar_button b[64];
            int n = taskbar_layout(b, 64);
            for (int k = 0; k < n; k++) {
                if (!uui_hit(b[k].x, screen_h - taskbar_h, b[k].w, taskbar_h, mx, my))
                    continue;
                want = b[k].first;
                // THE PEEK CARD, where it can show every window the
                // button stands for -- in open order, as the strip is.
                if (wm_peek_mode() != WM_PEEK_OFF) {
                    uint32_t seq = 0;
                    for (int i; peek_n < 8 && (i = next_in_order(&seq)) >= 0; )
                        if (!unlisted(i) && (i == b[k].first ||
                                             (b[k].count > 1 && same_app(i, b[k].first))))
                            peek_wins[peek_n++] = i;
                    for (int j = 0; j < peek_n; j++)
                        if (!wm_peek_can_show(peek_wins[j])) peek_n = 0;
                    peek_x = b[k].x; peek_w = b[k].w;
                }
                // Otherwise the title, only where the button does not
                // already say it -- Explorer's rule. A grouped button's
                // list is one click away.
                if (!peek_n && (g_buttons == TASKBAR_BUTTONS_ICONS || b[k].elided) &&
                    b[k].count == 1)
                    { tip = windows[b[k].first].title; tx = b[k].x; tw = b[k].w; }
                break;
            }
        }
    }
    wm_peek_hover(peek_wins, peek_n, peek_x, peek_w, mx, my);
    if (tip && !start_menu_open) {
        struct taskbar_geom g;
        taskbar_geom(&g);
        wm_tooltip_track(tip, tx, g.btn_y, tw, g.btn_h);
        g_tip_ours = 1;
    } else if (g_tip_ours) {
        wm_tooltip_cancel();
        g_tip_ours = 0;
    }
    if (want == g_hover) return;
    g_hover = want;
    damage_strip();
}

// ---- clicks ----------------------------------------------------------

#define TB_BUTTONS_MAX 64

// The group popup's rows. Static because context_menu_open_at() keeps
// the caller's array for as long as the menu is open (context_menu.h),
// so a stack local would be a dangling read the moment this returns.
#define TB_GROUP_ROWS 12
static struct context_menu_item g_rows[TB_GROUP_ROWS];
static int g_row_target[TB_GROUP_ROWS];
static char g_row_label[TB_GROUP_ROWS][WIN_LABEL_MAX_CHARS * 3];

static void row_raise(void *ctx) {
    int i = *(int *)ctx;
    if (i < 0 || i >= window_count) return; // the window may have closed while the menu was open
    if (windows[i].state == WIN_MINIMIZED) { wm_anim_restore(i); windows[i].state = WIN_NORMAL; }
    wm_ensure_reachable(i);
    raise_with_dialogs(i);
    redraw_pending = 1;
}

// Opens the list of windows a collapsed button stands for -- Windows'
// jump list, KDE's grouped-task popup. Anchored at the button's top-left
// on the taskbar; context_menu_open_at() lifts it clear of the strip.
static void open_group_menu(const struct taskbar_button *b) {
    int n = 0;
    uint32_t seq = 0;
    for (int i; n < TB_GROUP_ROWS && (i = next_in_order(&seq)) >= 0; ) {
        if (unlisted(i) || !same_app(b->first, i)) continue;
        int k = 0;
        for (; windows[i].title[k] && k < (int)sizeof g_row_label[n] - 1; k++)
            g_row_label[n][k] = windows[i].title[k];
        g_row_label[n][k] = '\0';
        g_row_target[n] = i;
        g_rows[n].label = g_row_label[n];
        g_rows[n].on_select = row_raise;
        g_rows[n].ctx = &g_row_target[n];
        n++;
    }
    if (n == 0) return;
    context_menu_open_at(b->x, screen_h - taskbar_h, g_rows, n);
}

// Acting on one window: raise it, recover it, or minimize it. Unchanged
// behaviour, moved here so the button rect it is tested against is the
// one taskbar_layout() placed.
// **THE STRIP RAISES A WINDOW'S DIALOGS WITH IT.** The button is the
// only handle a window with a modal dialog has out here -- its own title
// bar takes no press (wm_handle_left_click()) -- so raising it and
// burying the dialog underneath would leave the app looking wedged.
void taskbar_activate(int i) {
    if (i < 0 || i >= window_count) return;
    if (windows[i].state == WIN_MINIMIZED) {
        wm_anim_restore(i);
        windows[i].state = WIN_NORMAL;
        wm_ensure_reachable(i);
        raise_with_dialogs(i);
    } else if (wm_ensure_reachable(i)) {
        // It was somewhere it could not be grabbed -- off an edge, or
        // behind the taskbar. RECOVERING it is the action, taking
        // priority over the minimize toggle below: minimizing something
        // the user cannot see does nothing they can perceive, and this
        // button is the only handle such a window has left.
        raise_with_dialogs(i);
    } else if (i == wm_focus_index()) {
        wm_anim_minimize(i);
        windows[i].state = WIN_MINIMIZED;
    } else {
        raise_with_dialogs(i);
    }
    redraw_pending = 1;
}

// The PRESS only arms the button: whether it was a click or the start of
// a drag is decided by what the pointer does next (taskbar_update_press).
int taskbar_handle_click(int mx, int my) {
    static struct taskbar_button btns[TB_BUTTONS_MAX];
    int n = taskbar_layout(btns, TB_BUTTONS_MAX);
    int ty = screen_h - taskbar_h;
    for (int b = 0; b < n; b++) {
        if (!uui_hit(btns[b].x, ty, btns[b].w, taskbar_h, mx, my)) continue;
        if (g_tip_ours) { wm_tooltip_cancel(); g_tip_ours = 0; }
        wm_peek_close();
        g_drag.armed = 1;
        g_drag.dragging = 0;
        g_drag.key = btns[b].key;
        g_drag.press_x = g_drag.mx = mx;
        g_drag.press_y = my;
        g_drag.grab_dx = mx - btns[b].x;
        damage_strip();
        return 1;
    }
    return 0;
}

uint32_t taskbar_armed_key(void) { return g_drag.armed ? g_drag.key : 0; }

int taskbar_drag_state(int *armed, int *dragging) {
    if (armed) *armed = g_drag.armed;
    if (dragging) *dragging = g_drag.dragging;
    return g_drag.armed;
}

// THE DROP: the row as apply_drag() left it becomes the order. Every
// listed window is re-ranked, a group's members together and in their
// existing order, from the same counter new windows draw from -- so the
// ranks stay unique and a window opened later still lands at the end.
static void commit_order(void) {
    static struct taskbar_button b[TB_BUTTONS_MAX];
    int n = taskbar_layout(b, TB_BUTTONS_MAX);
    for (int k = 0; k < n; k++) {
        int starter = -1;
        for (int i = 0; i < window_count; i++)
            if (windows[i].open_seq == b[k].key) starter = i;
        if (starter < 0) continue;
        if (b[k].count == 1) { windows[starter].task_rank = wm_next_open_seq(); continue; }
        // A group: its members in their current rank order.
        uint32_t seq = 0;
        int members[TB_BUTTONS_MAX], m = 0;
        for (int i; m < TB_BUTTONS_MAX && (i = next_in_order(&seq)) >= 0; )
            if (!unlisted(i) && same_app(i, starter)) members[m++] = i;
        for (int j = 0; j < m; j++) windows[members[j]].task_rank = wm_next_open_seq();
    }
}

// Raise without the toggle a click has: a drag-over wants the window IN
// FRONT, and minimizing it because it happened to be focused would be
// the opposite of what the drop needs.
static void bring_forward(int i) {
    if (i < 0 || i >= window_count) return;
    if (windows[i].state == WIN_MINIMIZED) {
        wm_anim_restore(i);
        windows[i].state = WIN_NORMAL;
    }
    wm_ensure_reachable(i);
    raise_with_dialogs(i);
    redraw_pending = 1;
}

#define TB_DND_RAISE_TICKS 50   // a drag resting this long on a button raises its window

void taskbar_update_press(int mx, int my, uint8_t buttons) {
    int down = buttons & 0x1;

    // A CROSS-WINDOW DRAG resting on a button brings that window forward,
    // as Windows and KDE do, so the drop can land in it. Once per rest.
    static uint32_t dnd_key;
    static uint64_t dnd_since;
    if (wm_dnd_active() && down && my >= screen_h - taskbar_h && !wm_top_covers_screen()) {
        static struct taskbar_button b[TB_BUTTONS_MAX];
        int n = taskbar_layout(b, TB_BUTTONS_MAX);
        uint32_t key = 0;
        int first = -1;
        for (int k = 0; k < n; k++)
            if (uui_hit(b[k].x, screen_h - taskbar_h, b[k].w, taskbar_h, mx, my))
                { key = b[k].key; first = b[k].first; }
        uint64_t now = sys_ticks();
        if (key != dnd_key) { dnd_key = key; dnd_since = key ? now : 0; }
        else if (key && dnd_since && now - dnd_since >= TB_DND_RAISE_TICKS) {
            bring_forward(first);
            dnd_since = 0;
        }
    } else {
        dnd_key = 0;
    }

    if (!g_drag.armed) return;
    if (down) {
        if (mx == g_drag.mx && g_drag.dragging) return;
        g_drag.mx = mx;
        int dx = mx - g_drag.press_x, dy = my - g_drag.press_y;
        if (!g_drag.dragging && (dx > TB_DRAG_START || dx < -TB_DRAG_START ||
                                 dy > TB_DRAG_START || dy < -TB_DRAG_START))
            g_drag.dragging = 1;
        if (g_drag.dragging) damage_strip();
        return;
    }

    // THE RELEASE. A drag commits its order; a press that never became
    // one is a click -- if it was released on the button it went down on.
    if (g_drag.dragging) {
        commit_order();
    } else {
        static struct taskbar_button b[TB_BUTTONS_MAX];
        int n = taskbar_layout(b, TB_BUTTONS_MAX);
        for (int k = 0; k < n; k++) {
            if (b[k].key != g_drag.key) continue;
            if (!uui_hit(b[k].x, screen_h - taskbar_h, b[k].w, taskbar_h, mx, my)) break;
            if (b[k].count > 1) open_group_menu(&b[k]);
            else taskbar_activate(b[k].first);
            break;
        }
    }
    g_drag.armed = 0;
    g_drag.dragging = 0;
    damage_strip();
}

// ---- the glide ----------------------------------------------------------

#define TB_GLIDE_MS 150
#define TB_GLIDE_MAX 64

static struct glide {
    uint32_t key;
    int target;
    struct utween tw;
    uint64_t used;        // the frame it was last asked about; oldest is reused
} g_glide[TB_GLIDE_MAX];
static uint64_t g_frame;
static int g_gliding, g_gliding_prev;
static uint32_t g_glide_frames;   // frames that drew a glide, ever -- a test's counter

int taskbar_gliding(void) { return g_gliding_prev; }
uint32_t taskbar_glide_frames(void) { return g_glide_frames; }

int taskbar_draw_x(const struct taskbar_button *b) {
    unsigned long long now = sys_monotonic_ns();
    struct glide *e = 0, *oldest = &g_glide[0];
    for (int k = 0; k < TB_GLIDE_MAX; k++) {
        if (g_glide[k].key == b->key) { e = &g_glide[k]; break; }
        if (g_glide[k].used < oldest->used) oldest = &g_glide[k];
    }
    if (!e) {
        // A button seen for the first time APPEARS where it belongs.
        e = oldest;
        e->key = b->key;
        e->target = b->x;
        utween_start(&e->tw, b->x, b->x, 0, now);
    }
    e->used = ++g_frame;
    if (b->dragging || !wm_anim_enabled()) {
        // Under the pointer: exactly where it is. Recorded as the
        // resting value, so the drop glides FROM here into its slot.
        e->target = b->x;
        utween_start(&e->tw, b->x, b->x, 0, now);
        return b->x;
    }
    if (b->x != e->target) {
        e->target = b->x;
        utween_retarget(&e->tw, b->x, TB_GLIDE_MS, now);
    }
    int x = utween_value(&e->tw, now);
    if (utween_active(&e->tw)) g_gliding = 1;
    return x;
}

// Called once per strip draw: whether anything glided this frame decides
// whether the strip asks for the next one.
void taskbar_glide_frame_done(void) {
    g_gliding_prev = g_gliding;
    if (g_gliding) g_glide_frames++;
    g_gliding = 0;
    if (g_gliding_prev) damage_strip();
}

int taskbar_handle_right_click(int mx, int my) {
    static struct taskbar_button btns[TB_BUTTONS_MAX];
    int n = taskbar_layout(btns, TB_BUTTONS_MAX);
    int ty = screen_h - taskbar_h;
    for (int b = 0; b < n; b++) {
        if (!uui_hit(btns[b].x, ty, btns[b].w, taskbar_h, mx, my)) continue;
        if (btns[b].count > 1) {
            open_group_menu(&btns[b]);
        } else {
            // The window's own menu, as the title bar opens it -- what
            // KDE's task menu and Windows' Shift+right-click both offer.
            wm_open_window_menu(btns[b].first, mx, my);
        }
        return 1;
    }
    return 0;
}
