#ifndef UGFX_H
#define UGFX_H

#include <stdint.h>
#include "win_proto.h"
#include "geom.h" // enum geom_aa -- the geometry module is SHARED with the kernel

// ugfx -- the userland drawing runtime for ring-3 window clients.
//
// The ring-3 counterpart of the kernel's gfx.c, and deliberately much
// smaller: it draws into a client's OWN window buffer, never the real
// framebuffer, which it has no access to at all. Every primitive here
// is plain arithmetic over that buffer -- no syscalls in the drawing
// path, so a client redraws at memory speed and only crosses into the
// kernel to say WIN_REQ_PRESENT when it's done.
//
// WHY THIS EXISTS AS A LIBRARY, NOT MORE SYSCALLS
// -----------------------------------------------
// The alternative -- a "draw text" syscall the kernel services -- would
// put every client's rendering back inside the kernel, which is the
// thing Milestone 41 is moving away from. Drawing is not a privileged
// operation; only the framebuffer is. So the client draws for itself
// and the kernel only ever composites finished pixels.
//
// The FONT is the one thing a client can't produce for itself: the
// baked glyph tables are ~11,800 lines and live in the kernel image.
// ugfx_font_init() asks the server to map them READ-ONLY (WIN_REQ_FONT)
// rather than linking a copy into every client -- one instance in
// memory, and a client's text can never drift from the desktop's
// current font size. See win_proto.h.
//
// Scope, honestly: rectangles, glyph-accurate text, and the metrics to
// lay them out. There are no widgets here. Porting apps/ui/'s widget
// set is a separate step (see docs/roadmap.md's Milestone 41) -- this
// is the layer such a port would sit on.

struct ugfx_surface {
    uint32_t *pixels; // 32bpp, no padding
    int w, h;         // and therefore the row stride, in pixels

    // --- clip rect: what MAY be touched ---------------------------------
    // Inactive by default (the whole surface is drawable), which is what
    // every window client has always had. Read/written through
    // ugfx_set_clip_rect()/ugfx_clear_clip_rect() -- do not poke these,
    // because "empty" and "inactive" are different states and only the
    // setters keep them distinct. Half-open: [x0,x1) x [y0,y1).
    int clip_x0, clip_y0, clip_x1, clip_y1;
    int clip_active;

    // --- damage box: what WAS touched -----------------------------------
    // A single bounding box of every pixel written since the last
    // ugfx_damage_reset(), maintained by the primitives themselves so no
    // caller marks anything by hand. Empty when x1 <= x0.
    //
    // Always tracked, even on a window surface that has no use for it:
    // one branch per write is cheaper than two versions of every
    // primitive, and it is the kernel gfx.c arrangement this mirrors.
    int dirty_x0, dirty_y0, dirty_x1, dirty_y1;
};

// A surface over memory the CALLER owns -- a client's own window buffer,
// allocated by it and mapped where its mmap put it. Clip inactive,
// damage empty.
struct ugfx_surface ugfx_surface_for_pixels(void *pixels, int w, int h);
// --- clipping ---------------------------------------------------------
//
// Restricts what subsequent drawing MAY touch. The same contract as the
// kernel's gfx_set_clip_rect(), stated once more because getting it
// backwards has cost this project a real bug:
//
//   **A non-positive w or h sets an EMPTY clip -- nothing draws.** It
//   does not mean "no clip". Only ugfx_clear_clip_rect() removes one.
//
// A caller that computes an empty intersection and expects the surface
// to be untouched gets exactly that; one that expects a full-screen
// reset gets a whole frame painted outside its damage region, which is
// invisible until something stale is left on screen.
void ugfx_set_clip_rect(struct ugfx_surface *s, int x, int y, int w, int h);
void ugfx_clear_clip_rect(struct ugfx_surface *s);

// A NESTED clip, for a widget that confines part of its own painting
// (scrolled rows under a header) without losing the clip whoever is
// drawing it already set: save, INTERSECT with the part, paint,
// restore. ugfx_set_clip_rect() would replace the outer clip and
// ugfx_clear_clip_rect() would drop it.
struct ugfx_clip { int x0, y0, x1, y1, active; };
void ugfx_clip_save(const struct ugfx_surface *s, struct ugfx_clip *out);
void ugfx_clip_restore(struct ugfx_surface *s, const struct ugfx_clip *c);
void ugfx_clip_intersect(struct ugfx_surface *s, int x, int y, int w, int h);

// --- pixels -----------------------------------------------------------
//
// The chokepoint every irregular primitive here bottoms out at: bounds,
// clip and damage in one place. Exposed because a compositor legitimately
// pokes single pixels (a cursor sprite, a probe).
void ugfx_put_pixel(struct ugfx_surface *s, int x, int y, uint32_t color);

// Grow the dirty rect to cover a region written by hand. For a
// rasteriser that writes `pixels` directly rather than through the calls
// above -- ui/ugfx_tex.h is the one in this tree. Everything in this
// header already does it for itself.
void ugfx_mark_dirty_rect(struct ugfx_surface *s, int x, int y, int w, int h);

