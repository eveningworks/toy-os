// Everything the window manager draws -- window chrome, the taskbar,
// the cursor -- plus the shared layout metrics (title_buttons(),
// start_btn_w(), etc) that wm_input.c also needs for hit-testing the
// exact same regions this file draws. See wm.c's top comment for the
// overall split and wm_internal.h for the shared state. The Start
// menu POPUP's own drawing lives in start_menu.c now (see that file) --
// this file only draws the taskbar Start BUTTON that opens it, a
// separate piece of chrome.
#include "wm_internal.h"
#include "wm_idle.h"
#include "wm_dnd.h"
#include "wm_rawin.h"
#include "start_menu.h"
#include "context_menu.h"
#include "calendar_popup.h"
#include "volume_popup.h"
#include "wm_overlay.h"
#include "confirm_dialog.h"
#include "desktop.h"
#include "wm_tray.h"
#include "rt/sys.h"   // sys_monotonic_ns(), for the frame timer below
#include "wm_taskbar.h"
#include "wm_shadow.h"
#include "lib/icon_cache.h"
#include "cursor_theme.h"
#include "ui/uui.h"
#include "ui/utheme.h"
#include "kapi.h"
#include "wm/wm_log.h"

// The one screen this process composites into -- see wm_internal.h.
struct ugfx_screen g_wm_screen;

// THE START BUTTON'S MARK, AND ITS WIDTH, DERIVED FROM ONE ANSWER.
//
// Three functions could each have decided independently whether this
// button shows a mark -- the width, the drawing, and the debug report.
// They must not: a width that says "icon" while the drawing says "text"
// is a narrow button with a clipped word in it, which is exactly what
// happens when the artwork is missing from the disk and only one of
// them notices.
//
// So `start_mark()` is the single decision, and everything else asks
// it. NULL means "draw the word": `text` mode, or a mode that wanted a
// mark and could not have one.
static const struct uimg *start_mark(int *out_size) {
    int size = start_icon_size();          // 0 in text mode
    if (size <= 0) return 0;
    const struct uimg *ico = icon_get(START_ICON, size);
    if (!ico) return 0;                    // asked for, not on disk
    *out_size = size;
    return ico;
}

// THE START BUTTON'S WIDTH, and everything else on the strip starts to
// the right of it (wm_taskbar.c's layout) -- so this is where the three
// appearance modes turn into geometry.
//
// Measured, not multiplied, for the word: ugfx_text_width() rather than
// length * char_w, since a proportional face makes those disagree (see
// docs/conventions/gui.md's first GUI rule). The old form was wrong the
// day runtime fonts landed and merely looked right on a monospace one.
int start_btn_w(void) {
    int size = 0;
    const struct uimg *ico = start_mark(&size);
    int label_w = ugfx_text_width(START_LABEL);
    if (!ico) return label_w + 24;
    if (taskbar_start_mode() == START_BUTTON_ICON) {
        // A SQUARE, near enough -- one text button's worth of padding,
        // so an icon-only button still reads as a button rather than as
        // a picture floating on the strip.
        return size + 16;
    }
    return size + 5 + label_w + 24;
}

// WHERE THE MARK GOES, or NULL when the word is drawn instead -- the
// same one-function-for-both rule title_icon() states: this is what
// draw_taskbar() blits and what `gui taskbar --json` reports, so a test
// cannot re-derive it differently.
const struct uimg *start_icon(int *out_x, int *out_y, int *out_size) {
    int size = 0;
    const struct uimg *ico = start_mark(&size);
    if (!ico) return 0;
    // CENTRED when it is alone in the button, left-aligned when a word
    // follows it -- which is what makes `icon` read as a square button
    // and `both` read as a labelled one.
    *out_x = (taskbar_start_mode() == START_BUTTON_ICON)
                 ? 4 + (start_btn_w() - size) / 2 : 4 + 8;
    *out_y = screen_h - taskbar_h + 5;
    *out_size = size;
    return ico;
}

// A button's natural width: the label's characters, the padding, AND
// the icon column -- make_label() subtracts that column from the width
// it is handed, so leaving it out here spent the label on the icon
// ("untitled" drew as "un" once the strip, and so the icon, grew).
int win_btn_w(void) {
    int icon = taskbar_icon_size();
    // Reserved in a REPRESENTATIVE glyph, not the widest one -- this is
    // a budget for a title nobody has seen yet, and `char_w` makes it
    // nearly twice what ordinary text needs, so fewer buttons fit
    // before taskbar_layout() starts shrinking and grouping them.
    int per = ugfx_char_advance('n');
    if (per <= 0) per = ugfx_char_w();
    return WIN_LABEL_MAX_CHARS * per + 24 + (icon ? icon + 4 : 0);
}

// The minimize/maximize/close title-bar buttons used to be a fixed
// BTN_SIZE 18 -- fine back when the font was a fixed 16x16 cell, but once
// the font became runtime-selectable (11x22 up to 20x40) that stopped
// tracking anything. The close button in particular drew a full font
// glyph ('x') inside that fixed 18px box, so it clipped/overflowed badly
// at medium and large sizes. Deriving the button size from WM_TITLEBAR_H
// (itself already font-aware) keeps all three buttons proportional to
// whatever font is active, same fix as the taskbar buttons above.
int btn_size(void) {
    int s = WM_TITLEBAR_H - 6;
    return s < 14 ? 14 : s;
}

// Which visual state a title-bar button is in. Encodes the rule
// docs/gui-guidelines.md states for every armed control: a button that
// has been pressed and then dragged off falls back to REST, NOT hover
// -- something about to be cancelled must not look like it is still
// being interacted with, even though the cursor is elsewhere and the
// button is technically still armed.
static enum uui_state title_btn_state(int kind, int is_armed, int is_hovered) {
    if (is_armed && title_btn_armed_kind == kind) {
        return title_btn_pressed_active ? UUI_STATE_PRESSED : UUI_STATE_REST;
    }
    if (is_armed) return UUI_STATE_REST; // a different button on this window is armed
    return (is_hovered && title_hover_kind == kind) ? UUI_STATE_HOVER : UUI_STATE_REST;
}

struct btn_rects title_buttons(const struct window *win) {
    struct btn_rects r;
    r.size = btn_size();
    int gap = 4, margin = 6;
    r.y = win->y + (WM_TITLEBAR_H - r.size) / 2;
    r.close_x = win->x + win->w - margin - r.size;
    r.max_x = r.close_x - gap - r.size;
    r.min_x = r.max_x - gap - r.size;
    return r;
}

// See wm_internal.h. Two pixels smaller than a title-bar button so the
// artwork sits INSIDE the bar rather than filling it edge to edge, and
// centred on the same basis title_buttons() uses so the left and right
// ends of the bar line up.
//
// A floor rather than a clamp at the small end: below ~10px a 64x64
// icon box-filtered down is a smudge, and no icon at all reads better
// than a wrong-looking one. The title simply starts where it always
// did in that case.
const struct uimg *title_icon(int idx, int *out_x, int *out_y, int *out_size) {
    if (idx < 0 || idx >= window_count) return 0;
    int size = WM_TITLEBAR_H - 8;
    if (size < 10) return 0;
    const char *name = wm_window_icon_name(idx);
    if (!name) return 0;
    const struct uimg *ico = icon_get(name, size);
    if (!ico) return 0;
    const struct window *win = &windows[idx];
    *out_x = win->x + 5;
    *out_y = win->y + (WM_TITLEBAR_H - size) / 2;
    *out_size = size;
    return ico;
}

// Hand-drawn diagonal X, sized to fit inside a button/icon area of
// size x size with a small margin. Uses a thickened line (a few
// parallel diagonals) so it stays visible at both the small 14-18px
// buttons and the larger ones on bigger font sizes, without needing
// any glyph/font metrics.
// --- title-bar buttons ------------------------------------------------
//
// A DISC PER BUTTON, with the glyph on top. Adwaita's shape, and the
// reason it survives at this size is that a circle has no corners to
// alias -- a small filled rectangle with a 1px outline reads as a
// smudge, which is what the old square buttons did.
//
// The colours are DERIVED, not picked: the disc is the same `btnbg` the
// square used, and hover and pressed come from uui_state_bg() shifting
// it. That is what let the close button lose its permanent red without
// anybody choosing a new palette -- it is a grey disc like the others
// until you hover it, which is where the red belongs. A button that is
// red before you touch it spends the whole session shouting about the
// one action you least want to take by accident.

// One stroke weight for all three glyphs, derived from the button so it
// tracks the font. They were 2px, 1px and hand-drawn before, which is
// the sort of thing that reads as "unfinished" without being nameable.
static int btn_stroke(int size) {
    // THIN, and thin is right here rather than a compromise. At a
    // 14-pixel button a 2px wall around a 6px square leaves a 2px hole,
    // so the maximize glyph stops reading as an outline and becomes a
    // blob. Adwaita, Breeze and the Segoe MDL2 set are all hairline at
    // this size for the same reason. It thickens on a big font, where a
    // 1px line would disappear.
    int t = size / 12;
    return t < 1 ? 1 : t;
}

// **ONE CENTRE FOR THE DISC AND EVERY GLYPH, AND ODD EXTENTS AROUND
// IT.** This is the fix for glyphs that looked a shade off inside their
// buttons, and the cause is worth stating because it is easy to
// reintroduce: a filled circle centres on a PIXEL (cx, cy), while a
// rectangle placed at `(size - w) / 2` centres on a SPAN -- and those
// two agree only when the span is even. At an 18px button the glyphs
// sat half a pixel left of the disc, and the minimize bar sat a whole
// row above it at every size, because `(size - t) / 2` with t = 1 is
// always short of the middle.
//
// So every glyph below is drawn from cx - h to cx + h INCLUSIVE, which
// is 2h+1 pixels wide and cannot be off-centre at any size or any font.
static int btn_cx(int x, int size) { return x + size / 2; }
static int btn_cy(int y, int size) { return y + size / 2; }

// A bar of `thick` pixels centred on `cy`, spanning cx-h..cx+h.
static void bar_h(int cx, int cy, int h, int thick, uint32_t color) {
    ugfx_fill_rect(wm_surface(), cx - h, cy - thick / 2, 2 * h + 1, thick, color);
}

static void bar_v(int cx, int cy, int h, int thick, uint32_t color) {
    ugfx_fill_rect(wm_surface(), cx - thick / 2, cy - h, thick, 2 * h + 1, color);
}

// The disc. Inset by a pixel so neighbouring buttons never touch, which
// at this spacing is what makes three of them read as three.
static void draw_btn_disc(int x, int y, int size, uint32_t bg) {
    int r = size / 2 - 1;
    if (r < 3) r = 3;
    ugfx_fill_circle(wm_surface(), btn_cx(x, size), btn_cy(y, size), r, bg);
    // AND AN ANTI-ALIASED OUTLINE OVER THE SAME CIRCLE. The fill's edge
    // is a midpoint rasteriser's, which leaves a single-pixel spur at
    // each of the four cardinal points -- at this size that does not
    // read as a rough circle, it reads as a COG. Stroking the same
    // radius with aa smooths those away, and it is one call rather than
    // a second rasteriser.
    ugfx_draw_circle(wm_surface(), btn_cx(x, size), btn_cy(y, size), r, bg, GEOM_AA);
}

// How far a glyph reaches from the centre. A quarter of the button, so
// the glyph is about half its width and clears the disc's edge with the
// stroke counted.
static int glyph_h(int size) {
    int h = size / 4;
    return h < 2 ? 2 : h;
}

static void draw_min_icon(int x, int y, int size, uint32_t color) {
    bar_h(btn_cx(x, size), btn_cy(y, size), glyph_h(size), btn_stroke(size), color);
}

// Maximize: a square outline at the same stroke weight, drawn as four
// bars rather than with ugfx_draw_rect() -- that is 1px and would be
// thinner than the other glyphs on a large font.
static void draw_max_icon(int x, int y, int size, uint32_t color) {
    int cx = btn_cx(x, size), cy = btn_cy(y, size);
    int h = glyph_h(size), t = btn_stroke(size);
    bar_h(cx, cy - h, h, t, color);
    bar_h(cx, cy + h, h, t, color);
    bar_v(cx - h, cy, h, t, color);
    bar_v(cx + h, cy, h, t, color);
}

