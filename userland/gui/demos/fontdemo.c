// Font Demo -- the two font tiers, side by side, in one window.
//
// WHY THIS EXISTS. The font work is the kind that is easy to declare
// done and hard to prove: bold that is silently regular, kerning that is
// implemented and never applied, a private face that loads and is never
// selected -- every one of those still draws perfectly readable text.
// This app puts the four cases where a pixel probe can compare them
// against each other, which is the only comparison that means anything
// (an absolute width is a property of the shipped face; a RATIO between
// two ways of drawing the same string is a property of the code).
//
// **THE TWO TIERS** (see ui/ugfx.h for the full contract):
//
//   SESSION -- the desktop's active face, in regular and bold, mapped
//   read-only by the server. Free, shared, and it changes under this app
//   when `fontface`/`fontsize` change. Rows 1 and 2.
//
//   PRIVATE -- a .ttf this process opened and rasterized into its own
//   heap (ugfx_font_load), at a size and face nothing else on the
//   machine is using. Costs this app its own memory and its own
//   rasterization time, and no setting moves it. Row 3.
//
// Row 4 is the kerning control: the same string drawn once normally and
// once with every pair forced apart, so the difference is visible and
// measurable rather than a claim.
//
// Log grammar, one line per state change, so a test can assert on text
// without reading pixels:
//   fontdemo: session regular <w>x<h>
//   fontdemo: session bold <w>x<h> distinct <0|1>
//   fontdemo: private <path> <px> <loaded|failed> <w>x<h>
//   fontdemo: kern <sample> plain <w> unkerned <w>
#include "rt/sys.h"
#include "ui/uapp.h"
#include "lib/stdio.h"
#include <stdarg.h>
#include "lib/stdlib.h"
#include "lib/string.h"

// The face the private tier loads. Deliberately the PROPORTIONAL one
// and deliberately not whatever the session is on: a private font that
// happened to match the session font would look identical on screen and
// prove nothing.
#define PRIVATE_FACE "/usr/share/fonts/liberation-sans.ttf"
#define PRIVATE_PX   24

// The kerning sample. Every pair in it is one liberation-sans kerns
// (see the `kern` KTEST in kernel/drivers/font_face_test.c), so the
// kerned and unkerned widths differ by several pixels rather than by
// rounding.
#define KERN_SAMPLE "AV To Ta Wa Yo PA"

static struct ugfx_font g_private;
static int g_private_ok;

static int g_session_w, g_session_h;
static int g_bold_w, g_bold_h;

// Draws `str` with kerning DISABLED, by drawing one character at a time
// -- a single-character run has no preceding character, so nothing
// kerns. That is the control: it is the same glyphs from the same
// atlas, positioned only by their advances.
static int draw_unkerned(struct ugfx_surface *s, int x, int y,
                          const char *str, uint32_t fg, uint32_t bg) {
    int cx = x;
    for (int i = 0; str[i]; i++) {
        char one[2] = { str[i], 0 };
        ugfx_draw_string(s, cx, y, one, fg, bg);
        cx += ugfx_char_advance(str[i]);
    }
    return cx - x;
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    struct ugfx_surface *s = d->surface;
    (void)a;

    int row_h = ugfx_char_h() * 2;
    int y = 4;

    // --- tier 1, both weights ---------------------------------------
    ugfx_set_font(ugfx_font_session(UGFX_FONT_REGULAR));
    ugfx_draw_string(s, 8, y, "Session regular Handgloves", d->fg, d->bg);
    y += row_h;

    ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    ugfx_draw_string(s, 8, y, "Session bold Handgloves", d->fg, d->bg);
    y += row_h;

    // --- tier 2, this app's own -------------------------------------
    if (g_private_ok) {
        ugfx_set_font(&g_private);
        ugfx_draw_string(s, 8, y, "Private 24px Handgloves", d->fg, d->bg);
        y += ugfx_char_h() + 8;
    } else {
        ugfx_set_font(ugfx_font_session(UGFX_FONT_REGULAR));
        ugfx_draw_string(s, 8, y, "Private face unavailable", d->fg, d->bg);
        y += row_h;
    }

    // --- the kerning control ----------------------------------------
    //
    // BOTH ROWS IN THE SESSION FONT, so the only difference between
    // them is whether pair adjustments were applied. Drawn one above
    // the other and left-aligned, which is what makes the difference
    // in end position visible without measuring anything.
    ugfx_set_font(ugfx_font_session(UGFX_FONT_REGULAR));
    ugfx_draw_string(s, 8, y, KERN_SAMPLE, d->fg, d->bg);
    y += ugfx_char_h() + 2;
    draw_unkerned(s, 8, y, KERN_SAMPLE, d->fg, d->bg);

    // Never leave a font selected across a paint: the next widget or
    // app-level draw would inherit it. Same discipline gfx_set_bold()
    // asks for in ring 0, and the reason ugfx_set_font() returns the
    // previous value.
    ugfx_set_font(0);
}

