// Font Demo -- a font PREVIEWER, and the font-system plumbing probes.
//
// TWO JOBS IN ONE WINDOW.
//
//   1. THE PREVIEWER (what you see and drive). Pick a family from the
//      dropdown, toggle Bold, set a size, and type your own text -- the
//      string is drawn at a ladder of sizes in the chosen face, loaded
//      from a .ttf into this process's own memory (the PRIVATE tier, see
//      ui/ugfx.h). This is the "does font X actually render" tool a real
//      font viewer is (Windows Font Viewer, GNOME Fonts, macOS Font
//      Book all draw a pangram at a range of sizes); the interactivity
//      is what makes it a test rather than a screenshot.
//
//   2. THE PLUMBING PROBES (what a test asserts on). The previewer's own
//      private fonts cannot prove the things that are easy to ship
//      broken and still render readable text -- a bold that is silently
//      regular, kerning that is implemented and never applied. Those are
//      properties of the SESSION font path, so report() still measures
//      the session face in both weights and logs the numbers, exactly as
//      before, and font_test.py still asserts on them.
//
// Log grammar, one line per state change:
//   fontdemo: session regular <w>x<h>
//   fontdemo: session bold <w>x<h> distinct <0|1>
//   fontdemo: session-descender g regular <rows> bold <rows> cell <h>
//   fontdemo: kern "<sample>" plain <w> unkerned <w>
//   fontdemo: preview face "<name>" bold <0|1> base <px> text-w <w>
//   fontdemo: preview size <px> <loaded|failed>
#include "rt/sys.h"
#include "ui/ulog.h"
#include "ui/uapp.h"
#include "ui/ugfx.h"
#include "ui/uui_dropdown.h"
#include "ui/uui_checkbox.h"
#include "ui/uui_spinbox.h"
#include "ui/uui_textbox.h"
#include "ui/uui_focus.h"
#include "ui/utheme.h"
#include "font_ttf.h"   // font_ttf_slot(): the shared slot layout
#include <stdlib.h>
#include <string.h>

// The families offered, each a regular file and (where one exists) a
// bold file. Vera Mono ships no bold face on this disk, so Bold falls
// back to its regular file -- see reload_fonts().
static const struct {
    const char *name;
    const char *regular;
    const char *bold;   // 0 = no bold face; Bold reuses `regular`
} FACES[] = {
    { "Liberation Sans",  "/usr/share/fonts/liberation-sans.ttf",
                          "/usr/share/fonts/liberation-sans-bold.ttf" },
    { "DejaVu Sans Mono", "/usr/share/fonts/dejavu-sans-mono.ttf",
                          "/usr/share/fonts/dejavu-sans-mono-bold.ttf" },
    { "Vera Mono",        "/usr/share/fonts/vera-mono.ttf", 0 },
};
#define FACE_COUNT ((int)(sizeof FACES / sizeof FACES[0]))
static const char *const FACE_NAMES[FACE_COUNT] = {
    "Liberation Sans", "DejaVu Sans Mono", "Vera Mono",
};

// The classic pangram, editable in the field. Every ASCII letter, which
// is exactly what a font preview wants to exercise.
#define PANGRAM "The quick brown fox jumps over the lazy dog"

// The size control: a BASE size, and the ladder is {base, 1.5x, 2x}.
// Bounded so 2x still fits a sensible window -- the window is sized for
// the top of this range in on_size().
#define FD_SIZE_MIN 10
#define FD_SIZE_MAX 28
#define FD_SIZE_DEF 16
#define PREVIEW_ROWS 3

// The kerning sample, unchanged from the plumbing probe: every pair in
// it kerns on liberation-sans (see the `kern` KTEST in
// kernel/drivers/font_face_test.c), so plain vs unkerned differ by
// several pixels rather than by rounding.
#define KERN_SAMPLE "AV To Ta Wa Yo PA"

enum { ID_FAMILY = 1, ID_BOLD, ID_SIZE, ID_TEXT };

#define PAD 8
#define GAP 8
#define DD_W  180
#define SP_W  92

static struct {
    struct uui_dropdown family;
    struct uui_checkbox bold;
    struct uui_spinbox  size;
    struct uui_textbox  text;

    struct uui_item     items[4];
    struct uui_focus    focus;
    struct uui_focusable focus_items[4];

    // The preview fonts: one per ladder rung, rasterised for the current
    // (face, weight, base) and reloaded when any of those change. Text
    // changes need no reload, only a redraw.
    struct ugfx_font font[PREVIEW_ROWS];
    void            *arena[PREVIEW_ROWS];
    int              ok[PREVIEW_ROWS];
    int              px[PREVIEW_ROWS];

    int cur_face, cur_bold, cur_base;   // what font[] was loaded for
    int loaded;                          // deferred first load done?
    int preview_top;                     // y below the controls, from layout()