// Restore, shown in the maximize button's place WHILE MAXIMIZED: two
// overlapping squares, Windows' and Breeze's glyph, so the button says
// what the next click does.
static void draw_restore_icon(int x, int y, int size, uint32_t color) {
    int cx = btn_cx(x, size), cy = btn_cy(y, size);
    int h = glyph_h(size), t = btn_stroke(size);
    int o = (h + 1) / 2;         // the two squares are offset by this
    int q = h - o / 2;           // each square's half-extent
    int fx = cx - o / 2, fy = cy + o / 2;   // front: down-left
    int bx = cx + o / 2, by = cy - o / 2;   // back: up-right
    // The back square shows only its top and right edges; the rest is
    // behind the front one.
    bar_h(bx, by - q, q, t, color);
    bar_v(bx + q, by, q, t, color);
    bar_h(fx, fy - q, q, t, color);
    bar_h(fx, fy + q, q, t, color);
    bar_v(fx - q, fy, q, t, color);
    bar_v(fx + q, fy, q, t, color);
}

static void draw_close_icon(int x, int y, int size, uint32_t color) {
    // TWO ANTI-ALIASED DIAGONALS about the same centre as everything
    // else. It was a hand-rolled loop plotting pixels along both
    // diagonals, which is a staircase -- and it sat next to a disc whose
    // edge IS anti-aliased, so the one jagged thing on the button was
    // the glyph. A stroke wider than a pixel is drawn as parallel lines,
    // because geom has no thick-line primitive and two calls are cheaper
    // than one.
    int cx = btn_cx(x, size), cy = btn_cy(y, size);
    int h = glyph_h(size), t = btn_stroke(size);
    for (int o = -(t / 2); o <= t / 2; o++) {
        ugfx_draw_line(wm_surface(), cx - h + o, cy - h, cx + h + o, cy + h,
                        color, GEOM_AA);
        ugfx_draw_line(wm_surface(), cx + h + o, cy - h, cx - h + o, cy + h,
                        color, GEOM_AA);
    }
}

// Anti-aliased arrow cursor -- two baked 8-bit alpha masks (outline in
// black, fill in white), the same "baked alpha, blended per-pixel"
// approach font_ttf.h's glyphs use (see gfx_draw_char()), just for a
// one-off 13x19 sprite instead of a whole font -- not worth a new
// tools/gen_*.py baking step for a single asset, so these are pasted
// literal data, generated once with PIL (supersampled polygon fill +
// dilate/erode for the outline ring, downsampled to this size) rather
// than hand-drawn pixel by pixel. Replaces the old hard-edged
// staircase shape (a capped `row+1` triangle, no anti-aliasing at all)
// -- see docs/decisions.md and the commit that added it for
// the "why" and the before/after screenshots.
#define CURSOR_SPRITE_W 13
#define CURSOR_SPRITE_H 19

static const unsigned char cursor_outline_alpha[CURSOR_SPRITE_H][CURSOR_SPRITE_W] = {
    {255,136,0,0,0,0,0,0,0,0,0,0,0},
    {255,255,126,0,0,0,0,0,0,0,0,0,0},
    {255,130,255,120,0,0,0,0,0,0,0,0,0},
    {255,0,136,255,120,0,0,0,0,0,0,0,0},
    {255,0,0,136,255,105,0,0,0,0,0,0,0},
    {255,0,0,0,150,254,105,0,0,0,0,0,0},
    {255,0,0,0,1,150,254,101,0,0,0,0,0},
    {255,0,0,0,0,1,154,252,91,0,0,0,0},
    {255,0,0,0,0,0,3,164,252,91,0,0,0},
    {255,0,0,0,0,0,0,3,164,252,82,0,0},
    {255,0,0,0,0,9,48,48,51,206,249,72,0},
    {255,0,0,17,20,46,255,221,207,207,207,116,0},
    {255,0,59,238,216,3,223,137,0,0,0,0,0},
    {255,76,245,206,254,64,119,235,8,0,0,0,0},
    {255,251,179,10,191,164,20,247,96,0,0,0,0},
    {255,159,4,0,91,247,18,159,203,0,0,0,0},
    {16,1,0,0,8,237,127,148,255,28,0,0,0},
    {0,0,0,0,0,143,255,239,140,10,0,0,0},
    {0,0,0,0,0,24,76,12,0,0,0,0,0},
};

static const unsigned char cursor_fill_alpha[CURSOR_SPRITE_H][CURSOR_SPRITE_W] = {
    {0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,125,0,0,0,0,0,0,0,0,0,0,0},
    {0,255,119,0,0,0,0,0,0,0,0,0,0},
    {0,255,255,119,0,0,0,0,0,0,0,0,0},
    {0,255,255,255,104,0,0,0,0,0,0,0,0},
    {0,255,255,255,254,104,0,0,0,0,0,0,0},
    {0,255,255,255,255,254,100,0,0,0,0,0,0},
    {0,255,255,255,255,255,252,88,0,0,0,0,0},
    {0,255,255,255,255,255,255,252,88,0,0,0,0},
    {0,255,255,255,255,246,207,207,204,43,0,0,0},
    {0,255,255,238,235,209,0,0,0,0,0,0,0},
    {0,255,196,0,3,252,0,0,0,0,0,0,0},
    {0,179,0,0,0,191,136,0,0,0,0,0,0},
    {0,0,0,0,0,84,235,0,0,0,0,0,0},
    {0,0,0,0,0,0,237,91,0,0,0,0,0},
    {0,0,0,0,0,0,128,107,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0},
};

void wm_builtin_arrow_masks(const unsigned char **outline,
                             const unsigned char **fill,
                             int *w, int *h, int *stride) {
    *outline = &cursor_outline_alpha[0][0];
    *fill = &cursor_fill_alpha[0][0];
    *w = CURSOR_SPRITE_W;
    *h = CURSOR_SPRITE_H;
    *stride = CURSOR_SPRITE_W;
}

// WHERE THE POINTER IS DRAWN. Normally the back buffer, like everything
// else here; wm_render_cursor_into() points it at a caller's surface for
// one call. That exists because a pointer riding the HARDWARE cursor
// plane is not in the back buffer at all, so a screenshot taken on a
// machine that has one would silently have no pointer in it.
// A CLIENT THAT ASKED NOT TO BE IN THE PICTURE. Set for the one frame a
// WIN_SHOT_NO_SELF capture renders, so the screenshot tool is not in its
// own screenshot -- which is the visible behaviour of every screenshot
// tool there is: the window goes away, the shot is taken, it comes back.
// See wm_screenshot.c, the only caller.
static int g_hidden_pid;
int  wm_render_hidden_pid(void)      { return g_hidden_pid; }
void wm_render_hide_pid(int pid)     { g_hidden_pid = pid; }

static struct ugfx_surface *g_cursor_dst;
static struct ugfx_surface *cursor_surface(void) {
    return g_cursor_dst ? g_cursor_dst : wm_surface();
}

static void draw_cursor_normal(int x, int y) {
    uint32_t fill = UTHEME_WHITE, outline = ugfx_rgb(0, 0, 0);
    // Outline first, fill on top -- matches how the two masks were
    // baked (the outline ring sits where fill was subtracted out, so
    // drawing fill second never re-covers outline-only pixels, and
    // pixels both masks touch get the fill's fully-opaque top coat).
    for (int row = 0; row < CURSOR_SPRITE_H; row++) {
        for (int col = 0; col < CURSOR_SPRITE_W; col++) {
            ugfx_blend_pixel(cursor_surface(), x + col, y + row, outline, cursor_outline_alpha[row][col]);
        }
    }
    for (int row = 0; row < CURSOR_SPRITE_H; row++) {
        for (int col = 0; col < CURSOR_SPRITE_W; col++) {
            ugfx_blend_pixel(cursor_surface(), x + col, y + row, fill, cursor_fill_alpha[row][col]);
        }
    }
}

// Directional resize cursors, shown while hovering (or actively
// dragging) a resizable window's edge/corner -- see
// wm_find_resize_zone() (wm_input.c). Same construction as each other
// (and the same "triangle wedge with an outline on its slanted edges"
// style as draw_cursor_normal() above): a short double-headed wedge
// shape, tapering from a single-pixel tip at each end up to a 3px-wide
// base in the middle, connected by a thin 1px shaft.
// THE RESIZE ARROW, ONE PROFILE FOR ALL THREE DIRECTIONS, and the same
// arithmetic as tools/gen_cursors.py's resize_shape() -- change both.
//
// Rasterised ANALYTICALLY rather than stepped along the axis. A stepped
// diagonal sets only the pixels whose x+y is even, so the corner cursor
// came out a CHECKERBOARD and read as bigger and different from the edge
// ones; scanning every pixel and asking how far ALONG the arrow (u) and
// how far ACROSS it (v) it lies is solid in any direction, and makes the
// three one arrow rotated. Fixed point, 256 = 1 px; RZ_DIAG is
// 256/sqrt(2), which is what makes a diagonal arrow 19 px long too
// rather than 27.
#define RZ_S     256
#define RZ_LEN   (19 * RZ_S)   // tip to tip
#define RZ_HEAD  (5 * RZ_S)    // of which each arrowhead
#define RZ_HW    1152          // 4.5 px: the head's half-width at its base
#define RZ_SW    384           // 1.5 px: the shaft's
#define RZ_DIAG  181
#define RZ_GRID  34            // comfortably over the longest extent

// Half the arrow's width at `u` along it, or -1 past either end.
static int rz_halfwidth(int u) {
    if (u < 0 || u > RZ_LEN) return -1;
    if (u < RZ_HEAD) return RZ_HW * u / RZ_HEAD;
    if (u > RZ_LEN - RZ_HEAD) return RZ_HW * (RZ_LEN - u) / RZ_HEAD;
    return RZ_SW;
}

// Rasterised into a grid rather than plotted straight out, because the
// outline is "every empty pixel touching a filled one" and that cannot
// be decided until the fill is complete.
static unsigned char rz_grid[RZ_GRID][RZ_GRID];

static void draw_resize_cursor(int x, int y, enum wm_cursor_kind kind) {
    int c = RZ_GRID / 2;
    for (int gy = 0; gy < RZ_GRID; gy++) {
        for (int gx = 0; gx < RZ_GRID; gx++) {
            int dx = gx - c, dy = gy - c, u, v;
            if (kind == WM_CURSOR_H)      { u = dx * RZ_S + RZ_LEN / 2; v = dy * RZ_S; }
            else if (kind == WM_CURSOR_V) { u = dy * RZ_S + RZ_LEN / 2; v = dx * RZ_S; }
            else if (kind == WM_CURSOR_DIAG) { u = (dx + dy) * RZ_DIAG + RZ_LEN / 2; v = (dx - dy) * RZ_DIAG; }
            else { u = (dx - dy) * RZ_DIAG + RZ_LEN / 2; v = (dx + dy) * RZ_DIAG; }
            int hw = rz_halfwidth(u);
            int av = v < 0 ? -v : v;
            rz_grid[gy][gx] = (hw >= 0 && av <= hw + RZ_S / 2) ? 1 : 0;
        }
    }
    for (int gy = 0; gy < RZ_GRID; gy++) {
        for (int gx = 0; gx < RZ_GRID; gx++) {
            if (rz_grid[gy][gx]) continue;
            int near = 0;
            for (int dy = -1; dy <= 1 && !near; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int ny = gy + dy, nx = gx + dx;
                    if (ny < 0 || ny >= RZ_GRID || nx < 0 || nx >= RZ_GRID) continue;
                    if (rz_grid[ny][nx] == 1) { near = 1; break; }
                }
            if (near) rz_grid[gy][gx] = 2;
        }
    }

    // CENTRED ON THE HOTSPOT, as the I-beam is and unlike the arrow: a
    // resize arrow has to straddle the edge it is grabbing.
    uint32_t fill = UTHEME_WHITE, outline = ugfx_rgb(0, 0, 0);
    for (int gy = 0; gy < RZ_GRID; gy++)
        for (int gx = 0; gx < RZ_GRID; gx++)
            if (rz_grid[gy][gx])
                ugfx_put_pixel(cursor_surface(), x + gx - c, y + gy - c,
                               rz_grid[gy][gx] == 1 ? fill : outline);
}

