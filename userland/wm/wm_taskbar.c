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

static enum taskbar_style g_style = TASKBAR_STYLE_CLASSIC;
static int g_dark = 1;
static int g_bar_h = TASKBAR_H_DEFAULT;
static int g_deflated;   // a floating panel filling its band, see below

enum taskbar_style taskbar_style(void) { return g_style; }
int taskbar_dark(void) { return g_dark; }
int taskbar_bar_h(void) { return g_bar_h; }
int taskbar_floating(void) { return g_style == TASKBAR_STYLE_FLOATING && !g_deflated; }

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
    int defl = g_style == TASKBAR_STYLE_FLOATING && want_deflated();
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
    setting_get("desktop.taskbar_style", &msg);
    enum taskbar_style style = TASKBAR_STYLE_CLASSIC;   // every value named, see below
    if (k_strcmp(msg.value, "centered") == 0)      style = TASKBAR_STYLE_CENTERED;
    else if (k_strcmp(msg.value, "floating") == 0) style = TASKBAR_STYLE_FLOATING;
    else if (k_strcmp(msg.value, "classic") == 0)  style = TASKBAR_STYLE_CLASSIC;
    setting_get("desktop.taskbar_theme", &msg);
    int dark = k_strcmp(msg.value, "light") != 0;

    int look_changed = style != g_style || dark != g_dark;
    g_style = style;
    g_dark = dark;
    g_deflated = g_style == TASKBAR_STYLE_FLOATING && want_deflated();

    // The BAND grows by the float gap in the floating style, so a
    // style change can move the work area as a height change does.
    g_bar_h = read_height();
    int h = g_bar_h + (g_style == TASKBAR_STYLE_FLOATING ? TASKBAR_FLOAT_GAP : 0);
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

// THE STRIP LISTS IN OPEN ORDER (`open_seq`), never windows[] order:
// windows[] is z-order, a click raises, and a raise moves the window to
// the end -- so a button listed by index jumped to the end of the strip
// every time it was clicked. Walked by next_opened(), which needs no
// buffer for a table that can grow.
static int next_opened(uint32_t *seq) {
    int best = -1;
    for (int j = 0; j < window_count; j++)
        if (windows[j].open_seq > *seq &&
            (best < 0 || windows[j].open_seq < windows[best].open_seq))
            best = j;
    if (best >= 0) *seq = windows[best].open_seq;
    return best;
}

// Every window is stamped where it is created; a path that forgot
// would make its window invisible to next_opened(), so one that
// arrives unstamped is stamped here rather than lost.
static void stamp_unopened(void) {
    for (int j = 0; j < window_count; j++)
        if (!windows[j].open_seq) windows[j].open_seq = wm_next_open_seq();
}