    // session plumbing measurements, for the log the test reads
    int session_w, session_h, bold_w, bold_h;
} g;


// --- the plumbing probes (session font), kept for font_test.py --------

static int glyph_ink(const struct ugfx_font *f, char c) {
    if (!f || !f->glyphs) return 0;
    int slot = font_ttf_slot((unsigned char)c);
    if (slot < 0 || slot >= f->count) return 0;
    const unsigned char *cell = f->glyphs + (unsigned long)slot * f->char_w * f->char_h;
    int ink = 0;
    for (int i = 0; i < f->char_w * f->char_h; i++) if (cell[i]) ink++;
    return ink;
}

static int glyph_bottom_slack(const struct ugfx_font *f, char c) {
    if (!f || !f->glyphs) return -1;
    int slot = font_ttf_slot((unsigned char)c);
    if (slot < 0 || slot >= f->count) return -1;
    const unsigned char *cell = f->glyphs + (unsigned long)slot * f->char_w * f->char_h;
    for (int row = f->char_h - 1; row >= 0; row--)
        for (int col = 0; col < f->char_w; col++)
            if (cell[row * f->char_w + col]) return f->char_h - 1 - row;
    return -1;
}

// Measures the SESSION font in both weights and logs it. Runs after the
// first load and again on WIN_EV_FONT (the session face can change under
// a running app). Unchanged in substance from the previous Font Demo --
// this is the half font_test.py's font-plumbing checks read.
static void report(void) {
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_REGULAR));
    g.session_w = ugfx_text_width("Handgloves");
    g.session_h = ugfx_char_h();
    ulogf("fontdemo: session regular %dx%d\n", g.session_w, g.session_h);

    ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    g.bold_w = ugfx_text_width("Handgloves");
    g.bold_h = ugfx_char_h();
    // DISTINCT MEASURED IN INK, not width: a designed bold on a MONOSPACE
    // face carries the regular advances, so width reports "distinct 0"
    // for a perfectly working bold. More ink in the same letter is what
    // is true of every bold, monospace or not.
    ulogf("fontdemo: session bold %dx%d distinct %d\n", g.bold_w, g.bold_h,
              glyph_ink(ugfx_font_session(UGFX_FONT_BOLD), 'H')
                  > glyph_ink(ugfx_font_session(UGFX_FONT_REGULAR), 'H') ? 1 : 0);

    ulogf("fontdemo: session-descender g regular %d bold %d cell %d\n",
              glyph_bottom_slack(ugfx_font_session(UGFX_FONT_REGULAR), 'g'),
              glyph_bottom_slack(ugfx_font_session(UGFX_FONT_BOLD), 'g'),
              ugfx_char_h());

    ugfx_set_font(ugfx_font_session(UGFX_FONT_REGULAR));
    int plain = ugfx_text_width(KERN_SAMPLE);
    int unkerned = 0;
    for (int i = 0; KERN_SAMPLE[i]; i++) unkerned += ugfx_char_advance(KERN_SAMPLE[i]);
    ulogf("fontdemo: kern \"%s\" plain %d unkerned %d\n", KERN_SAMPLE, plain, unkerned);

    ugfx_set_font(was);
}

// --- the previewer ----------------------------------------------------

static void log_preview(struct uapp *a);

static void free_fonts(void) {
    for (int i = 0; i < PREVIEW_ROWS; i++) {
        free(g.arena[i]);
        g.arena[i] = 0;
        g.ok[i] = 0;
    }
}

// Rasterises the current family+weight at the size ladder. Called after
// the first frame (see on_draw's deferred load, the blank-window rule
// from the previous version) and on every family/weight/size change.
static void reload_fonts(struct uapp *a) {
    int face = uui_dropdown_selected(&g.family);
    if (face < 0 || face >= FACE_COUNT) face = 0;
    int bold = g.bold.checked ? 1 : 0;
    int base = uui_spinbox_value(&g.size);
    if (base < FD_SIZE_MIN) base = FD_SIZE_MIN;

    const char *path = (bold && FACES[face].bold) ? FACES[face].bold
                                                  : FACES[face].regular;

    g.px[0] = base;
    g.px[1] = base + base / 2;
    g.px[2] = base * 2;

    free_fonts();
    for (int i = 0; i < PREVIEW_ROWS; i++) {
        unsigned long need = ugfx_font_arena_size(g.px[i]);
        g.arena[i] = malloc(need);
        g.ok[i] = g.arena[i] && ugfx_font_load(path, g.px[i], 0,
                                               &g.font[i], g.arena[i], need);
    }

    g.cur_face = face;
    g.cur_bold = bold;
    g.cur_base = base;
    log_preview(a);
}

