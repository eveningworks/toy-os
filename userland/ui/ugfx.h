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

// Wraps a window's buffer as a drawable surface. `window` is the id
// WIN_REQ_CREATE handed back. Clip inactive, damage empty.
struct ugfx_surface ugfx_surface_for_window(uint32_t window, int w, int h);

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

// --- pixels -----------------------------------------------------------
//
// The chokepoint every irregular primitive here bottoms out at: bounds,
// clip and damage in one place. Exposed because a compositor legitimately
// pokes single pixels (a cursor sprite, a probe).
void ugfx_put_pixel(struct ugfx_surface *s, int x, int y, uint32_t color);

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

// Asks the server for the shared font. Must succeed before any text
// call below; returns 1 on success, 0 if the server refused (no
// desktop session). Safe to call again after a font-size change -- it
// re-reads the metrics and the mapping is idempotent.
int ugfx_font_init(void);

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
