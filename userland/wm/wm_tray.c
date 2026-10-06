// See wm_tray.h for the WM-internal half of this API and wm.h for the
// app-facing half (tray_register()/tray_set_text()/tray_unregister()).
//
// A fixed array of slots rather than a linked list -- same "WM-global,
// small, fixed" shape as windows[] in wm_internal.h, and there's no
// real need for more items than fit in a taskbar strip anyway. Slot 0
// is always the built-in clock (registered by tray_init()); apps that
// register later get whatever slot is free, not necessarily in order,
// so `active` is what actually matters, not slot index continuity.
#include "wm_internal.h"
#include <time.h>
#include <locale.h>
#include "lib/udate.h"
#include "wm/wm_conf.h"   // wm_setting_generation()
#include "rt/sys.h"
#include "wm_tray.h"
#include "calendar_popup.h"   // its clock card ticks with the tray's
#include "lib/usetting.h"
#include "wm_taskbar.h"     // taskbar_h
#include "lib/icon_cache.h"
#include "kapi.h"
#include "wm_glass.h"
#include "ui/uui_primitives.h" // uui_state_bg(), uui_fill_round_rect()

// Eight: the clock, the on-screen keyboard, volume, brightness, the
// network and the remote-activity indicator, with room for the next
// one. A hidden item KEEPS its slot (see below), so this is the
// number of items that exist, not the number on screen.
#define TRAY_MAX_ITEMS 8

struct tray_item {
    int active;
    char text[TRAY_TEXT_MAX];
    // An ICON item instead of a text one: the name of an icon file
    // (icon_cache.h's rule -- a name, never a path), or NULL. Its width
    // is the icon's, so the right-to-left walk below needs no other
    // change. Only the panel's own items use this; an app still
    // registers text, which is all wm.h offers.
    const char *icon;
    // HIDDEN KEEPS ITS SLOT. The alternative, tray_unregister(), frees
    // it -- so an item that came back landed in whatever slot was free
    // and the strip silently reordered, since a slot index IS the
    // left-to-right position here. A visibility that can change at
    // runtime (`desktop.tray_*`, or the hardware behind an item
    // appearing) must not be able to move its neighbours.
    int hidden;
};

static struct tray_item tray_items[TRAY_MAX_ITEMS];
static int clock_tray_id = -1;
// The clock's second line, the short date in the locale's spelling.
static char clock_date[24];

// An item's box reaches this far past its content either side, and
// neighbouring boxes stand TRAY_GAP apart.
#define TRAY_PAD 8
#define TRAY_GAP 4

// This DOES damage the taskbar strip now. It deliberately didn't, for
// two stated reasons, and both have since stopped applying -- the
// lifted-constraint pattern this project keeps hitting.
//
// Reason one was real and still is: tray_init() runs before wm_run()'s
// loop, and damaging from there poisoned the very first frame's "no
// damage reported yet -- draw everything" fallback, narrowing it to the
// taskbar and leaving the desktop never drawn. That's handled by
// `tray_ready` below rather than by refusing to damage at all.
//
// Reason two has expired. It said the once-a-second full-screen repaint
// was load-bearing for the cursor's saved-pixels-underneath snapshot
// staying in sync -- an implicit resync. wm_render_frame() restores the
// cursor before repainting now (see wm_render.c), so nothing depends on
// that accidental resync any more.
//
// And leaving it unscoped had a cost that only became visible once
// there was a way to see it: on any frame where something ELSE reported
// damage, the clock's repaint landed outside that rect. `gui damage
// verify on` reported it immediately -- "93 px changed outside the
// damage rect, first at (1255,699)" -- which is the clock.
static int tray_ready;

static void tray_damage(void) {
    redraw_pending = 1;
    // Before the main loop exists there is nothing to scope, and
    // damaging here would narrow the first frame -- see above.
    if (tray_ready) wm_damage_rect(0, screen_h - taskbar_h, screen_w, taskbar_h);
}