// Reads back what is in the surface. Safe because a surface is ordinary
// cached memory -- a window buffer or a compositor's own back buffer.
// It is NOT a way to read the screen: the framebuffer behind
// ugfx_screen is write-combining, and this never touches it.
// Out of bounds reads as 0.
uint32_t ugfx_get_pixel(const struct ugfx_surface *s, int x, int y);

// Copies a w*h block of 32bpp pixels to (x, y). `src_pitch_px` is the
// SOURCE's row stride in pixels, which is not always `w` -- a caller
// blitting a sub-rectangle out of a larger buffer passes the larger
// buffer's width. Honours the clip and marks damage.
void ugfx_blit(struct ugfx_surface *s, int x, int y, int w, int h,
                const uint32_t *src, int src_pitch_px);

// The same, COMPOSITED: `src` is 0xAARRGGBB (uimg.h's format) and each
// pixel is blended over what the surface already holds, source-over.
// Honours the clip and marks damage exactly as ugfx_blit() does.
//
// A SEPARATE CALL RATHER THAN A FLAG, because the cost difference is
// large and the caller always knows which it wants: a straight copy is a
// memcpy per row, and this is a multiply-add per channel per pixel. A
// wallpaper is opaque and must not pay for it -- `struct uimg` carries
// `has_alpha` so a caller can pick without inspecting the pixels.
//
// Alpha is STRAIGHT, not premultiplied, matching what every decoder
// here produces. Fully opaque pixels take the copy path inside the loop,
// which is what makes an icon that is mostly opaque cost about what a
// blit does.
void ugfx_blit_alpha(struct ugfx_surface *s, int x, int y, int w, int h,
                      const uint32_t *src, int src_pitch_px);
// A `sw` x `sh` source stretched to `w` x `h` at (x, y), NEAREST
// NEIGHBOUR, blended over the surface at one constant `alpha` (255 is a
// plain scaled copy). The source's own alpha byte is ignored. For a
// window animating over a few frames, where a bilinear filter's cost
// would buy nothing anyone sees; not for an image that stays on
// screen -- that is uimg_scale().
void ugfx_blit_scaled_alpha(struct ugfx_surface *s, int x, int y, int w, int h,
                             const uint32_t *src, int sw, int sh, int src_pitch_px,
                             uint8_t alpha);

// The same again, but the source's COLOUR is discarded and only its
// alpha is used, as coverage for `color`. A SYMBOLIC icon: one that
// takes the colour of the text beside it instead of carrying its own,
// which is what GTK's `-symbolic` icons and Windows' MDL2 glyphs are
// for. The reason it is not a nicety: an icon with its ink baked in is
// drawn in one panel's colour and is nearly invisible on another --
// the tray's speaker was toolbar ink on the dark taskbar, at a sixth
// of the contrast of the clock beside it.
void ugfx_blit_tinted(struct ugfx_surface *s, int x, int y, int w, int h,
                       const uint32_t *src, int src_pitch_px, uint32_t color);

// --- damage -----------------------------------------------------------

// The bounding box of everything drawn since the last reset. Returns 0
// and leaves the outputs untouched when nothing has been drawn, so a
// caller can skip a present entirely rather than publishing an empty
// rect. Any output pointer may be NULL.
int ugfx_damage(const struct ugfx_surface *s, int *x, int *y, int *w, int *h);

// Declares everything published. ugfx_screen_present() calls this; a
// caller driving its own publish path calls it after doing so.
void ugfx_damage_reset(struct ugfx_surface *s);

// 0xRRGGBB, matching the buffer's own layout.
void ugfx_fill(struct ugfx_surface *s, uint32_t color);
void ugfx_fill_rect(struct ugfx_surface *s, int x, int y, int w, int h, uint32_t color);
void ugfx_draw_rect(struct ugfx_surface *s, int x, int y, int w, int h, uint32_t color);

// --- fonts ------------------------------------------------------------
//
// **TWO TIERS, ONE HANDLE.** A struct ugfx_font is either a SESSION
// font -- the desktop's active face in one weight, mapped read-only by
// the server, shared by every client and changing under them all when
// `fontface`/`fontsize` change -- or a PRIVATE one an app rasterized
// for itself out of a .ttf (ugfx_font_load), which nothing else can see
// and which no setting moves.
//
// Both are the same type on purpose: a widget takes a font and never
// asks which kind it is, so a heading that starts out bold-from-the-
// session can become 24px-Liberation-private without the widget
// changing. Drawing selects one with ugfx_set_font().
//
// WHY BOTH EXIST, since one would be simpler. The session font is what
// keeps every window's text identical to the desktop's by construction
// -- the property WIN_REQ_FONT was built for -- and it costs a client
// nothing, because the pages are already rasterized and shared. But it
// can only ever offer what the desktop is on: one face, two weights,
// one size. An app wanting a 24px heading beside 11px body text, or a
// second face entirely, cannot be served by a shared atlas without the
// kernel caching every combination any app ever asks for, in a cache
// it can never evict from (clients hold the mappings). So that case
// rasterizes CLIENT-SIDE, which is what every Wayland client does and
// what this OS already had the pieces for -- ttf.c compiles into
// libuapp.a precisely so a ring-3 program can do this.
//
// The trade is explicit: a private font costs the app its own memory
// and its own rasterization time, and does NOT follow the desktop's
// settings. Use the session font unless you specifically need what it
// cannot express.