// Re-emits the whole preview report from the CURRENT state, without
// reloading. Called at the end of a reload AND from on_font() -- a
// session-font change makes a test re-read the log, and the drains
// between test phases mean the preview lines have to be re-stated or a
// reader waiting on "preview face" waits forever. The private preview
// fonts are untouched; this only re-logs what they already are.
static void log_preview(struct uapp *a) {
    for (int i = 0; i < PREVIEW_ROWS; i++)
        ulogf("fontdemo: preview size %d %s\n", g.px[i],
                  g.ok[i] ? "loaded" : "failed");

    // The width of the current text at the base size -- the number that
    // lets a test prove selecting a DIFFERENT family actually rendered a
    // different font (the metrics change), not just relabelled a control.
    int tw = 0;
    if (g.ok[0]) {
        const struct ugfx_font *was = ugfx_set_font(&g.font[0]);
        tw = ugfx_text_width(uui_textbox_text(&g.text));
        ugfx_set_font(was);
    }
    ulogf("fontdemo: preview face \"%s\" bold %d base %d text-w %d\n",
              FACES[g.cur_face].name, g.cur_bold, g.cur_base, tw);

    // Each control's rect (content-relative), so a test drives it by
    // asking rather than guessing pixels -- now the toolkit's job, one
    // `fontdemo: layout <name> x y w h` line per named widget.
    uapp_log_layout(a, "fontdemo");
}

// Places the controls in a row and the preview area below them, from the
// live surface width -- FONT-DERIVED, never a pixel constant
// (docs/gui-guidelines.md), so it reflows if the desktop font changes.
static void layout(int surface_w) {
    int rh = ugfx_char_h() + 10;
    int y = PAD;
    int x = PAD;

    uui_dropdown_set_geometry(&g.family, x, y, DD_W, rh);
    x += DD_W + GAP;
    // The checkbox was init'd with size 0, which uui_checkbox now
    // resolves to the font height at draw/measure time -- so it needs no
    // sizing here, only vertical centring against the control row.
    uui_checkbox_set_geometry(&g.bold, x, y + (rh - ugfx_char_h()) / 2);
    x += g.bold.w + GAP;
    uui_spinbox_set_geometry(&g.size, x, y, SP_W, rh);

    int ty = y + rh + GAP;
    uui_textbox_set_geometry(&g.text, PAD, ty, surface_w - 2 * PAD, rh);

    g.preview_top = ty + rh + GAP + 4;
}

// A change arrived by EITHER path -- a mouse-driven widget (on_widget)
// or a key routed to the focused widget (on_key). Reload the fonts only
// if the face/weight/size moved; a text edit just needs a repaint.
static void apply(struct uapp *a) {
    int face = uui_dropdown_selected(&g.family);
    if (face < 0) face = 0;
    int bold = g.bold.checked ? 1 : 0;
    int base = uui_spinbox_value(&g.size);
    if (g.loaded && (face != g.cur_face || bold != g.cur_bold || base != g.cur_base))
        reload_fonts(a);
    uapp_redraw(a);
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    (void)a;
    struct ugfx_surface *s = d->surface;
    // The toolkit cleared the surface and will paint the controls on top
    // of this. Everything drawn here sits in the preview area below them.
    layout(s->w);

    uint32_t fg = UTHEME_TEXT, bg = UTHEME_PANEL_BG;

    // A hairline under the controls, so the preview reads as its own pane.
    ugfx_fill_rect(s, PAD, g.preview_top - 5, s->w - 2 * PAD, 1,
                   ugfx_rgb(200, 205, 215));

    const char *text = uui_textbox_text(&g.text);
    int y = g.preview_top;

    // The fonts load off the first TICK, not here: the window must draw
    // its controls (toolkit) and this placeholder FAST -- a slow first
    // frame reads as a blank window (blank_window_test catches exactly
    // that). on_tick() does the ~1-2s rasterisation right after this
    // frame is presented, then repaints with the real preview. A
    // uapp_redraw() from here would not do: the event loop blocks between
    // frames, so it would not repaint until the user touched something.
    if (!g.loaded) {
        ugfx_draw_string(s, PAD, y, "Loading fonts...", fg, bg);
        return;
    }

    for (int i = 0; i < PREVIEW_ROWS; i++) {
        if (!g.ok[i]) continue;
        if (y + g.font[i].char_h > s->h) break;   // clip rows past the bottom
        ugfx_set_font(&g.font[i]);
        // Clipped to the content width: the pangram is long and runs off
        // the right at large sizes -- showing as much as fits is what a
        // real font viewer does, rather than forcing a huge window.
        ugfx_draw_string_clipped(s, PAD, y, s->w - 2 * PAD, text, fg, bg);
        y += g.font[i].char_h + 6;
    }
    // Never leave a font selected across a paint -- the toolkit's own
    // widget draw would inherit it.
    ugfx_set_font(0);
}