static void tray_copy_text(char *dst, const char *src) {
    int i = 0;
    for (; src[i] && i < TRAY_TEXT_MAX - 1; i++) dst[i] = src[i];
    dst[i] = '\0';
}

// An ICON item, registered by the panel rather than by an app. Its
// slot is drawn from the same array and walked by the same loop -- the
// only difference is what fills its box.
int tray_register_icon(const char *icon) {
    int id = tray_register("");
    if (id >= 0) tray_items[id].icon = icon;
    return id;
}

// `desktop.tray_<key>` resolved against whether the hardware behind the
// item exists: `auto` asks the hardware, the other two outrank it.
// Unknown or unset reads as `auto`, which is the registry's default.
int tray_want_shown(const char *key, int hardware_present) {
    char name[SETTING_ABI_QUALIFIED_MAX];
    k_snprintf(name, sizeof name, "desktop.tray_%s", key);

    // usetting_get(), not sys_setting(): these are DECLARED settings
    // (/etc/settings.d), which the syscall alone cannot see.
    char value[SETTING_ABI_VALUE_MAX];
    if (usetting_get(name, value, sizeof value) && value[0]) {
        if (k_strcmp(value, TRAY_SHOW_ALWAYS) == 0) return 1;
        if (k_strcmp(value, TRAY_SHOW_NEVER) == 0) return 0;
    }
    return hardware_present;
}

void tray_set_hidden(int tray_id, int hidden) {
    if (tray_id < 0 || tray_id >= TRAY_MAX_ITEMS) return;
    if (!tray_items[tray_id].active) return;
    if (tray_items[tray_id].hidden == !!hidden) return;
    tray_items[tray_id].hidden = !!hidden;
    tray_damage(); // the strip's whole width moves, not just this box
}

int tray_is_hidden(int tray_id) {
    if (tray_id < 0 || tray_id >= TRAY_MAX_ITEMS) return 0;
    return tray_items[tray_id].hidden;
}

void tray_set_icon(int tray_id, const char *icon) {
    if (tray_id < 0 || tray_id >= TRAY_MAX_ITEMS) return;
    if (!tray_items[tray_id].active) return;
    if (tray_items[tray_id].icon == icon) return;   // nothing to repaint
    tray_items[tray_id].icon = icon;
    tray_damage();
}

int tray_register(const char *initial_text) {
    for (int i = 0; i < TRAY_MAX_ITEMS; i++) {
        if (tray_items[i].active) continue;
        tray_items[i].active = 1;
        tray_items[i].icon = 0;
        tray_copy_text(tray_items[i].text, initial_text);
        tray_damage();
        return i;
    }
    return -1;
}

void tray_set_text(int tray_id, const char *text) {
    if (tray_id < 0 || tray_id >= TRAY_MAX_ITEMS) return;
    if (!tray_items[tray_id].active) return;
    // The same text is no frame: the clock sets itself every second, and
    // without seconds it changes once a minute.
    char was[TRAY_TEXT_MAX];
    k_strlcpy(was, tray_items[tray_id].text, sizeof was);
    tray_copy_text(tray_items[tray_id].text, text);
    if (k_strcmp(was, tray_items[tray_id].text) == 0) return;
    tray_damage();
}

void tray_unregister(int tray_id) {
    if (tray_id < 0 || tray_id >= TRAY_MAX_ITEMS) return;
    if (!tray_items[tray_id].active) return;
    tray_items[tray_id].active = 0;
    tray_damage();
}

void tray_init(void) {
    tray_ready = 0;
    clock_tray_id = tray_register("00:00:00");
    tray_ready = 1; // from here on, scope the clock tick to the taskbar
}

// The compositor is not a uapp, so it follows the zone and the region
// itself: tzset() and setlocale() again whenever the settings registry
// moves -- one compare per call, the shape cursor_theme_poll() uses.
// libc caches both, so without this the clock would keep the old zone
// and spelling until the desktop restarted.
void wm_locale_sync(void) {
    static uint32_t seen_gen;
    static int primed;
    uint32_t gen = wm_setting_generation();
    if (primed && gen == seen_gen) return;
    primed = 1;
    seen_gen = gen;
    tzset();
    setlocale(LC_ALL, "");
}