// The built-in I-beam, for a theme with no `text` shape.
//
// CENTRED ON THE HOTSPOT, unlike the three above, which draw from their
// anchor down and right. cursor_rect() subtracts the same
// CURSOR_TEXT_HOT_X/Y; if the two disagree a move strands the sprite.
#define CURSOR_TEXT_W     7
#define CURSOR_TEXT_H     17
#define CURSOR_TEXT_HOT_X 3
#define CURSOR_TEXT_HOT_Y 8

static void draw_cursor_text(int x, int y) {
    uint32_t color = UTHEME_WHITE, outline = ugfx_rgb(0, 0, 0);
    int ox = x - CURSOR_TEXT_HOT_X, oy = y - CURSOR_TEXT_HOT_Y;
    // Outline one pixel out all round, so the bar stays visible over
    // white paper as well as over dark text.
    for (int j = 0; j < CURSOR_TEXT_H; j++) {
        int serif = (j <= 1 || j >= CURSOR_TEXT_H - 2);
        int x0 = serif ? 0 : CURSOR_TEXT_HOT_X - 1;
        int x1 = serif ? CURSOR_TEXT_W - 1 : CURSOR_TEXT_HOT_X + 1;
        for (int i = x0; i <= x1; i++)
            ugfx_put_pixel(cursor_surface(), ox + i, oy + j, outline);
    }
    // Fill: the shaft, plus the one-pixel serif caps inside the outline.
    for (int j = 1; j < CURSOR_TEXT_H - 1; j++)
        ugfx_put_pixel(cursor_surface(), ox + CURSOR_TEXT_HOT_X, oy + j, color);
    for (int i = 1; i < CURSOR_TEXT_W - 1; i++) {
        ugfx_put_pixel(cursor_surface(), ox + i, oy + 1, color);
        ugfx_put_pixel(cursor_surface(), ox + i, oy + CURSOR_TEXT_H - 2, color);
    }
}

// The built-in busy pointer, for a theme with no `wait` shape. Centred
// on its hotspot like the I-beam, not anchored like the wedges.
#define CURSOR_WAIT_W     11
#define CURSOR_WAIT_H     15
#define CURSOR_WAIT_HOT_X 5
#define CURSOR_WAIT_HOT_Y 7

// Half-width of the glass at row j: 5 at the waist, tapering to 2 at
// the bars. Matches the shipped theme's silhouette so the fallback and
// the theme read as the same object.
static int wait_half(int j) {
    int d = j - CURSOR_WAIT_HOT_Y;
    if (d < 0) d = -d;
    return 5 - (d >= 2) - (d >= 4) - (d >= 6);
}

static void draw_cursor_wait(int x, int y) {
    uint32_t color = UTHEME_WHITE, outline = ugfx_rgb(0, 0, 0);
    int cx = x, oy = y - CURSOR_WAIT_HOT_Y;
    for (int j = 0; j < CURSOR_WAIT_H; j++) {
        if (j == 0 || j == CURSOR_WAIT_H - 1) { // the end bars
            for (int i = -4; i <= 4; i++)
                ugfx_put_pixel(cursor_surface(), cx + i, oy + j, outline);
            continue;
        }
        int h = wait_half(j);
        ugfx_put_pixel(cursor_surface(), cx - h, oy + j, outline);
        ugfx_put_pixel(cursor_surface(), cx + h, oy + j, outline);
        for (int i = -(h - 1); i <= h - 1; i++)
            ugfx_put_pixel(cursor_surface(), cx + i, oy + j, color);
    }
}

// A themed shape: two coverage masks, coloured here rather than in the
// file, scaled by whole multiples. Outline first and fill over it, the
// same order draw_cursor_normal() uses and for the same reason -- a
// pixel both masks touch ends up the fill's colour.
//
// Nearest-neighbour at integer scales only: a pointer wants a hard
// edge, and a smoothly scaled mask reads as blurry rather than large.
static void draw_cursor_themed(const struct cursor_shape *s, int x, int y,
                                int scale) {
    uint32_t fill = UTHEME_WHITE, outline = ugfx_rgb(0, 0, 0);
    int ox = x - s->hot_x * scale, oy = y - s->hot_y * scale;
    for (int row = 0; row < s->h; row++) {
        for (int col = 0; col < s->w; col++) {
            unsigned char a = s->outline[row][col];
            if (!a) continue;
            for (int j = 0; j < scale; j++)
                for (int i = 0; i < scale; i++)
                    ugfx_blend_pixel(cursor_surface(), ox + col * scale + i, oy + row * scale + j,
                                     outline, a);
        }
    }
    for (int row = 0; row < s->h; row++) {
        for (int col = 0; col < s->w; col++) {
            unsigned char a = s->fill[row][col];
            if (!a) continue;
            for (int j = 0; j < scale; j++)
                for (int i = 0; i < scale; i++)
                    ugfx_blend_pixel(cursor_surface(), ox + col * scale + i, oy + row * scale + j,
                                     fill, a);
        }
    }
}

static void draw_cursor(int x, int y, enum wm_cursor_kind kind) {
    // A theme shape when one loaded, the built-in otherwise. That
    // either/or is the vendor-default/override pattern /etc already
    // uses: a missing or malformed theme file degrades to a working
    // pointer rather than to no pointer, which is the one failure this
    // must not have.
    const struct cursor_shape *s = cursor_theme_shape(kind);
    if (s) {
        draw_cursor_themed(s, x, y, cursor_theme_scale());
        return;
    }
    switch (kind) {
        case WM_CURSOR_H:
        case WM_CURSOR_V:
        case WM_CURSOR_DIAG:
        case WM_CURSOR_DIAG2: draw_resize_cursor(x, y, kind); break;
        case WM_CURSOR_TEXT: draw_cursor_text(x, y); break;
        case WM_CURSOR_WAIT: draw_cursor_wait(x, y); break;
        default:              draw_cursor_normal(x, y); break;
    }
}

// Which cursor shape to show at (mx, my) right now -- shared by the full
// frame path and the cursor-only-moved path below, so hovering a resize
// edge shows the right cursor either way. While actively dragging a
// resize, keep showing the cursor for whichever edge(s) that drag started
// on (don't re-query by position -- the mouse may have moved past the
// window's edge mid-drag); otherwise ask the same hit-test the click
// handler uses (wm_find_resize_zone(), wm_input.c) so hovering shows the
// cursor before you click, not just while dragging.
// The client's say-so, CLAMPED to what the client owns: its content
// area, topmost window only, never under the taskbar or a popup.
//
// The clamp is the safety property. A client is a process; a wedged one
// never answers, so the shape it last named outlives the pointer being
// over it. Bounding it costs one hit-test already done for input
// routing, and turns "stranded over the desktop" into "wrong inside one
// window until the pointer crosses a boundary".
static enum wm_cursor_kind client_cursor_at(int mx, int my) {
    if (start_menu_open || context_menu_open || calendar_open ||
        confirm_dialog_open) return WM_CURSOR_NORMAL;
    if (my >= screen_h - taskbar_h) return WM_CURSOR_NORMAL;

    for (int i = window_count - 1; i >= 0; i--) {
        struct window *w = &windows[i];
        if (w->state == WIN_MINIMIZED) continue;
        if (!uui_hit(w->x, w->y, w->w, w->h, mx, my)) continue;
        // Topmost hit wins EITHER WAY: the search stops whether or not
        // this window had anything to say, or a buried text field would
        // show its I-beam through the window covering it.
        if (!uui_hit(window_content_x(w), window_content_y(w),
                     window_content_w(w), window_content_h(w), mx, my)) break;
        // NOT ANSWERING OUTRANKS WHATEVER IT LAST SAID. A wedged client
        // cannot name a shape -- naming one needs the event loop that
        // is wedged -- so this is the case only the compositor can
        // report, and it must override a stale TEXT.
        if (w->not_responding) return WM_CURSOR_WAIT;
        switch (w->client_cursor) {
        case WIN_CURSOR_TEXT:     return WM_CURSOR_TEXT;
        case WIN_CURSOR_WAIT:     return WM_CURSOR_WAIT;
        // The theme's own resize shapes, which the frame already draws
        // for its edges -- a client with a splitter in it asks for the
        // same one rather than for a second drawing of the same idea.
        case WIN_CURSOR_RESIZE_H: return WM_CURSOR_H;
        case WIN_CURSOR_RESIZE_V: return WM_CURSOR_V;
        default: break;
        }
        break;
    }
    return WM_CURSOR_NORMAL;
}

static enum wm_cursor_kind resolve_cursor_kind(int mx, int my) {
    int edges = 0;
    if (resizing >= 0) edges = resize_edges;
    else if (wm_find_resize_zone(mx, my, &edges) < 0) edges = 0;

    // Which DIAGONAL a corner gets is the direction the drag runs in:
    // top-left and bottom-right both lie on the \ axis, the other two
    // on the /.
    int left = edges & WM_EDGE_LEFT, right = edges & WM_EDGE_RIGHT;
    int top = edges & WM_EDGE_TOP, bottom = edges & WM_EDGE_BOTTOM;
    if ((left && top) || (right && bottom)) return WM_CURSOR_DIAG;
    if ((right && top) || (left && bottom)) return WM_CURSOR_DIAG2;
    if (left || right) return WM_CURSOR_H;
    if (top || bottom) return WM_CURSOR_V;
    // Frame first: a resize edge is geometry the compositor owns, and
    // wins over anything the client inside asked for.
    return client_cursor_at(mx, my);
}

enum wm_cursor_kind wm_cursor_kind_at(int mx, int my) {
    return resolve_cursor_kind(mx, my);
}

// Draws the pointer into `dst`, whose top-left sits at (ox, oy) on the
// screen. The shape is resolved from the real pointer position, so a
// capture shows the same cursor the screen does.
void wm_render_cursor_into(struct ugfx_surface *dst, int ox, int oy) {
    int mx, my;
    uint8_t buttons;
    wm_rawin_mouse(&mx, &my, &buttons);
    g_cursor_dst = dst;
    draw_cursor(mx - ox, my - oy, resolve_cursor_kind(mx, my));
    g_cursor_dst = NULL;
}


// ---- cursor sprite save/restore ----
//
// Used only by wm_render_cursor_move() (the cheap "mouse moved, nothing
// else changed" path) to avoid a full-scene redraw for the single most
// common event this loop sees. Classic sprite trick: before drawing the
// cursor somewhere, save the back-buffer pixels it's about to overwrite;
// next time the cursor moves, restore exactly those pixels (undrawing the
// cursor) before drawing it at the new spot. wm_render_frame() (the full
// path) also goes through draw_cursor_at() so this stays correct across
// both paths -- a cursor-only move after a full repaint restores exactly
// what that repaint actually drew underneath, not stale content.
//
// CURSOR_BOX is anchored 2px above/left of the cursor's own (x,y) and
// sized generously to comfortably cover all four hand-drawn cursor
// shapes above, including draw_cursor_h()/draw_cursor_v()'s asymmetric
// bounding boxes (they draw up to 1px above/left of their own anchor --
// see their own comments) and draw_cursor_normal()'s sprite (13x19,
// drawn from the anchor down/right -- the tallest shape here, hence
// this needing to be taller than it is wide) -- getting this too small
// would leave a stale cursor-colored pixel behind on every move, so
// it's deliberately oversized rather than tightly fit to each shape.
#define CURSOR_BOX_MARGIN 2
#define CURSOR_BOX_SIZE 22 // the BUILT-IN shapes' box; themed ones derive theirs

// The themed shapes make the drawn extent variable -- a theme's size,
// its hotspot and the size setting's scale all move it -- so the box can
// no longer be one constant. It is DERIVED from whatever is actually
// going to be drawn, and every consumer (the save/restore pair and the
// damage rect) asks the same function. That is the invariant: if these
// two ever disagree, a cursor move leaves a stale sprite behind, which
// is a bug this file's comments already record paying for twice.
#define CURSOR_UNDER_MAX (CURSOR_SHAPE_MAX * 3 + 2 * CURSOR_BOX_MARGIN)

