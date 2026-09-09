// The taskbar strip's layout and click routing -- see wm_taskbar.h for
// what it does and why it is one file rather than three walks.
#include "wm_internal.h"
#include "wm_taskbar.h"
#include "wm_tray.h"
#include "context_menu.h"
#include "ui/uui.h"
#include "kapi.h"
#include "rt/sys.h"
#include "wm/wm_conf.h"   // struct setting_msg, SETTING_OP_*
#include "taskbar_config.h" // TASKBAR_H_MIN/MAX/DEFAULT

#define TB_GAP 4

// The narrowest a window button is allowed to get before grouping takes
// over. Font-derived, like every other measurement on this strip: three
// characters plus the button's own padding is the point below which a
// label stops telling you anything, which is roughly where Windows and
// KDE both stop shrinking too.
static int btn_floor(void) { return 3 * ugfx_char_w() + 16; }

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

static void setting_get(const char *name, struct setting_msg *msg) {
    for (unsigned i = 0; i < sizeof *msg; i++) ((uint8_t *)msg)[i] = 0;
    msg->op = SETTING_OP_GET;
    k_strlcpy(msg->name, name, sizeof msg->name);
    if (sys_setting(msg) != 0) msg->value[0] = 0;
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
    static uint64_t seen_gen;
    static int primed;
    uint64_t gen = sys_fs_generation();
    if (primed && gen == seen_gen) return;
    seen_gen = gen;
    primed = 1;

    // THE HEIGHT FIRST, because everything below the strip moves with
    // it: the desktop grid, every maximized window, the Start menu's
    // anchor. wm_layout_changed() re-derives all of that, the same walk
    // a screen-size change makes.
    int h = read_height();
    if (h != taskbar_h) {
        taskbar_h = h;
        wm_layout_changed();
    }

    // The default when nothing is written; it must match
    // start_button_config.c's DEFAULT_MODE, which is what the registry
    // answers with and what System Settings shows.
    //
    // **EVERY MODE IS NAMED, including the default one.** This tested
    // only `icon` and `both` and let `text` fall through to the
    // initialiser, which was silently the same value -- so moving the
    // default made `text` unreachable while both other modes still
    // worked, and the taskbar simply ignored the setting for one of its
    // three values. Found by icons_test, which had been passing that
    // case for the wrong reason.
    enum start_button_mode want = START_BUTTON_BOTH;
    struct setting_msg msg;
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
    redraw_pending = 1;
    wm_damage_rect(0, screen_h - taskbar_h, screen_w, taskbar_h);
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

// Window i starts a group iff no earlier window shares its app. That
// makes the group order the windows[] order of each group's FIRST
// member, which is what keeps a button from jumping left along the
// strip every time one of its siblings is raised.
static int starts_group(int i) {
    if (windows[i].popup) return 0; // a menu is not a window the strip lists
    for (int j = 0; j < i; j++) if (same_app(i, j)) return 0;
    return 1;
}

// How many windows the strip has to fit -- popups are not among them.
static int listed_count(void) {
    int n = 0;
    for (int i = 0; i < window_count; i++) if (!windows[i].popup) n++;
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
    for (int j = i + 1; j < window_count; j++)
        if (!windows[j].popup && same_app(i, j)) front = j;
    return front;
}

static int group_size(int i) {
    int n = 1;
    for (int j = i + 1; j < window_count; j++)
        if (!windows[j].popup && same_app(i, j)) n++;
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
    int s = taskbar_h - 10;
    // Capped: the masters are 64px and a thick strip would otherwise
    // ask for an upscaled icon. 32 is Windows 11's in a 48px bar.
    if (s > TASKBAR_ICON_MAX) s = TASKBAR_ICON_MAX;
    return s < 8 ? 0 : s;
}

static void make_label(char *dst, int cap, const char *src, int w, int count) {
    int cw = ugfx_char_w();
    // 24 matches win_btn_w()'s own padding, so a full-width label sits
    // inside the button rather than touching its edges.
    int room = cw > 0 ? (w - 24) / cw : 0;
    if (room > cap - 1) room = cap - 1;
    if (room < 0) room = 0;

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

    int nameroom = room - sn;
    if (nameroom < 0) nameroom = 0;
    int n = 0;
    for (; src && src[n] && n < nameroom; n++) dst[n] = src[n];
    for (int k = 0; k < sn && n < room; k++) dst[n++] = suffix[k];
    dst[n] = '\0';
}

int taskbar_layout(struct taskbar_button *out, int max) {
    g_hidden = 0;
    int listed = listed_count();
    if (!out || max <= 0 || listed <= 0) { g_hidden = listed; return 0; }

    int x0 = 4 + start_btn_w() + 8;
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
    // count suffix (" (99+)"). Still capped by fit_width above, so this
    // can only spend space that is genuinely spare.
    //
    // THE ICON IS PART OF THE ALLOWANCE, and leaving it out meant the
    // widening bought nothing: make_label() is handed w MINUS the icon
    // square, so those five characters were spent on the icon and
    // "notepad (26)" still came out as "notep (26)". Reserved here from
    // the same taskbar_icon_size() the loop below subtracts, which is
    // the same "one source for a measurement" rule that pairs the icon
    // with the renderer.
    if (grouped) {
        int icon = taskbar_icon_size();
        int cap = natural + 5 * ugfx_char_w() + (icon ? icon + 4 : 0);
        int wide = fit_width(avail, n, cap);
        if (wide > w) w = wide;
    }

    int count = 0;
    for (int i = 0; i < window_count; i++) {
        if (windows[i].popup) continue;
        if (grouped && !starts_group(i)) continue;
        int x = x0 + count * (w + TB_GAP);
        int members = grouped ? group_size(i) : 1;
        if (count >= max || x + w > x1) { g_hidden += members; continue; }
        out[count].x = x;
        out[count].w = w;
        out[count].first = grouped ? group_front(i) : i;
        out[count].count = members;
        // A GROUPED button is named after the APPLICATION, an ungrouped
        // one after its window -- which is what Windows and KDE both
        // show, and the only naming that stays true when the group's
        // frontmost window changes underneath it. Falls back to the
        // title for a window whose client declared no app id.
        const char *name = windows[out[count].first].title;
        if (members > 1 && windows[out[count].first].app_id[0])
            name = windows[out[count].first].app_id;
        // The label gets what the ICON does not take. Reserved from the
        // same taskbar_icon_size() the renderer blits with, so the two
        // cannot disagree about where the text starts.
        out[count].icon = wm_window_icon_name(out[count].first);
        int label_w = w - (out[count].icon ? taskbar_icon_size() + 4 : 0);
        make_label(out[count].label, (int)sizeof out[count].label,
                    name, label_w, members);
        count++;
    }
    return count;
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
    if (windows[i].state == WIN_MINIMIZED) windows[i].state = WIN_NORMAL;
    wm_ensure_reachable(i);
    bring_to_front(i);
    redraw_pending = 1;
}

static void row_close(void *ctx) {
    int i = *(int *)ctx;
    if (i < 0 || i >= window_count) return;
    wm_request_close(i);
}

// Opens the list of windows a collapsed button stands for -- Windows'
// jump list, KDE's grouped-task popup. Anchored at the button's top-left
// on the taskbar; context_menu_open_at() lifts it clear of the strip.
static void open_group_menu(const struct taskbar_button *b) {
    int n = 0;
    for (int i = 0; i < window_count && n < TB_GROUP_ROWS; i++) {
        if (!same_app(b->first, i)) continue;
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
static void activate(int i) {
    if (windows[i].state == WIN_MINIMIZED) {
        windows[i].state = WIN_NORMAL;
        wm_ensure_reachable(i);
        bring_to_front(i);
    } else if (wm_ensure_reachable(i)) {
        // It was somewhere it could not be grabbed -- off an edge, or
        // behind the taskbar. RECOVERING it is the action, taking
        // priority over the minimize toggle below: minimizing something
        // the user cannot see does nothing they can perceive, and this
        // button is the only handle such a window has left.
        bring_to_front(i);
    } else if (i == wm_focus_index()) {
        windows[i].state = WIN_MINIMIZED;
    } else {
        bring_to_front(i);
    }
    redraw_pending = 1;
}

int taskbar_handle_click(int mx, int my) {
    static struct taskbar_button btns[TB_BUTTONS_MAX];
    int n = taskbar_layout(btns, TB_BUTTONS_MAX);
    int ty = screen_h - taskbar_h;
    for (int b = 0; b < n; b++) {
        if (!uui_hit(btns[b].x, ty, btns[b].w, taskbar_h, mx, my)) continue;
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
            g_row_target[0] = btns[b].first;
            g_rows[0].label = "Close window"; // a literal, so no copy into g_row_label
            g_rows[0].on_select = row_close;
            g_rows[0].ctx = &g_row_target[0];
            context_menu_open_at(mx, my, g_rows, 1);
        }
        return 1;
    }
    return 0;
}