static void on_widget(struct uapp *a, int id, int reason) {
    (void)id; (void)reason;
    apply(a);
}

// uapp owns the focus ring now (desc.focus): it click-updates it and
// routes keys through it before calling this. So on_key just REACTS to
// whatever the ring may have changed -- reload if the face/weight/size
// moved, otherwise repaint (a textbox edit). apply() reads the widgets'
// current state, so it needs neither the key nor a focus call.
static void on_key(struct uapp *a, int key, unsigned mods) {
    (void)key; (void)mods;
    apply(a);
}

// Loads the fonts once, right after the first frame is on screen -- see
// on_draw's placeholder branch for why this cannot be a uapp_redraw().
// Returns 1 (repaint) only for that one-shot load; idle otherwise, so
// the timer keeps firing but nothing repaints.
static int on_tick(struct uapp *a) {
    (void)a;
    if (g.loaded) return 0;
    g.loaded = 1;
    reload_fonts(a);
    report();
    return 1;
}

static void on_font(struct uapp *a) {
    (void)a;
    // The session measurements are all stale -- re-measure them. The
    // private preview fonts are deliberately untouched, but re-state them
    // too so a reader that drained the log still sees the full report.
    report();
    if (g.loaded) log_preview(a);
}

static void on_open(struct uapp *a) {
    (void)a;
    // Cheap only -- the fonts are loaded after the first frame, see
    // on_draw. uapp creates the window before on_open, so slow work here
    // shows a blank rectangle for as long as it takes.
}

// FONT-DERIVED. Wide enough for the pangram at a middling size (clipped
// past that) and tall enough for the ladder at the TOP of the size range,
// so cranking the spinbox up never pushes the last row off the bottom.
static void on_size(int *w, int *h) {
    *w = ugfx_char_w() * 56;
    if (*w < 620) *w = 620;
    int ladder = FD_SIZE_MAX + (FD_SIZE_MAX + FD_SIZE_MAX / 2) + FD_SIZE_MAX * 2; // ~4.5x
    *h = (ugfx_char_h() + 10) * 2 + GAP * 2 + ladder + PREVIEW_ROWS * 8 + PAD * 3;
}

int main(void) {
    uui_dropdown_init(&g.family, 0, 0, 0, 0, FACE_NAMES, FACE_COUNT);
    uui_checkbox_init(&g.bold, 0, 0, 0, "Bold", UTHEME_PANEL_BG, UTHEME_TEXT);
    uui_spinbox_init(&g.size, FD_SIZE_DEF, FD_SIZE_MIN, FD_SIZE_MAX, 2, "px");
    uui_textbox_init(&g.text, PANGRAM);

    g.items[0] = (struct uui_item){ .ops = &uui_dropdown_ops, .widget = &g.family, .id = ID_FAMILY, .name = "family" };
    g.items[1] = (struct uui_item){ .ops = &uui_checkbox_ops, .widget = &g.bold,   .id = ID_BOLD,   .name = "bold" };
    g.items[2] = (struct uui_item){ .ops = &uui_spinbox_ops,  .widget = &g.size,   .id = ID_SIZE,   .name = "size" };
    g.items[3] = (struct uui_item){ .ops = &uui_textbox_ops,  .widget = &g.text,   .id = ID_TEXT,   .name = "text" };

    g.focus_items[0] = (struct uui_focusable){ &g.text,   &uui_textbox_focus_ops };
    g.focus_items[1] = (struct uui_focusable){ &g.family, &uui_dropdown_focus_ops };
    g.focus_items[2] = (struct uui_focusable){ &g.size,   &uui_spinbox_ops };
    g.focus_items[3] = (struct uui_focusable){ &g.bold,   &uui_checkbox_ops };
    uui_focus_init(&g.focus, g.focus_items, 4);

    struct uapp_desc desc = {
        .title = "Font Demo",
        .app_id = "fontdemo",
        .flags = UAPP_SINGLE_INSTANCE,
        // A cadence purely to get ONE wake shortly after the first frame,
        // where the fonts load (on_tick); the "Loading fonts..."
        // placeholder covers the gap. It keeps firing after that, but
        // on_tick goes idle -- so keep it SLOW (not 60ms), or the idle
        // wakeups add CPU churn that destabilised font_test's own
        // timing-sensitive desktop-font-switch checks while this app sat
        // open beside them.
        .tick_ms = 250,
        .on_size = on_size,
        .on_open = on_open,
        .on_font = on_font,
        .on_tick = on_tick,
        .on_draw = on_draw,
        .widgets = g.items,
        .widget_count = 4,
        .on_widget = on_widget,
        .focus = &g.focus,   // uapp routes clicks/keys through the ring
        .on_key = on_key,
    };
    return uapp_run(&desc);
}