static void cursor_rect(enum wm_cursor_kind kind, int x, int y,
                         int *ox, int *oy, int *w, int *h) {
    const struct cursor_shape *s = cursor_theme_shape(kind);
    if (s) {
        int sc = cursor_theme_scale();
        *ox = x - s->hot_x * sc - CURSOR_BOX_MARGIN;
        *oy = y - s->hot_y * sc - CURSOR_BOX_MARGIN;
        *w  = s->w * sc + 2 * CURSOR_BOX_MARGIN;
        *h  = s->h * sc + 2 * CURSOR_BOX_MARGIN;
    } else if (kind == WM_CURSOR_H || kind == WM_CURSOR_V ||
               kind == WM_CURSOR_DIAG || kind == WM_CURSOR_DIAG2) {
        // Centred too, since draw_resize_cursor() draws from the grid's
        // middle. A box that assumed the other three's down-and-right
        // anchor would strand half the arrow on every move.
        *ox = x - RZ_GRID / 2 - CURSOR_BOX_MARGIN;
        *oy = y - RZ_GRID / 2 - CURSOR_BOX_MARGIN;
        *w = *h = RZ_GRID + 2 * CURSOR_BOX_MARGIN;
    } else if (kind == WM_CURSOR_TEXT || kind == WM_CURSOR_WAIT) {
        // Centred on their hotspots, so not the down-and-right box the
        // other three share. The drawing functions' own constants; if
        // these disagree a move strands part of the sprite.
        int cw = kind == WM_CURSOR_TEXT ? CURSOR_TEXT_W : CURSOR_WAIT_W;
        int chh = kind == WM_CURSOR_TEXT ? CURSOR_TEXT_H : CURSOR_WAIT_H;
        int hx = kind == WM_CURSOR_TEXT ? CURSOR_TEXT_HOT_X : CURSOR_WAIT_HOT_X;
        int hy = kind == WM_CURSOR_TEXT ? CURSOR_TEXT_HOT_Y : CURSOR_WAIT_HOT_Y;
        *ox = x - hx - CURSOR_BOX_MARGIN;
        *oy = y - hy - CURSOR_BOX_MARGIN;
        *w  = cw + 2 * CURSOR_BOX_MARGIN;
        *h  = chh + 2 * CURSOR_BOX_MARGIN;
    } else {
        *ox = x - CURSOR_BOX_MARGIN;
        *oy = y - CURSOR_BOX_MARGIN;
        *w = *h = CURSOR_BOX_SIZE;
    }
    if (*w > CURSOR_UNDER_MAX) *w = CURSOR_UNDER_MAX;
    if (*h > CURSOR_UNDER_MAX) *h = CURSOR_UNDER_MAX;
}

static uint32_t cursor_under[CURSOR_UNDER_MAX][CURSOR_UNDER_MAX];
static int cursor_under_valid = 0;
static int cursor_under_x, cursor_under_y, cursor_under_w, cursor_under_h;

static void restore_cursor_under(void) {
    if (!cursor_under_valid) return;
    for (int j = 0; j < cursor_under_h; j++)
        for (int i = 0; i < cursor_under_w; i++)
            ugfx_put_pixel(wm_surface(), cursor_under_x + i, cursor_under_y + j, cursor_under[j][i]);
    cursor_under_valid = 0;
}

static void save_cursor_under(int x, int y, enum wm_cursor_kind kind) {
    int sx, sy, w, h;
    cursor_rect(kind, x, y, &sx, &sy, &w, &h);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            cursor_under[j][i] = ugfx_get_pixel(wm_surface(), sx + i, sy + j);
    cursor_under_x = sx;
    cursor_under_y = sy;
    cursor_under_w = w;
    cursor_under_h = h;
    cursor_under_valid = 1;
}

// Puts back what the SOFTWARE pointer is covering, into `dst`, whose
// top-left sits at (ox, oy) on the screen. Returns 1 if it erased
// anything.
//
// The screenshot path needs this because the default is a capture with
// no pointer in it, and with a software cursor the pointer IS in the
// back buffer -- there is nothing to leave out, only something to undo.
// These are the exact pixels the sprite overwrote, so the result is the
// frame as it would have been, not a repaint of it.
int wm_render_cursor_erase(struct ugfx_surface *dst, int ox, int oy) {
    if (!cursor_under_valid) return 0;
    for (int j = 0; j < cursor_under_h; j++)
        for (int i = 0; i < cursor_under_w; i++)
            ugfx_put_pixel(dst, cursor_under_x + i - ox,
                            cursor_under_y + j - oy, cursor_under[j][i]);
    return 1;
}

// --- the hardware cursor is BACK, and per shape ------------------------
//
// The plane the M41 migration measured away returned with virtio-gpu's
// cursorq (wm_hwcursor.c, WIN_REQ_FB_CURSOR): draw_cursor_at() asks
// wm_hwcursor_sync() first, and when the plane shows the pointer the
// software sprite below draws NOTHING -- no saved pixels, no damage, a
// zero-length prev box. The handover is per SHAPE, not per boot: a
// shape the plane cannot hold (cursor_size=huge past 64px, a built-in
// resize arrow that exists only as draw calls) falls back to the
// sprite for exactly as long as it is the resolved shape.
//
// The software sprite still needs nothing from the kernel: it draws
// into the framebuffer the compositor was already granted (R1). See
// docs/decisions.md.

// The outline a drag is proposing, when the drag is showing one rather
// than moving the window itself (`desktop.move_mode`,
// `desktop.resize_mode` -- see wm_input.c). A 1px rectangle: cheap,
// unmistakable, and the idiom every WM used before compositing made
// live dragging affordable.
//
// Drawn just before the cursor, so it sits over the windows it
// describes.
static void draw_drag_outline(void) {
    int i = drag_outline_win;
    if (i < 0 || i >= window_count) return;
    if (drag_outline_w <= 0 || drag_outline_h <= 0) return;
    ugfx_draw_rect(wm_surface(), drag_outline_x, drag_outline_y,
                   drag_outline_w, drag_outline_h, UTHEME_WHITE);
    ugfx_draw_rect(wm_surface(), drag_outline_x + 1, drag_outline_y + 1,
                   drag_outline_w - 2, drag_outline_h - 2, ugfx_rgb(50, 90, 160));
}

// Saves what's under (x, y) before drawing the cursor there, so a later
// cursor-only move can restore it. Used by both render paths.
// What was last actually drawn, recorded by the code that drew it.
static int drawn_cursor_kind = -1;

static void draw_cursor_at(int x, int y) {
    draw_drag_outline();
    // NO POINTER OVER A SCREENSAVER. An arrow sitting on top of the
    // stars is what every version of this feature has hidden since
    // Windows 3.1, and it is not cosmetic here: the pointer is the one
    // thing on screen that says the machine is awake.
    //
    // THE PLANE HAS TO BE TOLD. On a machine with a hardware cursor the
    // sprite is not part of the composited image, so not drawing it
    // leaves it exactly where it was -- which is the bug this fixes,
    // reported from the ASUS where the plane is live.
    //
    // Both render paths funnel through here, so this is the whole of
    // it: the cheap move path has already restored what was under the
    // sprite by the time it calls this.
    if (wm_idle_saver_pid()) {
        wm_hwcursor_hide();
        drawn_cursor_kind = -1;
        return;
    }
    enum wm_cursor_kind kind = resolve_cursor_kind(x, y);
    if (wm_hwcursor_sync(kind)) {
        // The plane shows the pointer: nothing saved, nothing drawn.
        // drawn_cursor_kind still tracks, so a shape change is still
        // what re-renders (wm_cursor_shape_changed()).
        drawn_cursor_kind = (int)kind;
        return;
    }
    save_cursor_under(x, y, kind);
    draw_cursor(x, y, kind);
    drawn_cursor_kind = (int)kind;
}

int wm_cursor_shape_changed(int mx, int my) {
    return (int)resolve_cursor_kind(mx, my) != drawn_cursor_kind;
}

// --- rounded corners --------------------------------------------------
//
// A window that is not maximized has its four corners rounded, KDE
// Breeze's and Windows 11's frame; a maximized one is square, as both
// make it. The compositor repaints everything under the damage box
// back to front (docs/decisions/gui.md), so what lies under a corner --
// wallpaper or a lower window -- is already on the surface when this
// window paints. The corner pixels are SAVED before the window draws
// and blended back afterwards by the arc's coverage, which is what
// makes the edge anti-aliased against whatever is really behind it
// rather than against a guessed colour.
//
// THE RADIUS IS FONT-DERIVED (half the line height, 8 px at the default
// font -- Breeze's), so it scales with the chrome. Hit testing stays
// rectangular on purpose: a click in a corner still belongs to the
// window, as it does on every real desktop.

#define CORNER_MAX_R 16
// ONE OUTLINE COLOUR ALL THE WAY ROUND, the theme's border: Breeze,
// Windows 11 and macOS draw a single hairline, and the two-tone bevel
// this replaced read as a raised Windows 95 panel once the corners
// rounded.
#define FRAME_OUTLINE UTHEME_BORDER

static int corner_radius(const struct window *win) {
    if (win->state == WIN_MAXIMIZED || win->fullscreen) return 0;
    int r = ugfx_char_h() / 2;
    if (r < 4) r = 4;
    if (r > CORNER_MAX_R) r = CORNER_MAX_R;
    if (2 * r > win->w || 2 * r > win->h) return 0;
    return r;
}

// Coverage of pixel (px, py) -- measured from the corner's outer edge,
// 0 being the outermost row/column -- by a disc of radius `rad` centred
// on the arc centre (r, r), in 0..255. Sixteen sub-samples per pixel; r
// is a handful of pixels, so the whole corner is a few hundred compares.
static uint8_t corner_coverage(int r, int rad, int px, int py) {
    int in = 0;
    for (int sy = 0; sy < 4; sy++) {
        for (int sx = 0; sx < 4; sx++) {
            // Sub-sample centre in eighths, distance to (r, r) in the
            // same units: inside when d^2 <= (8 rad)^2.
            int cx = 8 * px + 2 * sx + 1 - 8 * r;
            int cy = 8 * py + 2 * sy + 1 - 8 * r;
            if (cx * cx + cy * cy <= 64 * rad * rad) in++;
        }
    }
    return (uint8_t)(in * 255 / 16);
}

static uint32_t corner_under[4][CORNER_MAX_R * CORNER_MAX_R];

// The screen pixel for corner c's (px, py), px/py counted inward from
// that corner's own edges.
static void corner_pixel(const struct window *win, int c, int px, int py,
                         int *x, int *y) {
    *x = (c & 1) ? win->x + win->w - 1 - px : win->x + px;
    *y = (c & 2) ? win->y + win->h - 1 - py : win->y + py;
}

// Only a pixel the frame will actually REPAINT may be saved and
// re-blended: one outside the damage clip keeps its previous composite,
// which already holds a blended corner, and blending it again drifts
// it a little every frame -- the damage verifier's exact-repaint check
// caught that on day one.
static int corner_in_clip(int x, int y) {
    const struct ugfx_surface *s = wm_surface();
    if (!s->clip_active) return 1;
    return x >= s->clip_x0 && x < s->clip_x1 && y >= s->clip_y0 && y < s->clip_y1;
}

static void corners_save(const struct window *win) {
    int r = corner_radius(win);
    if (!r) return;
    for (int c = 0; c < 4; c++)
        for (int py = 0; py < r; py++)
            for (int px = 0; px < r; px++) {
                int x, y;
                corner_pixel(win, c, px, py, &x, &y);
                corner_under[c][py * r + px] = ugfx_get_pixel(wm_surface(), x, y);
            }
}

static void corners_round(const struct window *win) {
    int r = corner_radius(win);
    if (!r) return;
    for (int c = 0; c < 4; c++)
        for (int py = 0; py < r; py++)
            for (int px = 0; px < r; px++) {
                uint8_t cov   = corner_coverage(r, r, px, py);
                uint8_t inner = corner_coverage(r, r - 1, px, py);
                if (cov == 255 && inner == 255) continue;
                int x, y;
                corner_pixel(win, c, px, py, &x, &y);
                if (!corner_in_clip(x, y)) continue;
                uint32_t under = corner_under[c][py * r + px];
                if (cov == 0) { ugfx_put_pixel(wm_surface(), x, y, under); continue; }
                if (cov < 255) ugfx_blend_pixel(wm_surface(), x, y, under, (uint8_t)(255 - cov));
                // THE OUTLINE FOLLOWS THE ARC: the hairline is four straight
                // lines, so the ring between radius r and r-1 carries its
                // colour round the corner.
                uint8_t ring = (uint8_t)(cov - inner);
                if (ring) ugfx_blend_pixel(wm_surface(), x, y, FRAME_OUTLINE, ring);
            }
}