#define UGFX_FONT_REGULAR 0
#define UGFX_FONT_BOLD    1
#define UGFX_FONT_WEIGHTS 2

// **TWO FAMILIES: a proportional one for the interface and a fixed-cell
// one for terminals and code.** GNOME's `font-name` beside
// `monospace-font-name`, Windows' UI font beside Consolas -- no single
// face can be both, and a grid of cells drawn in a proportional face
// does not line up.
//
// A widget wants the UI family and says nothing: `ugfx_font_session()`
// IS the UI family, which is why adding this changed no call site. Only
// something that genuinely needs a fixed cell names the other one.
#define UGFX_FONT_FAMILY_UI   0
#define UGFX_FONT_FAMILY_MONO 1
#define UGFX_FONT_FAMILIES    2

// The slot a (family, weight) pair lives in, matching
// abi/font_shm.h's FONT_SHM_SLOT -- the two sides of that ABI must
// agree and this is the client's copy of the arithmetic.
#define UGFX_FONT_SLOT(family, weight) ((family) * UGFX_FONT_WEIGHTS + (weight))
#define UGFX_FONT_SLOTS (UGFX_FONT_FAMILIES * UGFX_FONT_WEIGHTS)
#define UGFX_FONT_SLOT_UI_REGULAR UGFX_FONT_SLOT(UGFX_FONT_FAMILY_UI, UGFX_FONT_REGULAR)

struct ugfx_font {
    const unsigned char *glyphs;   // count cells of char_w x char_h coverage
    const unsigned char *advances; // count bytes, or NULL for a fixed cell
    const signed char   *kern;     // count x count, or NULL -- see ugfx_kern()
    int char_w;

    // **THE BITMAP IS TALLER THAN THE LINE.** `char_h` is how many rows
    // a glyph's coverage map has -- the stride between cells, so it is
    // what INDEXING uses and the only value win_glyph_offset() may be
    // given. `line_h` is how far apart two lines of text sit, which is
    // what LAYOUT uses and what ugfx_char_h() returns.
    //
    // The difference is the descender: the cell extends below the
    // baseline far enough to hold a 'g' tail, while the pitch stays at
    // the height everything is laid out against, so nothing reflowed
    // when the tails appeared. A glyph therefore PAINTS BELOW ITS LINE,
    // which is ordinary (FreeType, Pango and CoreText all separate the
    // line box from a glyph's ink extent) and has one consequence worth
    // knowing: anything painting an opaque background over the row below
    // will erase a descender. A widget that wants to keep its tails
    // reserves the overhang -- uui_label does.
    //
    // Equal for the BAKED font, whose bitmaps were rasterized squeezed
    // at build time; it still clips.
    int char_h;   // rows per glyph bitmap -- the indexing stride
    int line_h;   // rows between lines -- what layout uses
    int count;
};

// Asks the server for the shared font. Must succeed before any text
// call below; returns 1 on success, 0 if the server refused (no
// desktop session). Safe to call again after a font-size change -- it
// re-reads the metrics and the mapping is idempotent.
int ugfx_font_init(void);

// Whether /bin/fontd has republished the session font since the last
// call, re-mapping it if so. A memory read rather than a syscall, so an
// app may ask every frame; uapp does. See abi/font_shm.h's beacon.
int ugfx_font_recheck(void);

// The face /bin/fontd published for the session's regular UI font, or
// NULL when this client draws the kernel's baked tables instead (no
// fontd, or a `builtin` face). /bin/font uses it to tell "different
// fonts, by design" from a mapping that disagrees with its source.
const char *ugfx_font_session_face(void);

// The fontd beacon generation this process has mapped, 0 before any.
// Compared with `diag font`'s, it says whether a client has caught up
// with a republish -- fontd bumping is not the screen having changed.
uint32_t ugfx_font_generation(void);

// Font metrics, valid once ugfx_font_init() has succeeded. Both are 0
// before that, which is what makes a forgotten init show up as text
// that doesn't draw rather than as a wild pointer.
// How far the pen moves after drawing one character: the cell width on
// the baked font or any monospace face, genuinely per-glyph on a
// proportional one (see ugfx_font_init(), which picks the advance table
// up out of the same mapping the glyphs arrive in). ugfx_text_width()
// and ugfx_text_fit_chars() are both built on it, which is why a client
// that asks THEM rather than multiplying by ugfx_char_w() needed no
// change at all when proportional faces became loadable.
int ugfx_char_advance(char c);

// The glyph BITMAP height -- taller than ugfx_char_h() (the line pitch)
// by however much descender overhang the font has. Needed only by code
// that indexes glyph bytes or reserves room for a tail; everything doing
// layout wants ugfx_char_h().
int ugfx_glyph_h(void);