// desktop.clock_seconds, re-read only when some setting has changed --
// this runs every second.
static int clock_seconds(void) {
    static uint32_t seen;
    static int on = 1;
    uint32_t gen = wm_setting_generation();
    if (gen != seen) {
        seen = gen;
        char v[SETTING_ABI_VALUE_MAX];
        on = !(usetting_get("desktop.clock_seconds", v, sizeof v) && k_strcmp(v, "off") == 0);
    }
    return on;
}

void tray_update_clock(void) {
    if (clock_tray_id < 0) return;
    wm_locale_sync();
    struct rtc_time t;
    sys_gettime(&t);   // UTC; udate_format() localises
    udate_format(clock_date, sizeof clock_date, &t, UDATE_DATE);
    char buf[TRAY_TEXT_MAX];
    udate_format(buf, sizeof buf, &t, UDATE_TIME | (clock_seconds() ? UDATE_SECONDS : 0));
    tray_set_text(clock_tray_id, buf);
    // The calendar's clock card ticks with this one.
    if (calendar_open) calendar_damage();
}

// The tray's icons run LARGER than a taskbar button's, because a tray
// icon is the whole item where a button's sits beside a label with the
// button's padding around it. Font-derived like everything else here
// (docs/gui-guidelines.md): the taskbar's height is, so this is.
static int tray_icon_size(void) {
    // 32 at the 48px default. The redesign's mockup drew 20, and on a
    // real 1920-wide panel that read as "really small" (2026-09-29).
    int s = taskbar_bar_h() * 2 / 3;
    if (s > TASKBAR_ICON_MAX + 4) s = TASKBAR_ICON_MAX + 4; // still larger than a button's
    return s < 8 ? 0 : s;
}

// The clock takes two lines -- time over date, as Windows 11's and
// Plasma's do -- whenever the buttons are tall enough to hold them.
//
// THE LINES MAY OVERLAP BY A QUARTER. A line box holds an accent above
// the capitals and a tail below the baseline, and digits and the date's
// separators use neither; demanding two full lines dropped the date at
// font size 18 on a 40 px button once the UI font took its full ascent.
static int clock_pitch(void) {
    struct taskbar_geom g;
    taskbar_geom(&g);
    int ch = ugfx_char_h();
    int pitch = g.btn_h - ch < ch ? g.btn_h - ch : ch;
    return pitch >= ch * 3 / 4 ? pitch : 0;
}

static int clock_two_lines(void) { return clock_date[0] && clock_pitch() > 0; }

static int item_width(int i) {
    if (tray_items[i].icon) return tray_icon_size();
    int w = ugfx_text_width(tray_items[i].text);
    if (i == clock_tray_id && clock_two_lines()) {
        int dw = ugfx_text_width(clock_date);
        if (dw > w) w = dw;
    }
    return w;
}

// THE one right-to-left walk. draw_tray(), tray_left() and
// tray_clock_rect() all used to be separate copies of this loop in
// spirit -- and the taskbar's own history says what that costs: window
// buttons laid out against a separately-guessed tray width ran under
// the clock (docs/bugs.md, fixed). `want` is the item whose rect the
// caller is after, or -1 for "just tell me where the strip ends";
// `visit` (nullable) is called for every active item in draw order.
// Returns the leftmost x the tray occupies.
static int tray_walk(int want, int *out_x, int *out_w,
                     void (*visit)(int id, int x, int w, void *ctx), void *ctx) {
    struct taskbar_geom g;
    taskbar_geom(&g);
    int cx = g.px + g.pw - 4 - TRAY_PAD;   // the last box 4px in, as Start is
    int left = cx;
    // THE CLOCK IS ALWAYS RIGHTMOST, whatever slot it holds. It used to
    // be leftmost by accident -- it takes slot 0 and the walk runs from
    // the highest slot down -- so the first item registered after it
    // (the volume icon) landed between the clock and the screen edge,
    // which is the one position no desktop puts a tray icon in. Windows,
    // KDE and GNOME all pin the clock to the end of the strip.
    for (int pass = 0; pass < 2; pass++)
    for (int i = TRAY_MAX_ITEMS - 1; i >= 0; i--) {
        if (!tray_items[i].active || tray_items[i].hidden) continue;
        if ((pass == 0) != (i == clock_tray_id)) continue;
        // Measured, never strlen * char_w: the face is proportional now.
        int text_w = item_width(i);
        cx -= text_w;
        // The item's BOX, not its text: the ground draw_tray() paints
        // runs TRAY_PAD either side of the content, and a click landing
        // in that padding is a click on the item.
        if (i == want) { if (out_x) *out_x = cx - TRAY_PAD; if (out_w) *out_w = text_w + 2 * TRAY_PAD; }
        if (visit) visit(i, cx, text_w, ctx);
        left = cx - TRAY_PAD;
        cx -= 2 * TRAY_PAD + TRAY_GAP;
    }
    return left;
}