static void draw_window_chrome(struct window *win, int idx, int focused) {
    uint32_t titlebar = focused ? ugfx_rgb(50, 90, 160) : ugfx_rgb(120, 120, 130);
    uint32_t titletext = UTHEME_WHITE;
    uint32_t winbg = UTHEME_WINDOW_BG;
    int can_resize = win->resizable;

    ugfx_fill_rect(wm_surface(), win->x, win->y, win->w, win->h, winbg);

    // A 1px hairline in one colour, which the rounded corners continue.
    uint32_t outline = FRAME_OUTLINE;
    ugfx_fill_rect(wm_surface(), win->x, win->y, win->w, 1, outline);                // top
    ugfx_fill_rect(wm_surface(), win->x, win->y, 1, win->h, outline);                // left
    ugfx_fill_rect(wm_surface(), win->x, win->y + win->h - 1, win->w, 1, outline);   // bottom
    ugfx_fill_rect(wm_surface(), win->x + win->w - 1, win->y, 1, win->h, outline);   // right

    ugfx_fill_rect(wm_surface(), win->x + 1, win->y + 1, win->w - 2, WM_TITLEBAR_H, titlebar);

    struct btn_rects r = title_buttons(win);

    // Truncate the title if the window's been resized narrower than it
    // needs -- otherwise it draws over the minimize/maximize/close buttons.
    //
    // A wedged client says so in its title bar, as it does on Windows.
    // Built into the SAME buffer that gets truncated, so the suffix is
    // subject to the same width budget as the name -- appending it after
    // the truncation would draw straight over the buttons, which is this
    // codebase's most-repeated drawing bug (docs/gui-guidelines.md).
    char full[WIN_TITLE_MAX + 20];
    const char *shown = win->title;
    if (win->not_responding) {
        k_snprintf(full, sizeof full, "%s (Not Responding)", win->title);
        shown = full;
    }

    // THE APP ICON, and the title shifted past it -- Windows' system
    // menu icon and Breeze's window-menu button, in the same corner
    // both put it. Drawn BEFORE the width budget below is computed, so
    // the title is truncated against the room the icon actually left;
    // appending chrome after a truncation is this codebase's
    // most-repeated drawing bug (docs/gui-guidelines.md).
    int text_x = win->x + 6;
    int ix, iy, isz;
    const struct uimg *ico = title_icon(idx, &ix, &iy, &isz);
    if (ico) {
        // ALPHA, not a plain blit: an icon is a rounded tile on a
        // transparent field, and a plain blit lands its corners as
        // black squares on the title bar.
        ugfx_blit_alpha(wm_surface(), ix, iy, ico->w, ico->h, ico->px, ico->w);
        text_x = ix + isz + 5;
    }

    // **MEASURED, NOT DIVIDED.** `avail_px / ugfx_char_w()` asks how
    // many of the WIDEST glyph fit, which is the right answer only on a
    // monospace face -- with a proportional interface face it cuts a
    // title several characters early and every window reads as
    // truncated. ugfx_text_fit_chars() exists for exactly this
    // (ui/ugfx.h's "no widget does character arithmetic on a string").
    char title_buf[WIN_TITLE_MAX + 20];
    int avail_px = r.min_x - text_x;
    int max_chars = avail_px > 0 ? ugfx_text_fit_chars(shown, avail_px) : 0;
    int i = 0;
    for (; shown[i] && i < max_chars && i < (int)sizeof title_buf - 1; i++) title_buf[i] = shown[i];
    title_buf[i] = '\0';
    ugfx_draw_string(wm_surface(), text_x, win->y + (WM_TITLEBAR_H - ugfx_char_h()) / 2, title_buf, titletext, titlebar);

    uint32_t btnbg = ugfx_rgb(230, 230, 235);
    uint32_t btnfg = UTHEME_TEXT;

    // Hover/press feedback for the three title-bar buttons -- see
    // wm_internal.h's title_btn_armed_win/title_hover_win comments for
    // the full press-hover-commit-on-release story. Each button gets
    // one of three looks: plain (nothing armed or hovered), hover (a
    // lighter tint -- cursor's over it, mouse not held), or pressed
    // (armed AND the cursor's still over it -- the tint PLUS
    // uui_button_draw()'s own inset `pressed` look, same visual language
    // used everywhere else a button can be held in this codebase, e.g.
    // Calculator). Dragging off an armed button (armed but NOT
    // pressed_active) drops back to the plain look, not hover -- a
    // button that's about to be canceled shouldn't look like it's still
    // being interacted with.
    // No hand-picked tints any more: uui_state_bg() derives hover and
    // pressed from each button's OWN base colour, so the red close
    // button gets a lighter red and a darker red for free. The pair of
    // constants that used to live here -- one neutral, one a
    // specially-chosen lighter red -- existed only because a caller had
    // no way to shift an arbitrary packed colour. See
    // docs/gui-guidelines.md.
    int is_armed = (title_btn_armed_win == idx ? UUI_STATE_PRESSED : UUI_STATE_REST);
    int is_hovered = (title_hover_win == idx);

    // Icon-only buttons: uui_button_draw() with label=NULL just fills the
    // background, then each hand-drawn icon goes on top with its own
    // gfx_* calls -- these aren't text, so widgets.h's centered-label
    // path doesn't apply to them (see widgets.h's top comment).
    draw_btn_disc(r.min_x, r.y, r.size,
                   uui_state_bg(btnbg, title_btn_state(0, is_armed, is_hovered)
                                        ? UUI_STATE_PRESSED : UUI_STATE_REST));
    draw_min_icon(r.min_x, r.y, r.size, btnfg);

    // Maximize/restore: drawn muted and does nothing when the app isn't
    // resizable (Calculator -- see gui_apps.h) -- a visibly "disabled"
    // button rather than removing it, so the title bar layout doesn't
    // shift between fixed and resizable apps. Still shows hover/press
    // feedback either way (it still focuses the window on commit even
    // when disabled -- see wm_update_title_btn_press()), just tinted
    // from its own muted base color instead of btnbg.
    uint32_t max_bg_base = can_resize ? btnbg : ugfx_rgb(210, 210, 212);
    uint32_t max_fg = can_resize ? btnfg : ugfx_rgb(170, 170, 172);
    draw_btn_disc(r.max_x, r.y, r.size,
                   uui_state_bg(max_bg_base, title_btn_state(1, is_armed, is_hovered)));
    if (win->state == WIN_MAXIMIZED) draw_restore_icon(r.max_x, r.y, r.size, max_fg);
    else                             draw_max_icon(r.max_x, r.y, r.size, max_fg);

    // close: red button with a hand-drawn X. This used to draw the font
    // glyph 'x' via gfx_draw_char(), which clipped/overflowed once the
    // font became runtime-resizable (a glyph cell is 11x22 to 20x40px,
    // way bigger than this button at most sizes). A hand-drawn diagonal
    // cross scales cleanly with r.size instead, same approach already
    // used for the minimize/maximize icons above.
    // THE CLOSE BUTTON IS GREY UNTIL YOU HOVER IT. Red is the mark of
    // the destructive action and it belongs on the moment you are about
    // to take it, not on every title bar for the whole session --
    // Windows, GNOME and KDE all moved this way. The glyph goes white on
    // red so it stays legible against it.
    int close_state = title_btn_state(2, is_armed, is_hovered);
    int close_hot = (close_state != UUI_STATE_REST);
    uint32_t close_bg = close_hot
        ? uui_state_bg(ugfx_rgb(190, 60, 60),
                        close_state == UUI_STATE_PRESSED ? UUI_STATE_PRESSED
                                                          : UUI_STATE_REST)
        : uui_state_bg(btnbg, UUI_STATE_REST);
    draw_btn_disc(r.close_x, r.y, r.size, close_bg);
    draw_close_icon(r.close_x, r.y, r.size, close_hot ? UTHEME_WHITE : btnfg);
}

// Resize grip hint in the bottom-right corner -- drawn as a separate
// pass AFTER the app's on_draw() (see wm_render_frame()), not as part
// of draw_window_chrome() above. The grip's pixels sit right at the
// edge of the content area, and every app's on_draw() repaints its
// whole content area every frame (e.g. notepad_draw()'s background
// gfx_fill_rect()) -- drawing the grip before that content redraw meant
// it was invisible, silently painted over a moment later. Not shown
// when maximized or when the app isn't resizable (see gui_apps.h).
// A DIAGONAL OF DOTS, six of them in a 3-2-1 triangle stepping up from
// the corner -- the macOS and GTK grip. It replaced an L of two solid
// bars, which read as half a crosshair: two axis-aligned strokes say
// "here is a corner", and a corner is not what the control does. The
// diagonal says which way to drag, which is the whole message.
//
// **DOTS RATHER THAN SOLID DIAGONAL LINES**, and that is the reason it
// survives at this size. A 1px diagonal on an unantialiased raster is a
// staircase, and three of them next to a scrollbar's down-arrow -- which
// sits directly above this on any scrollable window -- read as a smudge
// competing with a control. Discrete dots read as TEXTURE, which is
// what a grip should be: findable when looked for, invisible when not.
#define GRIP_DOT   2   // px, square
#define GRIP_STEP  3   // px between dot origins -- one clear pixel between
#define GRIP_INSET 3   // px from the frame's outer edge to the first dot
#define GRIP_ROWS  3

static void draw_resize_grip(const struct window *win) {
    if (win->state == WIN_MAXIMIZED) return;
    if (!win->resizable) return;

    // The corner the dots are anchored to. Everything below counts
    // INWARD from here, so the shape cannot drift off the frame when a
    // window is at the screen edge.
    int right = win->x + win->w - GRIP_INSET - GRIP_DOT;
    int bottom = win->y + win->h - GRIP_INSET - GRIP_DOT;

    for (int row = 0; row < GRIP_ROWS; row++) {
        // Row 0 is the bottom one and is the longest: the triangle's
        // hypotenuse runs from bottom-left to top-right, which is the
        // direction the drag goes.
        for (int col = 0; col + row < GRIP_ROWS; col++) {
            ugfx_fill_rect(wm_surface(),
                           right - col * GRIP_STEP,
                           bottom - row * GRIP_STEP,
                           GRIP_DOT, GRIP_DOT, UTHEME_BORDER);
        }
    }
}

static void draw_taskbar(void) {
    uint32_t bg = ugfx_rgb(30, 30, 34), fg = ugfx_rgb(230, 230, 230);
    int ty = screen_h - taskbar_h;
    ugfx_fill_rect(wm_surface(), 0, ty, screen_w, taskbar_h, bg);

    int sbw = start_btn_w();

    uint32_t start_bg = start_menu_open ? ugfx_rgb(70, 70, 90) : ugfx_rgb(50, 50, 60);
    // THE THREE MODES. `text` draws the button exactly as it always
    // did, label centred by uui_button_draw(); the other two draw the
    // button with an EMPTY label and place the mark (and, for `both`,
    // the word) by hand -- the same arrangement the window buttons on
    // this strip already use, and for the same reason: a centred label
    // beside a left-hand icon reads as neither centred nor aligned.
    int sx, sy, sisz;
    const struct uimg *sico = start_icon(&sx, &sy, &sisz);
    // The button carries the word only when there is no mark -- with
    // one, the label is placed by hand beside it (BOTH) or omitted
    // (ICON), because uui_button_draw() CENTRES its label and a centred
    // label beside a left-hand mark reads as neither.
    uui_button_draw(wm_surface(), 4, ty + 4, sbw, taskbar_h - 8,
                     sico ? "" : START_LABEL, start_bg, fg, UUI_STATE_REST);
    if (sico) {
        ugfx_blit_alpha(wm_surface(), sx, sy, sico->w, sico->h, sico->px, sico->w);
        if (taskbar_start_mode() == START_BUTTON_BOTH) {
            ugfx_draw_string_clipped(wm_surface(), sx + sisz + 5,
                                     ty + 4 + (taskbar_h - 8 - ugfx_char_h()) / 2,
                                     4 + sbw - 8 - (sx + sisz + 5) + 4,
                                     START_LABEL, fg, start_bg);
        }
    }

    // The buttons come from taskbar_layout(), which is also what
    // wm_input.c hit-tests against -- this loop used to walk the windows
    // itself with a fixed step, and so did both hit-tests, which is how
    // the strip came to run off the screen edge (wm_taskbar.h).
    static struct taskbar_button btns[64];
    int nb = taskbar_layout(btns, 64);
    for (int b = 0; b < nb; b++) {
        int i = btns[b].first;
        int is_front_and_visible = (i == wm_focus_index() && windows[i].state != WIN_MINIMIZED);
        uint32_t wbg = is_front_and_visible ? ugfx_rgb(70, 70, 90) : ugfx_rgb(50, 50, 60);

        // AN ICON WHERE THERE IS ONE, and the label shifted past it --
        // as on every taskbar since Windows 95. The button is drawn with
        // an EMPTY label and the text placed here, because
        // uui_button_draw() centres its label and a centred label beside
        // a left-hand icon reads as neither centred nor aligned. The
        // width the label was truncated to already accounts for the
        // column (wm_taskbar.h says why both live in one place).
        const struct uimg *ico = btns[b].icon ?
            icon_get(btns[b].icon, taskbar_icon_size()) : NULL;
        uui_button_draw(wm_surface(), btns[b].x, ty + 4, btns[b].w, taskbar_h - 8,
                         ico ? "" : btns[b].label, wbg, fg, UUI_STATE_REST);
        if (ico) {
            ugfx_blit_alpha(wm_surface(), btns[b].x + 4, ty + 5, ico->w, ico->h,
                            ico->px, ico->w);
            int tx = btns[b].x + 6 + ico->w;
            ugfx_draw_string_clipped(wm_surface(), tx, ty + 4 + (taskbar_h - 8 - ugfx_char_h()) / 2,
                                     btns[b].x + btns[b].w - 4 - tx,
                                     btns[b].label, fg, wbg);
        }
    }

    draw_tray(ty, bg, fg);
}