// One of the two session fonts. Valid after ugfx_font_init(); the bold
// one falls back to a copy of regular on a machine whose font has no
// bold weight (the baked font), so this never returns something
// undrawable.
const struct ugfx_font *ugfx_font_session(int weight);

// The MONOSPACE family, same contract: mapped on first use, and a
// failure returns the UI family's regular weight rather than nothing,
// so a terminal whose mono face will not load draws text in the wrong
// face instead of drawing none.
//
// **ASK FOR THIS WHENEVER A FIXED CELL IS PART OF THE MEANING** -- a
// terminal grid, a hex dump, a code view. Everything else wants
// ugfx_font_session().
const struct ugfx_font *ugfx_font_mono(int weight);

// Makes `f` the font every subsequent text call draws and measures
// with, RETURNING THE PREVIOUS ONE so save/restore is the shortest
// thing to write:
//
//     const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
//     ugfx_draw_string(...);
//     ugfx_set_font(was);
//
// NULL (or an unloaded font) means the session's regular weight, not
// "no font" -- a widget that forgot to restore would otherwise measure
// everything after it as zero-width and collapse the layout, which is
// far harder to see than text in the wrong weight.
const struct ugfx_font *ugfx_set_font(const struct ugfx_font *f);
const struct ugfx_font *ugfx_font_current(void);

// The kerning adjustment in pixels between two adjacent characters, 0
// when the current font does not kern them. Every measuring and drawing
// path in ugfx applies this already; a client drawing its own runs
// character by character must too, or its text will not match what
// ugfx_text_width() said.
int ugfx_kern(int prev, int c);

// --- a private font, rasterized by this app (tier 2) ------------------
//
// Loads `path` (a .ttf on the filesystem) and rasterizes the same
// 191-slot glyph set the session font uses, at `px`, into memory the
// CALLER owns. Returns 1 on success.
//
// `arena` must be at least ugfx_font_arena_size(px) bytes and must stay
// alive and unmodified for as long as `f` is drawn with -- the font
// points into it and copies nothing. `bold` embolden the outlines when
// the file is not already a bold face.
//
// **THIS IS NOT CHEAP AND IS NOT A PER-FRAME CALL.** It reads a
// several-hundred-KB file and fills 101 glyphs; do it once, at startup
// or when the app's own settings change, and keep the handle. It is
// also the one place a client needs real memory: an atlas at 14px is
// ~24 KB and at 32px ~130 KB.
//
// It needs no window and no compositor -- rasterizing is arithmetic
// over a file, which is exactly why ttf.c can be the same source in
// both rings.
int ugfx_font_load(const char *path, int px, int bold,
                    struct ugfx_font *f, void *arena, unsigned long arena_size);

// Twice the session's height, bold Liberation Sans -- the DISPLAY size
// a clock or a headline figure is drawn at (System Settings' Date &
// time, the tray calendar). Private and cached; re-rasterized when the
// session's cell height changes. Never NULL.
const struct ugfx_font *ugfx_font_display(void);

// Two thirds of the session's height, bold -- the CAPTION size a count
// badge or a small figure on an icon is drawn at (the taskbar's group
// count). Same contract as ugfx_font_display(): private, cached,
// re-rasterized when the session's height changes, never NULL.
const struct ugfx_font *ugfx_font_caption(void);

// Bytes ugfx_font_load() needs at `px`. Sized for the worst case at
// that size, so a caller can allocate before knowing which face it will
// get -- a face whose glyphs turn out narrower simply uses less.
unsigned long ugfx_font_arena_size(int px);

int ugfx_char_w(void);
int ugfx_char_h(void);

// Width in pixels of `s` rendered by ugfx_draw_string(). The interface
// face is PROPORTIONAL, so `length * char_w` is not this number -- it
// is the width of the widest string of that length, and a caller using
// it draws its labels on top of each other.
int ugfx_text_width(const char *str);

// The same for the first `n` characters (`n < 0` means all of it, which
// is what ugfx_text_width() is). Kerning is counted INSIDE the prefix,
// so this agrees with drawing that prefix on its own: a field that
// windows a long value must measure the SLICE it draws, not an offset
// into the whole string.
int ugfx_text_width_n(const char *str, int n);

// --- the rest of the text chokepoints ---------------------------------
//
// The ring-3 half of the kernel's gfx_text_* set, same rule and same
// reasoning (see kernel/include/api/gfx.h, which states it in full):
// **no widget does character arithmetic on a string itself** -- not
// `len * ugfx_char_w()` to measure it, not `i + 1` to step a cursor.
// Those are
// the sites that have to change when text stops being one byte per
// fixed cell, and routed through here that migration is four functions
// plus the glyph lookup instead of a hunt through every widget.
//
// Not a ban on ugfx_char_w() as a LAYOUT UNIT -- padding of
// `char_w / 2`, a `char_w + 4` scrollbar, `w / char_w` grid columns are
// all "size this proportionally to the font" and stay. The banned
// pattern is measuring or indexing A STRING with it.

