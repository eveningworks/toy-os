#ifndef UGFX_H
#define UGFX_H

#include <stdint.h>
#include "win_proto.h"

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

// Draws `str` with its top-left at (x, y), alpha-blending each glyph's
// coverage between `bg` and `color` -- the same anti-aliased result the
// kernel's own text has, because it is the same glyph data.
//
// CLIPS to the surface, unlike the kernel's gfx_draw_string(), whose
// not clipping is a documented trap that has caused the same overlap
// bug twice (see docs/gui-guidelines.md). There was no reason to
// reproduce that here.
void ugfx_draw_string(struct ugfx_surface *s, int x, int y,
                       const char *str, uint32_t color, uint32_t bg);

#endif