int tray_left(void) { return tray_walk(-1, 0, 0, 0, 0); }

// --- hover and the pressed ground ----------------------------------------
//
// A tray item lights on HOVER and darker while HELD, as the strip's own
// buttons do -- Windows 11's and Plasma's rule. (It was press-only, the
// macOS menu-bar rule, until the taskbar redesign gave every control on
// the strip a hover.) Nothing latches: the ground clears on release even
// when the click opened a popup.
static int tray_hovered = -1;        // the item under the pointer, or -1
static int tray_pressed = -1;        // the item drawn pressed, or -1
static int tray_press_origin = -1;   // the item the button went down on
static uint8_t tray_prev_buttons;

struct tray_at_ctx { int mx, my, id; };

static void tray_at_visit(int id, int x, int w, void *vctx) {
    struct tray_at_ctx *c = (struct tray_at_ctx *)vctx;
    // The item's BOX, the same 4px-either-side padding draw_tray()
    // fills and tray_item_rect() reports -- asked of the one walk, so a
    // press cannot light up an item a click would miss.
    if (uui_hit(x - TRAY_PAD, screen_h - taskbar_h, w + 2 * TRAY_PAD, taskbar_h, c->mx, c->my))
        c->id = id;
}

static int tray_item_at(int mx, int my) {
    struct tray_at_ctx c = { mx, my, -1 };
    tray_walk(-1, 0, 0, tray_at_visit, &c);
    return c.id;
}

void tray_update_press(int mx, int my, uint8_t buttons) {
    int down = buttons & 0x1;
    if (down && !(tray_prev_buttons & 0x1)) tray_press_origin = tray_item_at(mx, my);
    if (!down) tray_press_origin = -1;
    tray_prev_buttons = buttons;

    // Armed on press, and dragged off its item it goes out again --
    // docs/gui-guidelines.md's rule for every control here. Nothing
    // arms while a fullscreen window covers the strip, which is the
    // same condition wm_handle_left_click() refuses the taskbar under.
    int under = wm_top_covers_screen() ? -1 : tray_item_at(mx, my);
    int now = -1;
    if (tray_press_origin >= 0 && under == tray_press_origin)
        now = tray_press_origin;
    // No hover while the button is down: a press dragged off its item
    // falls back to REST, not hover (docs/gui-guidelines.md).
    int hov = down ? -1 : under;
    if (now == tray_pressed && hov == tray_hovered) return;
    tray_pressed = now;
    tray_hovered = hov;
    tray_damage();
}

int tray_pressed_item(void) { return tray_pressed; }