// Draw `str` in `max_w` pixels, MARKING it when it did not fit: the
// text is cut short and `..` drawn after it, so a truncated line reads
// as truncated instead of as a shorter sentence. Returns 1 when it had
// to elide.
//
// `..` and not U+2026: the font is indexed from ASCII 32
// (kernel/drivers/font_ttf.c), so an ellipsis glyph draws as NOTHING --
// which would make an elided line indistinguishable from a complete
// one, the exact failure this exists to stop. The desktop's icon
// captions have done it this way since they were written; this is that
// rule, in the one place every caller can reach it.
int ugfx_draw_string_elided(struct ugfx_surface *s, int x, int y, int max_w,
                            const char *str, uint32_t color, uint32_t bg);

// The same rule into a BUFFER, for text measured now and drawn later --
// a title inside a sentence that must keep its end: `src` as it fits in
// `max_w` pixels and `cap` bytes, ending in `..` when cut, and keeping
// at least ONE letter before the mark (cap allowing) -- so it may then
// be wider than `max_w`. Returns 1 when it cut.
int ugfx_text_elide(char *dst, int cap, const char *src, int max_w);

// How many leading characters of `str` fit within `max_w` pixels, whole
// glyphs only -- the measurement half of ugfx_draw_string_clipped(),
// for a caller doing its own windowing (a field scrolling to follow its
// cursor) that needs the count rather than the drawing.
int ugfx_text_fit_chars(const char *str, int max_w);

// Step one character forward/back from `i`, clamped to [0, length].
// Trivial today and deliberately still a function: these are the two
// places a multi-byte encoding has to skip a sequence rather than a
// byte.
int ugfx_text_next(const char *str, int i);
int ugfx_text_prev(const char *str, int i);

// Which character boundary of `str` sits at `x` pixels from its start
// -- the inverse of ugfx_text_width_n(), and what places a caret by
// clicking. Rounds to the NEAREST boundary, so the right half of a
// glyph selects the position after it.
//
// The kernel half stays absent (gfx.h says why): the console is a fixed
// grid and has no caller. This one has uui_textbox, which placed its
// caret by dividing by the cell until the interface face stopped being
// monospace.
int ugfx_text_index_at_x(const char *str, int x);

// Draws `str` with its top-left at (x, y), alpha-blending each glyph's
// coverage between `bg` and `color` -- the same anti-aliased result the
// kernel's own text has, because it is the same glyph data.
//
// CLIPS to the surface, unlike the kernel's gfx_draw_string(), whose
// not clipping is a documented trap that has caused the same overlap
// bug twice (see docs/gui-guidelines.md). There was no reason to
// reproduce that here.
// TEXT OVER SOMETHING ALREADY DRAWN. Pass this as `bg` to any of the
// three text calls below and each glyph's partial coverage blends
// against WHAT IS ON THE SURFACE rather than against a colour the
// caller had to guess.
//
// It exists because a guess is wrong the moment the backdrop stops
// being flat. The desktop's icon labels and its version watermark both
// passed the old flat desktop blue as `bg`, which was exactly right
// until wallpapers arrived -- after which every anti-aliased glyph edge
// carried a dark halo of a colour nothing on screen had any more, and
// the text read as smeared rather than as dim.
//
// NOT a colour: 0xRRGGBB uses the low three bytes, so a value with the
// top byte set cannot collide with one.
//
// TWO COSTS, both real. It READS THE SURFACE BACK, so it is not usable
// on a write-only target and is slower than the opaque path -- every
// other caller keeps the no-read-back contract unchanged. And it makes
// the text's legibility depend on a backdrop the caller does not
// control, which is what ugfx_draw_string_shadowed() is for.
#define UGFX_TRANSPARENT 0xFF000000u

// One glyph at (x, y), alpha-blended between `bg` and `color`. The
// per-cell primitive a text widget needs -- ugfx_draw_string() is a
// loop over this.
void ugfx_draw_char(struct ugfx_surface *s, int x, int y, char c,
                     uint32_t color, uint32_t bg);

void ugfx_draw_string(struct ugfx_surface *s, int x, int y,
                       const char *str, uint32_t color, uint32_t bg);

// Same, but also stops at `max_w` pixels from `x` -- for a label inside
// a control of known width. Returns 1 if the whole string fitted, 0 if
// it was cut short, so a caller can react (shorten, ellipsise) rather
// than silently overflowing its own layout.
int ugfx_draw_string_clipped(struct ugfx_surface *s, int x, int y, int max_w,
                              const char *str, uint32_t color, uint32_t bg);

// TEXT THAT STAYS LEGIBLE ON A BACKDROP THE CALLER DOES NOT OWN: the
// string drawn twice, once offset by a pixel in a contrasting shade and
// once in `color`, both transparently.
//
// This is what every desktop does with icon labels over a wallpaper --
// macOS, GNOME and KDE all shadow them, and Windows outlines them --
// because a user-chosen photograph can be any colour and no single ink
// works on all of them. The shadow's direction is fixed (down-right,
// the conventional light source) and its shade is picked from `color`'s
// own luminance, so light text gets a dark shadow and dark text a light
// one without the caller deciding.
//
// Costs two passes over the glyphs. Use it for text on a wallpaper, not
// for text in a widget -- a widget knows its own background.
void ugfx_draw_string_shadowed(struct ugfx_surface *s, int x, int y,
                                const char *str, uint32_t color);
