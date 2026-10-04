#ifndef ULIB_UREGION_H
#define ULIB_UREGION_H

// A REGION: a bounded list of disjoint rectangles, cut by subtraction --
// what the compositor draws each window inside once the opaque windows
// above it are taken away (pixman's region32, which wlroots' scene graph
// and Weston clip with; Mutter's cairo_region_t). Pure arithmetic: no
// allocation, no clock, no surface, so tools/uregion_hostcheck.py runs it
// under the host's gcc against a bitmap.
//
// BOUNDED, AND AN OVERFLOW ERRS LARGE. A subtraction whose result would
// need more than UREGION_MAX rectangles is NOT applied, and `overflow`
// says so: the region is then a SUPERSET of the exact answer, never a
// subset. That is the safe side for a clip -- drawing a pixel something
// opaque later covers costs time; missing one leaves a stale pixel.
// Do not use a region for anything where "too large" is the wrong side.
//
// Rectangles are half-open (x, y, w, h); empty ones are never stored.

#include <stdint.h>

#define UREGION_MAX 32

struct urect { int x, y, w, h; };

struct uregion {
    int n;
    int overflow;   // a subtraction was skipped for room: a superset
    struct urect r[UREGION_MAX];
};

void uregion_init(struct uregion *g);   // empty
void uregion_init_rect(struct uregion *g, int x, int y, int w, int h);
int  uregion_is_empty(const struct uregion *g);

// Keeps only what lies inside (x, y, w, h). Never overflows.
void uregion_intersect_rect(struct uregion *g, int x, int y, int w, int h);

// Removes (x, y, w, h); see BOUNDED above for what happens without room.
void uregion_subtract_rect(struct uregion *g, int x, int y, int w, int h);

// The bounding box; 0 (and a zero rect) when the region is empty.
int  uregion_bbox(const struct uregion *g, struct urect *out);

long long uregion_area(const struct uregion *g);
int  uregion_contains(const struct uregion *g, int x, int y);

// WHERE TWO EQUAL-SIZED PIXEL BUFFERS DIFFER, as at most `max` row BANDS
// (max <= UREGION_MAX): each band spans the rows from its first
// differing row to its last and the columns of the widest difference
// among them. Differences fewer than `gap` rows apart share a band, and
// a band past `max` is folded into the one before it -- a superset,
// never a miss. Empty when the buffers are equal. TigerVNC's comparing
// update tracker does this to shrink what a client declared; a
// compositor's client does it to declare damage at all.
void uregion_diff(struct uregion *g, const uint32_t *a, const uint32_t *b,
                  int w, int h, int stride, int max, int gap);

#endif