// Window i starts a group iff no window of its app OPENED before it, so
// a group sits where its first window did, whichever member is raised.
static int starts_group(int i) {
    if (unlisted(i)) return 0;
    for (int j = 0; j < window_count; j++)
        if (j != i && !unlisted(j) && same_app(i, j) &&
            windows[j].open_seq < windows[i].open_seq)
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
    // Half the BAR, not the band -- a floating panel's gap is not room
    // for a bigger icon. 24 in the 48px default, Windows 11's size.
    int s = g_bar_h / 2;
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
static void fill_button(struct taskbar_button *b, int first, int members,
                        int x, int w, int labelled) {
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

// The Start button's left edge, which the centred style derives from
// the buttons beside it -- so one function answers both.
static int g_start_x;

// Icon-only buttons, centred on the screen as one group with Start --
// Windows 11's shape. They never shrink: past the room there is, they
// group, and past that the extras are dropped and counted as hidden,
// the same response to pressure as the labelled layout below.
static int layout_centered(struct taskbar_button *out, int max,
                           const struct taskbar_geom *g, int listed) {
    int bw = g->btn_h + 4;
    int sw = start_btn_w();
    int lo = g->px + 4, hi = tray_left() - 8;
    int room = (hi - lo) - sw;                 // for the window buttons and their gaps
    int n = listed, grouped = 0;
    if (n * (bw + TB_GAP) > room) { n = group_count(); grouped = 1; }
    int fit = room / (bw + TB_GAP);
    if (fit < 0) fit = 0;
    if (n > fit) n = fit;
    if (n > max) n = max;

    int total = sw + n * (bw + TB_GAP);
    int sx = screen_w / 2 - total / 2;
    if (sx + total > hi) sx = hi - total;      // the tray wins over the centre
    if (sx < lo) sx = lo;
    g_start_x = sx;

    int count = 0;
    uint32_t seq = 0;
    for (int i; (i = next_opened(&seq)) >= 0; ) {
        if (unlisted(i)) continue;
        if (grouped && !starts_group(i)) continue;
        int members = grouped ? group_size(i) : 1;
        if (count >= n) { g_hidden += members; continue; }
        fill_button(&out[count], grouped ? group_front(i) : i, members,
                    sx + sw + TB_GAP + count * (bw + TB_GAP), bw, 0);
        count++;
    }
    return count;
}

int taskbar_layout(struct taskbar_button *out, int max) {
    g_hidden = 0;
    stamp_unopened();
    struct taskbar_geom g;
    taskbar_geom(&g);
    g_start_x = g.px + 4;
    int listed = listed_count();
    if (!out || max <= 0 || listed <= 0) {
        g_hidden = listed;
        if (g_style == TASKBAR_STYLE_CENTERED)   // Start alone, still centred
            g_start_x = screen_w / 2 - start_btn_w() / 2;
        return 0;
    }
    if (g_style == TASKBAR_STYLE_CENTERED) return layout_centered(out, max, &g, listed);

    int x0 = g_start_x + start_btn_w() + TB_GAP;
    int x1 = tray_left() - 8;
    int avail = x1 - x0;
    int natural = win_btn_w(), floor_w = btn_floor();
    if (avail < floor_w) { g_hidden = listed; return 0; }

    // Shrink first, group only if shrinking is not enough. Two windows
    // of the same app stay two buttons while there is room for two --
    // grouping is a response to pressure, not a policy.
    int n = listed, grouped = 0;
    int w = fit_width(avail, n, natural);
    if (w < floor_w) {
        n = group_count();
        grouped = 1;
        w = fit_width(avail, n, natural);
    }
    if (w < floor_w) w = floor_w; // past this the extras simply do not fit

    // A GROUPED button may be wider than a plain one, because there are
    // by definition few of them and its label carries a count as well as
    // a name -- at the plain natural width "notepad (32)" truncates to
    // "not (32)", which names nothing. Five characters is the widest
    // count suffix (" (99+)").
    //
    // THE ICON IS PART OF THE ALLOWANCE: make_label() is handed w MINUS
    // the icon column, so leaving it out here spent the widening on the
    // icon. Still capped by fit_width, so this can only spend space that
    // is genuinely spare.
    if (grouped) {
        int icon = taskbar_icon_size();
        int cap = natural + ugfx_text_width(" (99+)") + (icon ? icon + TB_ICON_GAP : 0);
        int wide = fit_width(avail, n, cap);
        if (wide > w) w = wide;
    }

    int count = 0;
    uint32_t seq = 0;
    for (int i; (i = next_opened(&seq)) >= 0; ) {
        if (unlisted(i)) continue;
        if (grouped && !starts_group(i)) continue;
        int x = x0 + count * (w + TB_GAP);
        int members = grouped ? group_size(i) : 1;
        if (count >= max || x + w > x1) { g_hidden += members; continue; }
        fill_button(&out[count], grouped ? group_front(i) : i, members, x, w, 1);
        count++;
    }
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
                // Only where the button does not already say it --
                // Explorer's rule. A grouped button's list is one click away.
                if ((g_style == TASKBAR_STYLE_CENTERED || b[k].elided) && b[k].count == 1)
                    { tip = windows[b[k].first].title; tx = b[k].x; tw = b[k].w; }
                break;
            }
        }
    }
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
    for (int i; n < TB_GROUP_ROWS && (i = next_opened(&seq)) >= 0; ) {
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
static void activate(int i) {
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

int taskbar_handle_click(int mx, int my) {
    static struct taskbar_button btns[TB_BUTTONS_MAX];
    int n = taskbar_layout(btns, TB_BUTTONS_MAX);
    int ty = screen_h - taskbar_h;
    for (int b = 0; b < n; b++) {
        if (!uui_hit(btns[b].x, ty, btns[b].w, taskbar_h, mx, my)) continue;
        if (g_tip_ours) { wm_tooltip_cancel(); g_tip_ours = 0; }
        if (btns[b].count > 1) open_group_menu(&btns[b]);
        else activate(btns[b].first);
        redraw_pending = 1;
        return 1;
    }
    return 0;
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