int ugfx_draw_string_clipped_shadowed(struct ugfx_surface *s, int x, int y,
                                       int max_w, const char *str, uint32_t color);

// --- colour ----------------------------------------------------------
//
// The kernel's gfx.c equivalents decode the framebuffer's actual pixel
// format at runtime (channel positions and widths vary by mode). These
// don't need to: a client's window buffer is 32bpp 0xRRGGBB by protocol
// definition (abi/win_proto.h), so the layout is fixed and known at
// compile time. Simpler, and correct for the only format a client ever
// sees.

uint32_t ugfx_rgb(uint8_t r, uint8_t g, uint8_t b);

// `under` mixed toward `over` by `alpha`/255.
uint32_t ugfx_blend(uint32_t under, uint32_t over, uint8_t alpha);

// Perceived brightness, 0..255. Used to decide which WAY to shift a
// colour for a hover/pressed state -- see uui.h, where getting this
// backwards produced a hover nobody could see.
uint8_t ugfx_luminance(uint32_t color);

// --- geometry ---------------------------------------------------------
//
// One line each over kernel/lib/geom.c, which is compiled a second time
// for userland (build/userland/shared/). The kernel's gfx.h has the
// same set backed by the same code -- there is exactly one Bresenham
// and one ellipse rasteriser in the tree.
//
// `aa` is per call, not a mode: a wireframe's diagonals want
// anti-aliasing (jaggies crawl as a shape rotates) and a 1px border
// does not.

void ugfx_draw_line(struct ugfx_surface *s, int x0, int y0, int x1, int y1,
                     uint32_t color, enum geom_aa aa);
void ugfx_draw_polyline(struct ugfx_surface *s, const int *xs, const int *ys,
                         int count, int closed, uint32_t color, enum geom_aa aa);
void ugfx_draw_circle(struct ugfx_surface *s, int cx, int cy, int r,
                       uint32_t color, enum geom_aa aa);
void ugfx_draw_ellipse(struct ugfx_surface *s, int cx, int cy, int rx, int ry,
                        uint32_t color, enum geom_aa aa);
// THE FILLS ARE ANTI-ALIASED, for every caller -- Cairo's and
// Direct2D's default, chosen 2026-10-01 after a play button and a
// speaker icon read as jagged (ugfx_fill.c). The footprint is the
// aliased fill's: an integer point is a pixel's CENTRE, and a circle
// reaches r + 1/2 from it; only the edge pixels differ, blended by
// coverage. A polygon is even-odd, at most 256 vertices.
//
// **NOT FOR A MESH.** Two anti-aliased faces sharing an edge each blend
// it half way, and the background shows through as a seam -- which is
// why uui_canvas (Shapes, the teapot) still fills through geom_fill_*,
// whose pixel-centre rule lets faces meet exactly.
void ugfx_fill_circle(struct ugfx_surface *s, int cx, int cy, int r, uint32_t color);
void ugfx_fill_ellipse(struct ugfx_surface *s, int cx, int cy, int rx, int ry,
                        uint32_t color);
void ugfx_fill_polygon(struct ugfx_surface *s, const int *xs, const int *ys,
                        int count, uint32_t color);

// A filled annulus sector -- what a ring gauge is made of. Angles are
// TURNS (fixed.h), turn 0 at 3 o'clock, positive going clockwise on
// screen. See geom_fill_ring().
void ugfx_fill_ring(struct ugfx_surface *s, int cx, int cy,
                     int r_outer, int r_inner, fx_t from, fx_t to,
                     uint32_t color);

// Alpha-blends one pixel into the surface. The geometry above uses it
// for partial coverage; exposed because a client drawing its own
// gradients or shadows wants the same thing.
void ugfx_blend_pixel(struct ugfx_surface *s, int x, int y, uint32_t color, uint8_t alpha);

// A RUN of `w` pixels of `color` along row `y` from `x`, taking each
// pixel's coverage from `cov` -- or, when `cov` is NULL, one constant
// `alpha` for the whole run. Clipped and dirty-marked ONCE per run
// rather than once per pixel, which is the whole reason it exists: the
// drop shadows walk a window's perimeter every frame, and a call per
// pixel there cost more than the blending did.
// `cov` is indexed from the UNCLIPPED `x`, so clipping a run does not
// shift its coverage.
void ugfx_blend_hspan(struct ugfx_surface *s, int x, int y, int w,
                       uint32_t color, const uint8_t *cov, uint8_t alpha);

// THE GENIE. The source squeezed into a vertical tube: the band
// [y0, y0+h) is filled row by row, each row's width and centre eased
// between the span at the top and the span at the bottom with a
// smoothstep, and each row taking its pixels from the corresponding
// row of the source. The whole source is always inside the tube --
// compressed, never cropped.
//
// It is macOS's Genie and KWin's Magic Lamp. A MESH would be the
// general version; one span per ROW is enough for a tube that only
// narrows vertically, and costs one sample per destination pixel --
// the same class as ugfx_blit_scaled_alpha(), and cheaper in practice
// because the tube is narrower than the window.
void ugfx_blit_genie(struct ugfx_surface *s, int y0, int h,
                      int top_cx, int top_w, int bot_cx, int bot_w,
                      const uint32_t *src, int sw, int sh, int src_pitch_px,
                      uint8_t alpha);