// The content rectangles of every visible CLIENT window -- the pixels
// the damage verifier must not judge. See wm_render_frame().
//
// Chrome is deliberately NOT included: a title bar is drawn by this
// compositor out of its own state, so it is fully verifiable and is
// exactly where a missed damage declaration for a client window would
// show up.
static int client_content_rects(struct ugfx_skip_rect *out, int max) {
    int n = 0;
    for (int i = 0; i < window_count && n < max; i++) {
        const struct window *w = &windows[i];
        if (!wm_client_is_client_window(w) || w->state == WIN_MINIMIZED) continue;
        out[n].x = window_content_x(w);
        out[n].y = window_content_y(w);
        out[n].w = w->client_w;
        out[n].h = w->client_h;
        n++;
    }
    return n;
}

// ---- scene damage region (compositor) ----
//
// Separate from gfx.c's own dirty-PIXEL tracking (which operates on the
// framebuffer, after drawing, purely to shrink gfx_present()'s blit) --
// this tracks, at the SCENE level, which screen region actually needs
// repainting BEFORE drawing happens, so wm_render_frame() can clip the
// whole pass to it instead of always touching the full screen (every
// draw call bottoms out at gfx_put_pixel(), which silently skips
// anything outside the active clip -- see gfx_set_clip_rect()). See
// docs/decisions.md for the overall design and why window geometry
// changes are handled precisely here (comparing each window's
// last-rendered rect to its current one, below) while most other
// redraw_pending sources (menus, taskbar, dialogs, the once-a-second
// clock) still fall back to a full-screen repaint for now -- a
// deliberately scoped first cut, not the final word; see the roadmap's
// Milestone 12 entry for what's still open.
static int damage_x0, damage_y0, damage_x1, damage_y1;

void wm_damage_rect(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    int x1 = x + w, y1 = y + h;
    if (damage_x1 <= damage_x0) { // was empty
        damage_x0 = x; damage_x1 = x1;
        damage_y0 = y; damage_y1 = y1;
        return;
    }
    if (x < damage_x0) damage_x0 = x;
    if (x1 > damage_x1) damage_x1 = x1;
    if (y < damage_y0) damage_y0 = y;
    if (y1 > damage_y1) damage_y1 = y1;
}

static void damage_reset(void) {
    damage_x0 = damage_y0 = damage_x1 = damage_y1 = 0;
}

// Compares every window's rect/visibility against what it was as of
// the last repaint and reports whatever changed as damage, before this
// frame draws anything -- see wm_damage_rect()'s comment above.
// Handles the three cases that don't need a caller elsewhere to
// explicitly report anything:
//   - a window just opened (last_w == 0, the "never rendered" sentinel)
//     -- damage its own (now-visible) rect; nothing to reveal from an
//     "old" position since there wasn't one.
//   - visibility flipped (minimized <-> restored) -- damage whichever
//     rect is relevant (current if now visible, last-known if not).
//     Sufficient on its own even though other windows may now be
//     revealed/covered underneath: redrawing everything within that
//     rect, back-to-front, naturally repaints whatever's really there
//     now, the same reason a plain move/resize's union rect below is
//     sufficient too.
//   - geometry changed (move/resize) -- damage the union of its old
//     and new rect.
// bring_to_front()/close_window() (wm.c) report their own precise
// damage directly via wm_damage_rect() at the point they mutate
// windows[], since by the time this runs a closed window is already
// gone from the array and a reordered window's geometry didn't change
// (nothing here would notice either on its own).
static void compute_window_damage(void) {
    for (int i = 0; i < window_count; i++) {
        struct window *w = &windows[i];
        int visible_now = (w->state != WIN_MINIMIZED);

        if (w->last_w == 0) {
            if (visible_now) wm_damage_window_rect(w->x, w->y, w->w, w->h);
        } else if (visible_now != w->last_visible) {
            wm_damage_window_rect(visible_now ? w->x : w->last_x,
                                   visible_now ? w->y : w->last_y,
                                   visible_now ? w->w : w->last_w,
                                   visible_now ? w->h : w->last_h);
            // draw_taskbar()'s per-button tint depends on whether the
            // FRONTMOST window is visible (see wm.c's bring_to_front()
            // comment on the same point) -- minimizing/restoring it
            // changes that button's look even though no window's
            // geometry or z-order changed. Only actually matters when
            // i == window_count - 1, but damaging the strip either way
            // is cheap and simpler than special-casing which index.
            wm_damage_rect(0, screen_h - taskbar_h, screen_w, taskbar_h);
        } else if (visible_now && (w->x != w->last_x || w->y != w->last_y ||
                                    w->w != w->last_w || w->h != w->last_h)) {
            int ux0 = w->x < w->last_x ? w->x : w->last_x;
            int uy0 = w->y < w->last_y ? w->y : w->last_y;
            int ux1_a = w->x + w->w, ux1_b = w->last_x + w->last_w;
            int uy1_a = w->y + w->h, uy1_b = w->last_y + w->last_h;
            int ux1 = ux1_a > ux1_b ? ux1_a : ux1_b;
            int uy1 = uy1_a > uy1_b ? uy1_a : uy1_b;
            wm_damage_window_rect(ux0, uy0, ux1 - ux0, uy1 - uy0);
        }

        w->last_x = w->x; w->last_y = w->y; w->last_w = w->w; w->last_h = w->h;
        w->last_visible = visible_now;
    }
}

// The damage box as it stood at the end of the last frame, for
// apps/wm/wm_debug.c's `gui state`. Reports w/h <= 0 when nothing was
// damaged (a full-screen repaint) rather than pretending to a rect --
// that distinction is exactly what someone debugging a repaint wants.
void wm_debug_damage(int *out_x, int *out_y, int *out_w, int *out_h) {
    *out_x = damage_x0;
    *out_y = damage_y0;
    *out_w = damage_x1 - damage_x0;
    *out_h = damage_y1 - damage_y0;
}

// Re-applies whatever clip the current frame's scene should be drawn
// under -- the accumulated damage box, or none. Paired with
// clip_to_window_content() below, which narrows it temporarily.
static void apply_scene_clip(int has_damage) {
    if (has_damage) {
        ugfx_set_clip_rect(wm_surface(), damage_x0, damage_y0, damage_x1 - damage_x0, damage_y1 - damage_y0);
    } else {
        ugfx_clear_clip_rect(wm_surface());
    }
}

// Narrows the clip to a window's CONTENT area for the duration of its
// on_draw(), so nothing an app draws can land outside its own window.
//
// **This is a containment boundary, not an optimisation.** Nothing else
// enforced it: an app whose content didn't fit painted straight over the
// desktop and any window behind it. Shrinking the Control Panel is how
// it turned up -- System Info's lower rows carried on down the desktop,
// perfectly legible, well outside the frame. gfx_draw_string_clipped()
// doesn't help there: it bounds a string's WIDTH and has no notion of a
// row budget, so the horizontal edge was clipped correctly while the
// bottom had nothing stopping it at all.
//
// Intersects with the scene clip by hand because gfx_set_clip_rect()
// REPLACES the active rect rather than intersecting -- setting the
// content rect naively would have widened the damage clip back out and
// quietly undone the compositor's whole point.
static void clip_to_window_content(const struct window *w, int has_damage) {
    int x0 = window_content_x(w), y0 = window_content_y(w);
    int x1 = x0 + window_content_w(w), y1 = y0 + window_content_h(w);

    if (has_damage) {
        if (damage_x0 > x0) x0 = damage_x0;
        if (damage_y0 > y0) y0 = damage_y0;
        if (damage_x1 < x1) x1 = damage_x1;
        if (damage_y1 < y1) y1 = damage_y1;
    }
    // A non-positive w/h is gfx_set_clip_rect()'s "nothing draws", which
    // is exactly right for a window with no visible content this frame.
    // (This sentence used to be a lie -- gfx_set_clip_rect() CLEARED the
    // clip on non-positive sizes, so exactly this case handed the app an
    // unclipped full-screen on_draw(). That was the damage sweep's
    // standing 20px violation: a tick frame's damage strip grazing only
    // a window's bottom border row emptied this intersection, the
    // content repaint escaped, and the resize grip under it was buried.
    // The contract in gfx.c matches the words now.)
    ugfx_set_clip_rect(wm_surface(), x0, y0, x1 - x0, y1 - y0);
}

// Does this window's current rect overlap the accumulated damage box at
// all? Used by wm_render_frame() (Phase 3, see docs/decisions.md) to
// skip a whole window's chrome/on_draw()/resize-grip work, not just the
// pixels it would have touched -- gfx_set_clip_rect() already drops
// those pixel writes for free, but the CALLS themselves (an app's
// on_draw() walking its own widgets/content) still cost real CPU even
// when every write they make gets clipped away. Only meaningful when a
// damage box was actually reported this frame (see the has_damage check
// at the call site) -- with no damage, every window is "unaffected" by
// this definition too, which would wrongly skip everyone.
static int window_intersects_damage(const struct window *w) {
    // Grown by the shadow's reach: a damage box that touches only the
    // shadow still needs this window to repaint it (wm_shadow.h).
    int m = wm_shadow_enabled() ? wm_shadow_margin() : 0;
    return w->x - m < damage_x1 && w->x + w->w + m > damage_x0 &&
           w->y - m < damage_y1 && w->y + w->h + m > damage_y0;
}

// Draws the whole scene for this frame. Split out of wm_render_frame()
// so it can be run TWICE: once damage-limited as normal, and once
// unrestricted for the verify mode below, which is only meaningful if
// both renders go through identical code.
// The top window is fullscreen AND has adopted the screen's size: it
// covers everything, so the wallpaper, the taskbar and the windows
// under it are not painted. Before the adoption the proposal is still
// in flight and the old buffer does not cover the screen.
int wm_top_covers_screen(void) {
    int focus = wm_focus_index();
    if (focus < 0) return 0;
    const struct window *w = &windows[focus];
    return w->fullscreen && w->x == 0 && w->y == 0 && w->w >= screen_w && w->h >= screen_h;
}

