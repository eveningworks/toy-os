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
};

// Wraps a window's buffer as a drawable surface. `window` is the id
// WIN_REQ_CREATE handed back.
struct ugfx_surface ugfx_surface_for_window(uint32_t window, int w, int h);

// 0xRRGGBB, matching the buffer's own layout.
void ugfx_fill(struct ugfx_surface *s, uint32_t color);
void ugfx_fill_rect(struct ugfx_surface *s, int x, int y, int w, int h, uint32_t color);
void ugfx_draw_rect(struct ugfx_surface *s, int x, int y, int w, int h, uint32_t color);

// Asks the server for the shared font. Must succeed before any text
// call below; returns 1 on success, 0 if the server refused (no
// desktop session). Safe to call again after a font-size change -- it
// re-reads the metrics and the mapping is idempotent.
int ugfx_font_init(void);

// Font metrics, valid once ugfx_font_init() has succeeded. Both are 0
// before that, which is what makes a forgotten init show up as text
// that doesn't draw rather than as a wild pointer.
int ugfx_char_w(void);
int ugfx_char_h(void);

// Width in pixels of `s` rendered by ugfx_draw_string(). Monospaced
// today, so it is length * char_w -- but call this rather than doing
// that multiplication, exactly as kapi's gfx_text_width() exists for:
// the identity stops holding the moment a proportional face appears,
// and callers that assumed it are then wrong everywhere at once.
int ugfx_text_width(const char *str);

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

// An index-at-x is deliberately absent here too -- see gfx.h.

// Draws `str` with its top-left at (x, y), alpha-blending each glyph's
// coverage between `bg` and `color` -- the same anti-aliased result the
// kernel's own text has, because it is the same glyph data.
//
// CLIPS to the surface, unlike the kernel's gfx_draw_string(), whose
// not clipping is a documented trap that has caused the same overlap
// bug twice (see docs/gui-guidelines.md). There was no reason to
// reproduce that here.
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
void ugfx_fill_circle(struct ugfx_surface *s, int cx, int cy, int r, uint32_t color);
void ugfx_fill_ellipse(struct ugfx_surface *s, int cx, int cy, int rx, int ry,
                        uint32_t color);

// Alpha-blends one pixel into the surface. The geometry above uses it
// for partial coverage; exposed because a client drawing its own
// gradients or shadows wants the same thing.
void ugfx_blend_pixel(struct ugfx_surface *s, int x, int y, uint32_t color, uint8_t alpha);

#endif