// --- glass (ugfx_blur.c) ----------------------------------------------
//
// A BACKDROP is a buffer the size of the surface, in the surface's own
// coordinates, holding what a glass rect shows through it -- the scene
// blurred, or a cached blurred wallpaper. Same coordinates so a caller
// never offsets a pointer into it for a rect that is half off-screen.

// The surface's pixels in the rect, BLURRED into `out` (a backdrop) --
// three box passes each way, about a Gaussian of sigma `radius`. READS
// ONLY THE RECT INSIDE THE CLIP and clamps at its edge: a pixel outside
// the clip holds a previous frame, perhaps already glass, and blurring it
// again would drift a little every frame. So the caller damages the
// WHOLE rect whenever any of it changes (wm_glass.c does).
void ugfx_blur_rect(const struct ugfx_surface *s, int x, int y, int w, int h,
                    int radius, uint32_t *out);

// A GLASS CLIENT's pixels at (x, y), each one either copied or, when its
// top byte is set, shown as GLASS: its low 24 bits blended at `alpha`
// over `backdrop` (NULL: over what the surface already holds). abi/
// win_proto.h's WIN_POPUP_GLASS is the contract; honours the clip.
void ugfx_blit_glass(struct ugfx_surface *s, int x, int y, int w, int h,
                     const uint32_t *src, int src_pitch_px,
                     const uint32_t *backdrop, uint8_t alpha);

// --- the screen -------------------------------------------------------
//
// A ring-3 COMPOSITOR's view of the real display: an ordinary
// ugfx_surface to draw into, plus the publish path that gets those
// pixels onto the glass. Milestone 41 stage 4b -- see
// docs/wm-ring3-design.md's R1, which decided that the back buffer
// belongs to the compositor rather than staying in the kernel.
//
// Three things about this are not obvious and all three fail quietly:
//
// 1. **The mapped framebuffer is WRITE-COMBINING, so it is never read.**
//    Writes coalesce into bursts; a read is a full uncached round trip
//    with no cache fill and no prefetch. So the compositor draws into a
//    private back buffer of normal memory and copies OUT. Every drawing
//    call here targets `back`; nothing in this header reads the screen.
//
// 2. **Present is required, not advisory.** A display_driver may declare
//    DISPLAY_CAP_NEEDS_FLUSH (vmsvga does), where written pixels stay
//    invisible until the adapter is told which region changed. So a
//    frame is not finished until ugfx_screen_present() runs, and on a
//    continuously-scanned adapter the kernel's flush is already a no-op
//    -- one code path serves both.
//
// 3. **The back buffer comes from SYS_SBRK**, which only ever grows, so
//    a screen is initialised once and lives for the process. There is no
//    ugfx_screen_free() because there is nothing that could give the
//    pages back.
#define UGFX_SCREEN_BUFFERS 3
#define UGFX_DAMAGE_RING    4

struct ugfx_screen {
    // Draw here. Its `w`/`h` are the screen's -- deliberately NOT
    // repeated as fields on this struct, because two copies of one
    // number is one copy too many and the duplicate is the one that
    // goes stale if a screen ever gets a back buffer of a different
    // size (scaling, rotation).
    struct ugfx_surface back;

    // What the back buffer does NOT describe: how the pixels are laid
    // out on the far side of the publish. `pitch` is in BYTES and is not
    // always w*bpp/8 -- an adapter may pad rows, and assuming it does
    // not writes a sheared picture on the machines where it matters.
    uint32_t pitch;
    int bpp;

    // The damage-verify comparison copy, or NULL if never taken. Held
    // here rather than in a file-global so the type says what the API
    // already implies: verification is per screen. `snapshot_valid`
    // separates "allocated" from "holds a frame worth comparing", which
    // is what lets release() keep the memory without leaving a stale
    // frame that a later diff would happily compare against.
    uint32_t *snapshot;
    int snapshot_valid;

    // The scanouts the grant mapped (1, or 3 on a display that flips)
    // and which one to draw into next -- WIN_REQ_FB_PRESENT hands back
    // the index each frame. The buffer handed back was last painted
    // some frames ago, so a present repaints the union of every
    // frame's damage since then (Wayland's buffer_age): `seq` numbers
    // the presents, `painted_seq` is when each buffer was last drawn,
    // and `dmg` is a ring of recent frames' damage boxes. A buffer
    // never painted, or older than the ring, gets the whole screen.
    int buffers;
    int back_index;
    uint32_t seq;
    uint32_t painted_seq[UGFX_SCREEN_BUFFERS];
    int dmg_x[UGFX_DAMAGE_RING], dmg_y[UGFX_DAMAGE_RING];
    int dmg_w[UGFX_DAMAGE_RING], dmg_h[UGFX_DAMAGE_RING];
    // For `gui fb`: how many presents, and how many changed the index.
    uint32_t presents, flips;
    // The back buffer's capacity in pixels: sbrk only grows, so a smaller
    // mode keeps the larger allocation and a larger one extends it.
    uint32_t back_capacity;
};