static void render_scene(int mx, int my, int has_damage) {
    apply_scene_clip(has_damage);

    int covered = wm_top_covers_screen();
    if (!covered) desktop_draw(); // background + icon grid, see desktop.h

    // Phase 3: a window whose rect doesn't overlap this frame's damage
    // box gets skipped entirely -- not just clipped. Its chrome and
    // on_draw() would write nothing visible anyway (every write lands
    // outside the active clip and gfx_put_pixel() drops it), so calling
    // them is pure wasted CPU once the damage region is known to be
    // precise. See docs/decisions.md for why this needed
    // bring_to_front() (wm.c) to start damaging the previously-frontmost
    // window's rect too, not just the newly-promoted one -- that gap
    // was harmless before this skip existed (the call still happened,
    // just clipped away) and became a real visible bug once it didn't.
    int focus = wm_focus_index(); // the topmost TOPLEVEL -- a popup's parent stays active
    // A FRAME WITHOUT ONE CLIENT'S WINDOWS (a self-excluding screenshot)
    // renders as if that client were not there: focus, and with it the
    // title-bar colour and the shadow's strength, go to the topmost
    // window that IS drawn. Spectacle hides itself the same way, and a
    // capture that showed every other window inactive was the tell.
    if (wm_render_hidden_pid()) {
        for (int i = window_count - 1; i >= 0; i--) {
            if (windows[i].state == WIN_MINIMIZED || windows[i].popup) continue;
            if (wm_client_is_client_window(&windows[i]) &&
                windows[i].client_pid == wm_render_hidden_pid()) continue;
            focus = i;
            break;
        }
    }
    for (int i = 0; i < window_count; i++) {
        if (windows[i].state == WIN_MINIMIZED) continue;
        if (wm_render_hidden_pid() &&
            wm_client_is_client_window(&windows[i]) &&
            windows[i].client_pid == wm_render_hidden_pid()) continue;
        if (has_damage && !window_intersects_damage(&windows[i])) continue;
        if (covered && i < focus && !windows[i].popup) continue; // under the fullscreen window
        if (windows[i].popup || windows[i].fullscreen) {
            // NO CHROME, NO CORNERS, NO GRIP -- and nothing at all until
            // the client's first present: the buffer opened at create is
            // whatever the client has drawn so far, which for one frame
            // is nothing, and a menu that flashes black before it
            // appears is the flash Wayland's map-on-first-commit avoids.
            if (windows[i].client_gen[windows[i].client_front] == 0) continue;
            // A menu's small shadow, under it (wm_shadow.h); a fullscreen
            // window has nothing beside it to shadow.
            if (windows[i].popup)
                wm_shadow_draw(windows[i].x, windows[i].y, windows[i].w, windows[i].h, 0,
                               WM_SHADOW_POPUP);
            clip_to_window_content(&windows[i], has_damage);
            wm_client_draw(&windows[i]);
            apply_scene_clip(has_damage);
            continue;
        }
        // The shadow FIRST, so corners_save() below sees it beneath the
        // corners and the rounded cut reveals shadow, not desktop. None
        // for a maximized window: nothing beside it to fall on.
        if (windows[i].state != WIN_MAXIMIZED)
            wm_shadow_draw(windows[i].x, windows[i].y, windows[i].w, windows[i].h,
                           corner_radius(&windows[i]),
                           i == focus ? WM_SHADOW_FOCUSED : WM_SHADOW_INACTIVE);
        corners_save(&windows[i]);   // what is beneath, before this window covers it
        draw_window_chrome(&windows[i], i, i == focus);
        if (windows[i].app && windows[i].app->on_draw) {
            // Only the app's own draw is confined to its content area.
            // The chrome above and the grip below are the WM's own
            // pixels and deliberately live at/outside that boundary.
            clip_to_window_content(&windows[i], has_damage);
            windows[i].app->on_draw(&windows[i]);
            apply_scene_clip(has_damage);
        } else if (wm_client_is_client_window(&windows[i])) {
            // A ring-3 client's window: its content is just the pixels
            // it has already drawn into its shared buffer, blitted.
            // Same content clip an app's on_draw() gets, for the same
            // reason -- a client that reports a size larger than its
            // window must not be able to paint over the chrome.
            clip_to_window_content(&windows[i], has_damage);
            wm_client_draw(&windows[i]);
            apply_scene_clip(has_damage);
        }
        draw_resize_grip(&windows[i]); // after on_draw() -- see its own comment
        corners_round(&windows[i]);    // last: the arc cuts chrome, content and grip alike
    }

    if (!covered) draw_taskbar();
    // Every open overlay, LEAST modal first, from the one table that
    // also decides who gets a click (wm_overlay.h). The order used to
    // be spelled out here and again, backwards, in wm_input.c -- two
    // lists that had to agree and nothing that made them.
    wm_overlay_draw(mx, my);
    wm_dnd_draw(mx, my);   // the cross-window drag's ghost, above everything

    // The cursor is always drawn full/unclipped, regardless of the scene
    // damage rect above -- it doesn't track its own screen position
    // against damage the way windows do, and it's cheap enough (a
    // single small sprite) that there's no real cost to always letting
    // it through.
    ugfx_clear_clip_rect(wm_surface());
    draw_cursor_at(mx, my); // also (re)establishes cursor_under for wm_render_cursor_move()

}

// ---- damage verification (debug) --------------------------------------
//
// **The bug class this exists for.** The compositor is correct only if
// everything that changes on screen is inside the damage rect, and
// nothing enforces that: damage is declared by hand from eight sites
// across three files, and a missed declaration produces stale pixels
// with no crash, no wrong return value and no failing assertion. Every
// rendering bug this project has had was that shape -- a window revealed
// by a raise, a cursor sprite overhanging its rect, an app drawing
// outside its own window.
//
// The check turns it into a loud one: render the frame the normal way,
// snapshot it, render the SAME frame with no damage limit, and compare.
// Any differing pixel is one the damage-limited path got wrong, and its
// coordinates say where to look. Off by default (it renders every frame
// twice and costs a full-screen buffer); `gui damage verify on` enables
// it -- see apps/wm/wm_debug.c.
static int first_frame = 1;
static int overlay_was_open;  // an overlay was up last frame -- see wm_render_frame()
static int prev_cursor_x = -1, prev_cursor_y = -1;
// The box the cursor was last DRAWN in, recorded beside the position
// because a themed shape's extent is not derivable from a position
// alone -- the theme, its hotspot and the size setting all move it, and
// any of the three can change between two frames.
static int prev_cursor_box_x, prev_cursor_box_y;
static int prev_cursor_box_w, prev_cursor_box_h;
static int verify_enabled;
static int verify_reported; // report each distinct failure once, not per frame

// Called by wm_run() when a GUI session begins, so the next frame is a
// full repaint even if the previous session left state behind.
void wm_render_reset(void) {
    first_frame = 1;
    overlay_was_open = 0;
    prev_cursor_x = prev_cursor_y = -1;
    prev_cursor_box_w = prev_cursor_box_h = 0;
}

void wm_damage_verify_set(int on) {
    verify_enabled = on ? 1 : 0;
    verify_reported = 0;
    if (!on) ugfx_verify_release(&g_wm_screen);
    wm_logf("wm: damage verification %s\n", on ? "ON (every frame rendered twice)" : "off");
}

int wm_damage_verify_enabled(void) { return verify_enabled; }

// The cursor's own damage. It is alpha-BLENDED, so it must always be
// composited over a freshly-drawn scene: if the pixels underneath
// aren't redrawn first, each frame blends the sprite over the previous
// frame's sprite and the anti-aliased edges creep steadily more opaque.
//
// That is why the cursor is a damage source like anything else rather
// than having its own save-the-pixels-underneath path beside the damage
// system. That parallel path is what produced the resize trail, and
// `gui damage verify on` then caught the self-compositing too --
// "80 px changed outside the damage rect, first at (641,360)" on frames
// where only the clock ticked, (641,360) being where the cursor sat.
//
// prev_cursor_* is "where the cursor was last actually DRAWN", and is
// recorded at the end of wm_render_frame() on every frame that draws the
// scene -- NOT here. It used to be updated in this function, which
// only runs on damage-limited frames, so a full-repaint frame moved the
// sprite without recording where it went; the next damage-limited frame
// then damaged a position the cursor had already left, and the pixels it
// was actually sitting on were never repainted. The residue is a second
// cursor left behind on screen.
//
// That stayed hidden while full-repaint frames were rare. Making the
// overlay fallback real (above) made them common and
// tools/damage_sweep.py found it immediately: "139 px changed outside
// the damage rect, first at (928,336)" -- 139 px being roughly a cursor
// sprite, and (928,336) exactly where the previous injected click had
// left it.
static void damage_cursor(int mx, int my) {
    // Generous: the sprite is 13x19 and the resize variants differ, so
    // a box comfortably covering any of them costs nothing meaningful
    // and removes a whole family of off-by-a-few-pixels questions.
    // Derived from the box save_cursor_under() actually uses, not from a
    // separately-chosen constant. It used to damage (x-1, y-1) sized
    // CURSOR_BOX_SIZE+2 while the sprite box is anchored at
    // (x-CURSOR_BOX_MARGIN, y-CURSOR_BOX_MARGIN) -- so the box's top row
    // and left column sat one pixel OUTSIDE the damage rect, and every
    // cursor move left a two-sided sliver behind. The extra 1px here is
    // slack, not the anchor: the anchor has to be the margin, or the two
    // drift apart again the moment either constant is retuned.
    // Derived from cursor_rect(), the same function save_cursor_under()
    // uses, with 1px of slack on each side. The PREVIOUS box is stored
    // rather than recomputed, because the shape under the old position
    // may not be the shape that is there now -- recomputing it with
    // today's kind and scale is how a theme or size change leaves the
    // last frame's larger sprite undamaged.
    if (prev_cursor_box_w > 0) {
        wm_damage_rect(prev_cursor_box_x - 1, prev_cursor_box_y - 1,
                        prev_cursor_box_w + 2, prev_cursor_box_h + 2);
    }
    // With the plane showing the pointer nothing is about to be drawn,
    // so only the ERASE half above applies -- it still fires once on
    // the frame the software sprite hands over, which is what removes
    // its last-drawn pixels.
    if (wm_hwcursor_active()) return;
    int ox, oy, w, h;
    cursor_rect(resolve_cursor_kind(mx, my), mx, my, &ox, &oy, &w, &h);
    wm_damage_rect(ox - 1, oy - 1, w + 2, h + 2);
}

// SCENE repaints, as distinct from the cursor-only path beside it in
// wm.c. It is here so a test can ask "did that input actually repaint
// anything?" without reading pixels -- the question a hover bug turns
// on, and one no screenshot answers reliably because the tray clock
// repaints once a second anyway and hides the difference.
static uint32_t g_scene_frames;

// --- what a frame COSTS ----------------------------------------------
//
// The number `docs/roadmap-details.md`'s blitter entry names as the
// condition for revisiting it ("a measured compositor frame time") and
// that nothing here could measure until now. Same shape as the ping's
// stats in wm_client.c -- last, worst and mean -- and for the same
// reason it carries TSC cycles beside microseconds: `sys_monotonic_ns()`
// is quantised to the 10 ms tick under an emulator, which is six times
// the figure being measured.
//
// TWO BUCKETS, AND ONE MEAN OVER BOTH WOULD MEASURE NOTHING. A
// damage-limited frame repaints a rectangle and a full frame repaints
// the screen; they differ by more than an order of magnitude, and it is
// the FULL one a blitter or a scanout plane would have to beat. Mixing
// them produces a number that moves with how much the mouse happened to
// be moving.
//
// Two kinds of frame are deliberately NOT counted: one that returned
// early because a client holds the display (nothing was drawn), and any
// frame while `gui damage verify on` is set (it renders the same scene
// two or three times on purpose).
struct frame_stats { unsigned long long last_us, max_us, sum_us; unsigned long long last_cyc, sum_cyc; unsigned n; };
static struct frame_stats g_frame_full, g_frame_partial;

static void frame_record(struct frame_stats *f, unsigned long long us,
                         unsigned long long cyc) {
    f->last_us = us;
    if (us > f->max_us) f->max_us = us;
    f->sum_us += us;
    f->last_cyc = cyc;
    f->sum_cyc += cyc;
    f->n++;
}

void wm_frame_stats(int full, unsigned long long *last_us, unsigned long long *max_us,
                    unsigned long long *avg_us, unsigned long long *avg_cyc, unsigned *n) {
    const struct frame_stats *f = full ? &g_frame_full : &g_frame_partial;
    if (last_us) *last_us = f->last_us;
    if (max_us)  *max_us  = f->max_us;
    if (avg_us)  *avg_us  = f->n ? f->sum_us / f->n : 0;
    if (avg_cyc) *avg_cyc = f->n ? f->sum_cyc / f->n : 0;
    if (n)       *n       = f->n;
}

void wm_frame_stats_reset(void) {
    struct frame_stats z = { 0, 0, 0, 0, 0, 0 };
    g_frame_full = z;
    g_frame_partial = z;
}

uint32_t wm_scene_frames(void) { return g_scene_frames; }