// The clock's own box, for the two callers that need to know where it
// is rather than what it says: wm_input.c hit-tests a click against it,
// and calendar_popup.c anchors its panel to it. Both ask the walk above
// rather than re-deriving "the leftmost item, probably" -- the clock is
// item 0 and therefore leftmost TODAY, and an app registering a tray
// item does not change where the clock is drawn but would change any
// guess phrased that way.
int tray_item_rect(int tray_id, int *out_x, int *out_y, int *out_w, int *out_h) {
    if (tray_id < 0 || tray_id >= TRAY_MAX_ITEMS || !tray_items[tray_id].active)
        return 0;
    int x = 0, w = 0;
    tray_walk(tray_id, &x, &w, 0, 0);
    if (w <= 0) return 0;
    if (out_x) *out_x = x;
    if (out_y) *out_y = screen_h - taskbar_h;
    if (out_w) *out_w = w;
    if (out_h) *out_h = taskbar_h;
    return 1;
}

int tray_clock_rect(int *out_x, int *out_y, int *out_w, int *out_h) {
    if (clock_tray_id < 0) return 0;
    int x = 0, w = 0;
    tray_walk(clock_tray_id, &x, &w, 0, 0);
    if (w <= 0) return 0;
    if (out_x) *out_x = x;
    if (out_y) *out_y = screen_h - taskbar_h;
    if (out_w) *out_w = w;
    if (out_h) *out_h = taskbar_h;
    return 1;
}

struct tray_draw_ctx {
    const struct taskbar_geom *g;
    const struct taskbar_palette *p;
};

static void tray_draw_item(int id, int x, int w, void *vctx) {
    struct tray_draw_ctx *c = (struct tray_draw_ctx *)vctx;
    const struct taskbar_geom *g = c->g;
    const struct taskbar_palette *p = c->p;
    uint32_t bg = p->bar;
    // Derived from the strip's own colour, never a picked tint
    // (uui_state_bg()), as the window buttons' hover is.
    if (id == tray_pressed)      bg = uui_state_bg(p->bar, UUI_STATE_PRESSED);
    else if (id == tray_hovered) bg = p->hover;
    if (bg != p->bar)
        uui_fill_round_rect(wm_surface(), x - TRAY_PAD, g->btn_y, w + 2 * TRAY_PAD,
                            g->btn_h, g->btn_r, bg);
    if (tray_items[id].icon) {
        const struct uimg *ico = icon_get(tray_items[id].icon, w);
        // No letter-tile fallback here, unlike an app icon: a tray item
        // with no file draws NOTHING rather than a lone initial, which
        // would read as a control the panel invented.
        //
        // SYMBOLIC: drawn in the SAME ink as the clock beside it rather
        // than in its own, so it follows the panel instead of assuming
        // one. Every tray item today is the shell's own indicator; an
        // app-registered icon would have to say it is not symbolic.
        if (ico)
            ugfx_blit_tinted(wm_surface(), x, g->btn_y + (g->btn_h - ico->h) / 2,
                             ico->w, ico->h, ico->px, ico->w, p->text);
        return;
    }
    int ch = ugfx_char_h();
    // `bg`, not the strip's: the glyph cells are painted opaque, so a
    // lit item would otherwise punch the strip's colour back through
    // its own ground. On glass, an unlit item's text blends against it.
    bg = wm_glass_ink_bg(WM_GLASS_TASKBAR, bg, p->bar);
    if (id == clock_tray_id && clock_two_lines()) {
        int pitch = clock_pitch();
        int y = g->btn_y + (g->btn_h - pitch - ch) / 2;
        const char *t = tray_items[id].text;
        // Right-aligned, both lines, against the screen edge. The date
        // last: its cell's empty top may cover the time's empty tail.
        ugfx_draw_string_clipped(wm_surface(), x + w - ugfx_text_width(t), y, w, t, p->text, bg);
        ugfx_draw_string_clipped(wm_surface(), x + w - ugfx_text_width(clock_date), y + pitch, w,
                                 clock_date, p->dim, bg);
        return;
    }
    ugfx_draw_string_clipped(wm_surface(), x, g->btn_y + (g->btn_h - ch) / 2, w,
                             tray_items[id].text, p->text, bg);
}

void draw_tray(const struct taskbar_geom *g, const struct taskbar_palette *p) {
    struct tray_draw_ctx ctx = { g, p };
    tray_walk(-1, 0, 0, tray_draw_item, &ctx);
}