// Measures and logs everything a test wants to assert on. Called after
// the font is mapped, and AGAIN on WIN_EV_FONT -- the session font can
// change under a running app, and every number below is derived from it.
// Non-zero coverage bytes in one character's cell. The atlas layout is
// ABI (win_proto.h): cells are count x (char_w * char_h) coverage bytes,
// back to back, slot 0 being WIN_FONT_FIRST_CHAR.
static int glyph_ink(const struct ugfx_font *f, char c) {
    if (!f || !f->glyphs) return 0;
    int slot = (int)(unsigned char)c - WIN_FONT_FIRST_CHAR;
    if (slot < 0 || slot >= f->count) return 0;
    const unsigned char *cell =
        f->glyphs + (unsigned long)slot * f->char_w * f->char_h;
    int ink = 0;
    for (int i = 0; i < f->char_w * f->char_h; i++) if (cell[i]) ink++;
    return ink;
}

static void logf_line(const char *fmt, ...) {
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    sys_eprint(line);
}

static void report(void) {
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_REGULAR));
    g_session_w = ugfx_text_width("Handgloves");
    g_session_h = ugfx_char_h();
    logf_line("fontdemo: session regular %dx%d\n", g_session_w, g_session_h);

    ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    g_bold_w = ugfx_text_width("Handgloves");
    g_bold_h = ugfx_char_h();
    // DISTINCT IS THE ASSERTION THAT MATTERS, AND IT IS MEASURED IN INK
    // RATHER THAN IN WIDTH. A bold that fell back to regular -- a failed
    // mapping, a server that answered the wrong weight -- has to be
    // caught, and the obvious probe is that bold text is wider. It is
    // not: on a MONOSPACE face a designed bold has exactly the regular
    // advances, because every cell is one width by definition. Width
    // reports "distinct 0" for a perfectly working bold on the default
    // face, which is worse than not checking.
    //
    // What is true of every bold, monospace or not, is that the same
    // letter carries more ink. The client has the glyph coverage bytes
    // mapped read-only, so it can count them -- the same data the
    // server rasterized, read the same way ugfx_draw_char() reads it.
    logf_line("fontdemo: session bold %dx%d distinct %d\n",
           g_bold_w, g_bold_h,
           glyph_ink(ugfx_font_session(UGFX_FONT_BOLD), 'H')
               > glyph_ink(ugfx_font_session(UGFX_FONT_REGULAR), 'H') ? 1 : 0);

    ugfx_set_font(ugfx_font_session(UGFX_FONT_REGULAR));
    int plain = ugfx_text_width(KERN_SAMPLE);
    int unkerned = 0;
    for (int i = 0; KERN_SAMPLE[i]; i++)
        unkerned += ugfx_char_advance(KERN_SAMPLE[i]);
    logf_line("fontdemo: kern \"%s\" plain %d unkerned %d\n",
           KERN_SAMPLE, plain, unkerned);

    if (g_private_ok) {
        ugfx_set_font(&g_private);
        logf_line("fontdemo: private %s %d loaded %dx%d\n",
               PRIVATE_FACE, PRIVATE_PX, ugfx_text_width("Handgloves"),
               ugfx_char_h());
    } else {
        logf_line("fontdemo: private %s %d failed 0x0\n", PRIVATE_FACE, PRIVATE_PX);
    }
    ugfx_set_font(was);
}

static void on_open(struct uapp *a) {
    (void)a;
    // ONCE, AT STARTUP, NEVER PER FRAME. ugfx_font_load() reads a
    // ~400 KB file and rasterizes 95 glyphs; doing it in on_draw would
    // put that on every paint. The arena is this process's, and the
    // font points into it forever -- so it is never freed.
    unsigned long need = ugfx_font_arena_size(PRIVATE_PX);
    void *arena = malloc(need);
    if (arena)
        g_private_ok = ugfx_font_load(PRIVATE_FACE, PRIVATE_PX, 0,
                                       &g_private, arena, need);
    if (!g_private_ok)
        logf_line("fontdemo: %s would not load -- showing the session font only\n",
               PRIVATE_FACE);
    report();
}

// The session font changed under us (`fontface`, `fontsize`). uapp has
// already re-mapped both weights by the time this runs; everything this
// app measured from them is stale, so it measures again. The PRIVATE
// font is deliberately untouched -- that is the point of it.
static void on_font(struct uapp *a) {
    (void)a;
    report();
}

// FONT-DERIVED, never a pixel constant (docs/gui-guidelines.md): this
// window holds six rows of text, the tallest of them the private font
// at PRIVATE_PX, and it must stay big enough for them at whatever size
// the desktop is on. A fixed 480x260 would be right at 14px and clip at
// 24px -- which is exactly the failure this app exists to make visible,
// so it would be a poor place to hardcode one.
static void on_size(int *w, int *h) {
    *w = ugfx_char_w() * 40;
    if (*w < 380) *w = 380;
    *h = ugfx_char_h() * 8 + PRIVATE_PX * 2 + 24;
}

int main(void) {
    struct uapp_desc desc = {
        .title = "Font Demo",
        .app_id = "fontdemo",
        .on_size = on_size,
        .flags = UAPP_SINGLE_INSTANCE,
        .on_open = on_open,
        .on_font = on_font,
        .on_draw = on_draw,
    };
    return uapp_run(&desc);
}