// Maps the real framebuffer and allocates a matching back buffer.
// Returns 1 on success, 0 if the grant was refused (the caller is not
// the registered compositor -- see WIN_REQ_SET_COMPOSITOR), if the
// reported format is one this cannot drive, or if the back buffer did
// not fit in the heap.
//
// Refusal is the normal outcome for any process that is not the
// compositor, so it is a return value rather than a fault.
int ugfx_screen_init(struct ugfx_screen *sc);

// Copies the damaged box of the back buffer out to the framebuffer and
// publishes it, then clears the damage. A no-op when nothing was drawn,
// which is what makes calling it every frame free.
void ugfx_screen_present(struct ugfx_screen *sc);
// After somebody else flipped these buffers (a lease, WIN_REQ_FB_LEASE):
// draw into `back` next, and treat every buffer as never painted, so
// the next present copies the whole screen rather than a damage union
// that assumes the buffers still hold this compositor's frames.
void ugfx_screen_forget(struct ugfx_screen *sc, int back);

// A TEST LEVER (`gui present full on`): present the WHOLE screen every
// frame rather than the buffer-age union, on a display that flips
// between several buffers. Inert where there is one buffer -- which is
// every machine the automated suite runs on, so the union path it
// bypasses has no coverage there and a screenshot cannot see its output
// (wm_screenshot.c copies the BACK buffer, and renders a frame first).
// It exists so a human on flip-capable hardware can tell a catch-up bug
// from one further down the present path, and costs a full-screen copy
// per frame while it is on.
void ugfx_screen_present_full(int on);
int  ugfx_screen_present_full_get(void);

// After WIN_EV_SCREEN: re-maps the grant at its new geometry, grows the
// back buffer if the mode did, and forgets every damage box, which was
// in the old coordinates. Returns 1, or 0 if the grant was refused.
int ugfx_screen_remode(struct ugfx_screen *sc);

// --- damage verification (R2) -----------------------------------------
//
// The ring-3 half of the kernel's gfx_verify_* set: snapshot the back
// buffer, re-render whatever is under suspicion, and diff. This is the
// only harness this project has for the compositor's worst bug class --
// a change on screen that was never declared as damage, which leaves
// stale pixels with no crash and no assertion.
//
// The diff is over the BACK BUFFER, never the screen, for the
// write-combining reason above -- which is also why it can be exact
// rather than sampled.
struct ugfx_diff {
    int count;             // pixels that differ
    int first_x, first_y;  // first difference in scan order, -1 if none
    int x0, y0, x1, y1;    // bounding box of the differences, half-open
    // WHAT the first difference was and became. A count and a box say
    // that something changed; these say WHAT, which is the difference
    // between "the panel was not drawn in one pass" (wallpaper vs
    // chrome) and "it was drawn twice" (a tone shifting by a little).
    // Chasing a damage fault without them cost a session of guessing.
    uint32_t first_was;    // the snapshot's pixel
    uint32_t first_now;    // the back buffer's
};

// Takes the comparison copy. Returns 1 on success, 0 if the scratch
// buffer could not be allocated -- which is a real possibility at large
// resolutions, since the scratch is a second full screen (see
// kernel/uaddr.h on the heap's ceiling). Failing to snapshot must read
// as "not verified", never as "verified clean".
int ugfx_verify_snapshot(struct ugfx_screen *sc);

// Compares the back buffer against the snapshot. Returns the count.
// Zero without a snapshot, with `out` zeroed -- see above.
int ugfx_verify_diff(struct ugfx_screen *sc, struct ugfx_diff *out);

// A rectangle to leave OUT of the comparison.
struct ugfx_skip_rect { int x, y, w, h; };

// The same comparison, ignoring any pixel inside one of `skip`.
//
// It exists because a caller can have regions whose content is not its
// own to hold still -- the window manager composites client windows out
// of another process's memory, which that process may rewrite at any
// moment, so those pixels can differ between two renders with nothing
// wrong. Masking them PER PIXEL rather than voiding the whole report is
// the point: a single difference spanning a client's content AND the
// taskbar underneath it would otherwise throw away the half that is
// genuinely verifiable, which is exactly how a deliberately broken
// taskbar declaration went undetected once.
int ugfx_verify_diff_masked(struct ugfx_screen *sc, struct ugfx_diff *out,
                             const struct ugfx_skip_rect *skip, int nskip);

// Drops the snapshot. The scratch memory is KEPT, unlike the kernel's
// gfx_verify_release(), which kfree()s it: sbrk cannot return pages, so
// releasing would leak the address range and then allocate a second one
// on the next snapshot. Keeping it makes repeated verification cost one
// allocation for the life of the process.
void ugfx_verify_release(struct ugfx_screen *sc);

#endif