void wm_render_frame(int mx, int my) {
    g_scene_frames++;
    // The lease decision first: while a client holds the display's
    // buffers this compositor's frame is not on screen, so it is not
    // drawn. Ending a lease inside update() asks for a full repaint,
    // which the rest of this frame then does.
    wm_scanout_update();
    if (wm_scanout_active()) { damage_reset(); return; }

    // After the lease check, so a frame that drew nothing is not timed.
    unsigned long long t0_ns = sys_monotonic_ns();
    unsigned long long t0_cyc = __builtin_ia32_rdtsc();

    compute_window_damage();

    // The FIRST frame of a GUI session is always a full repaint, never
    // damage-limited. wm_run() polls the debug console (and anything
    // else) before its first render, so an event arriving that early
    // reports damage and narrows the one frame that has to establish
    // the whole back buffer -- leaving everything outside it never
    // drawn at all. Caught by `gui damage verify on`: "51200 px changed
    // outside the damage rect, first at (0,0)", 51200 being exactly the
    // 1280x40 strip above a freshly-opened window.
    if (first_frame) {
        first_frame = 0;
        damage_reset();
    }
    // Only when something else already reported damage: with no damage
    // the frame is a full repaint anyway, and adding a rect here would
    // narrow it -- the same trap tray_init() hit (see wm_tray.c).
    if (damage_x1 > damage_x0) damage_cursor(mx, my);

    // Overlays (Start menu, context menu, file picker, confirm dialog)
    // draw OUTSIDE any window's rect and declare no damage of their own
    // -- the design note above calls that "falls back to a full-screen
    // repaint", and for a long time it was true by accident: an overlay
    // frame usually had nothing else reporting damage, so the frame was
    // unrestricted anyway.
    //
    // It stops being true the moment something else declares damage in
    // the SAME frame, and then the fallback silently inverts: the frame
    // becomes damage-limited, the overlay is clipped away, and whatever
    // was on screen before it stays there. Found by
    // tools/damage_sweep.py's random walk (seed 1, step 22): a click
    // that both raised a window and opened Notepad's file picker
    // reported "114932 px changed outside the damage rect, first at
    // (590,173)" -- the damage box was exactly the raised window's rect
    // and the picker was wholly outside it.
    //
    // So make the documented fallback actually hold: while an overlay is
    // up, discard the damage box and repaint the frame in full. That is
    // the correct-by-construction option rather than the fast one, and
    // it is what the compositor's scoped first cut always intended.
    // Giving each overlay a real damage rect of its own is the better
    // end state and needs geometry each of them doesn't expose yet --
    // see docs/roadmap.md's Milestone 12 entry.
    // ...and for one frame AFTER it closes, because the frame that
    // dismisses an overlay has already cleared its `_open` flag by the
    // time this runs, and nothing damages the region the overlay just
    // vacated. That one hid behind a coincidence too: the damage box on
    // such a frame is usually the full-width taskbar strip unioned with
    // the cursor, which covers most of a Start menu sitting just above
    // the taskbar. Dismissing it with a click low on the screen left
    // exactly the rows above that union stale -- "300 px changed
    // outside the damage rect, first at (4,448)", (4,448) being the
    // menu's own top-left corner and 300 being its top two rows.
    //
    // THE START MENU IS NOT IN THIS LIST ANY MORE. It declares its own
    // damage -- its rect when it opens, when it closes, when a click
    // flashes a row and when the hovered row changes (start_menu.c) --
    // so it no longer costs a full-screen repaint per frame for as long
    // as it is up, which is what made the cursor crawl while it was
    // open. The others still opt out, and converting each is the same
    // three steps: track the hover instead of deriving it in the draw,
    // damage the rect on every state change, drop it from here.
    int overlay_now = context_menu_open || calendar_open || confirm_dialog_open;
    if (overlay_now || overlay_was_open) {
        damage_reset();
    }
    overlay_was_open = overlay_now;

    // Clip this pass to the accumulated damage region, if any was
    // reported. No damage this frame (menus, dialogs, the clock tick,
    // the first frame) means "unknown, be safe" -- full screen.
    int has_damage = damage_x1 > damage_x0;

    render_scene(mx, my, has_damage);

    if (verify_enabled && has_damage) {
        // Compare against an unrestricted render of the same frame.
        // Only meaningful when damage WAS reported -- an unrestricted
        // frame is trivially equal to itself.
        if (ugfx_verify_snapshot(&g_wm_screen)) {
            render_scene(mx, my, 0);
            // MASK OUT CLIENT CONTENT, per pixel. Those pixels come from
            // another process's memory and can differ between two
            // renders with nothing wrong -- see client_content_rects().
            // Masking rather than voiding the report is what keeps a
            // difference that spans a client AND the taskbar underneath
            // it: voiding the whole thing hid a deliberately broken
            // taskbar declaration, which is how this was found.
            struct ugfx_skip_rect skip[16];
            int nskip = client_content_rects(skip, 16);
            struct ugfx_diff d;
            int diff = ugfx_verify_diff_masked(&g_wm_screen, &d, skip, nskip);
            if (diff && !verify_reported) {
                verify_reported = 1;
                // THE IDEMPOTENCE PROBE. A difference between the two
                // renders means one of exactly two things, and they need
                // opposite fixes:
                //
                //   (a) the damage-limited render missed a pixel that
                //       genuinely changed -- a real missed declaration,
                //       the bug this whole mode exists to find; or
                //   (b) render_scene() is not a pure function of the
                //       frame's state, so the two renders disagree about
                //       a scene that no damage rect could have covered.
                //
                // (b) is not hypothetical: the unrestricted pass calls
                // every window's on_draw(), while the damage-limited one
                // SKIPS windows outside the damage box (Phase 3 above),
                // so anything an on_draw() mutates happens a different
                // number of times in the two passes.
                //
                // Rendering the same unrestricted frame a THIRD time
                // separates them, in the same frame and under the same
                // load: if it reproduces its own output exactly, the
                // scene is stable and the diff above is a real (a). If
                // it does not, the comparison itself was measuring
                // nothing and (a) cannot be concluded from it.
                struct ugfx_diff again = { 0, -1, -1, 0, 0, 0, 0 };
                int probed = ugfx_verify_snapshot(&g_wm_screen);
                if (probed) {
                    render_scene(mx, my, 0);
                    ugfx_verify_diff_masked(&g_wm_screen, &again, skip, nskip);
                }

                // Which window the difference landed in, and whether
                // that window was inside this frame's damage box at
                // all. "63 px at (349,264)" says nothing on its own;
                // "inside Notepad, which this frame did not damage" is
                // the bug report.
                int owner = -1;
                int cx = (d.x0 + d.x1) / 2, cy = (d.y0 + d.y1) / 2;
                for (int i = window_count - 1; i >= 0; i--) {
                    if (windows[i].state == WIN_MINIMIZED) continue;
                    if (cx >= windows[i].x && cx < windows[i].x + windows[i].w &&
                        cy >= windows[i].y && cy < windows[i].y + windows[i].h) {
                        owner = i;
                        break;
                    }
                }

                // The cursor is the other half of the picture: its box
                // is damaged from prev_cursor_* and (mx,my), and
                // wm_render_cursor_move()'s cheap path draws the sprite
                // WITHOUT recording where it put it -- so a diff sitting
                // on the cursor with prev_cursor_* somewhere else is a
                // different bug from a diff in an app's own pixels.
                wm_logf("wm:   cursor now (%d,%d), prev drawn (%d,%d)\n",
                             mx, my, prev_cursor_x, prev_cursor_y);
                wm_logf("wm:   diff is in %s%s%s, damage-intersecting=%d\n",
                             owner < 0 ? "no window (desktop/taskbar/overlay)" : "window '",
                             owner < 0 ? "" : windows[owner].title,
                             owner < 0 ? "" : "'",
                             owner < 0 ? 0 : window_intersects_damage(&windows[owner]));
                wm_logf("wm: DAMAGE BUG -- %d px changed outside the damage rect, "
                             "first at (%d,%d); damage was (%d,%d %dx%d); "
                             "diff bbox (%d,%d %dx%d); %s\n",
                             diff, d.first_x, d.first_y, damage_x0, damage_y0,
                             damage_x1 - damage_x0, damage_y1 - damage_y0,
                             d.x0, d.y0, d.x1 - d.x0, d.y1 - d.y0,
                             !probed ? "probe unavailable"
                             : again.count == 0 ? "scene stable (real missed damage)"
                             : "SCENE UNSTABLE -- verdict void");
                if (probed && again.count) {
                    wm_logf("wm:   probe -- a repeated unrestricted render of the same "
                                 "frame differs from itself by %d px, first at (%d,%d), "
                                 "bbox (%d,%d %dx%d)\n",
                                 again.count, again.first_x, again.first_y,
                                 again.x0, again.y0, again.x1 - again.x0,
                                 again.y1 - again.y0);
                }
            } else if (!diff) {
                verify_reported = 0; // armed again for the next distinct failure
            }
            // The unrestricted render is left in the back buffer on
            // purpose: it is the CORRECT frame, so verification also
            // repairs what it caught rather than presenting the bug.
        }
    }

    // Record where the cursor was drawn, on EVERY frame -- damaged or
    // not. See damage_cursor()'s comment: this is the bookkeeping that
    // lets the next damage-limited frame erase the sprite from the place
    // it is genuinely sitting, and doing it only on damaged frames is
    // what left a second cursor behind.
    prev_cursor_x = mx;
    prev_cursor_y = my;
    if (wm_hwcursor_active()) {
        // NOTHING was drawn, and the record must say so: a stale box
        // here is a damage rect for a sprite that is not there --
        // harmless -- but a box recorded while the plane later hands
        // BACK to software would erase pixels software never drew.
        prev_cursor_box_w = prev_cursor_box_h = 0;
    } else {
        cursor_rect(resolve_cursor_kind(mx, my), mx, my,
                     &prev_cursor_box_x, &prev_cursor_box_y,
                     &prev_cursor_box_w, &prev_cursor_box_h);
    }

    ugfx_screen_present(&g_wm_screen);

    // `has_damage` is read AFTER render_scene(), which is the only place
    // that knows whether this frame was limited -- and the verify mode
    // is excluded because it renders the scene two or three times.
    if (!verify_enabled) {
        frame_record(has_damage ? &g_frame_partial : &g_frame_full,
                     (sys_monotonic_ns() - t0_ns) / 1000,
                     __builtin_ia32_rdtsc() - t0_cyc);
    }
    damage_reset();
}

void wm_render_cursor_move(int mx, int my) {
    // With the plane showing the pointer a pure motion frame touches
    // no framebuffer pixels at all -- unless a software sprite is
    // still on screen from before the handover, whose restore is the
    // one write worth presenting.
    int had_sprite = cursor_under_valid;
    restore_cursor_under();
    draw_cursor_at(mx, my);

    // This path DREW the cursor, so it has to say where -- prev_cursor_*
    // is "where the sprite actually is", and the next damage-limited
    // frame erases it from there. Leaving it to wm_render_frame() alone
    // meant a cheap move stranded the sprite: the frame damaged the
    // position before the cheap move and the position after it, and
    // never the one in between, so the sprite sat there until something
    // else happened to repaint that region.
    //
    // Consecutive cheap moves hid it (each one restores what the last
    // saved), which is why this survived as "currently harmless" in
    // docs/roadmap.md. It needs a FULL frame to land while the cursor
    // has already moved on -- an injected event overrides the real mouse
    // for one iteration, so a `gui` test's cursor snaps back to the real
    // PS/2 position every other iteration and gets plenty of chances.
    // `gui damage verify on` reported it as 12x19 and 9x15 boxes -- the
    // sprite is 13x19 -- sitting exactly under the cursor, roughly once
    // in five randomised damage_hunt.py sweeps, labelled as whatever
    // interaction happened to be running.
    prev_cursor_x = mx;
    prev_cursor_y = my;
    if (wm_hwcursor_active()) {
        prev_cursor_box_w = prev_cursor_box_h = 0;
        if (!had_sprite) return; // nothing written: nothing to present
    } else {
        cursor_rect(resolve_cursor_kind(mx, my), mx, my,
                     &prev_cursor_box_x, &prev_cursor_box_y,
                     &prev_cursor_box_w, &prev_cursor_box_h);
    }

    ugfx_screen_present(&g_wm_screen);
}
