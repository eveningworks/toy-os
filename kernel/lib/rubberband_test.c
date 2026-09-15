// Rubber-band selection: the behaviour, tested without a screen.
//
// This is the payoff of the module owning behaviour rather than the
// desktop owning it -- every rule below (what a shrinking band does, how
// a modifier composes, what a click means) is assertable against a grid
// of made-up rectangles, with no compositor, no cursor and no pixels.
#include "ktest.h"
#include "rubberband.h"

// A 4x4 grid of 20x20 items on a 40px pitch: item i sits at
// (col*40, row*40). Chosen so there is a real GAP between items -- a
// band can then be placed to touch some and miss others, which a
// gapless grid cannot express.
#define GRID_COLS 4
#define GRID_N 16

static int grid_count(void *ctx) { return ctx ? *(int *)ctx : GRID_N; }

static void grid_rect(void *ctx, int i, int *x, int *y, int *w, int *h) {
    (void)ctx;
    *x = (i % GRID_COLS) * 40;
    *y = (i / GRID_COLS) * 40;
    *w = 20;
    *h = 20;
}

static const struct rb_ops GRID = { grid_count, grid_rect };

// Drag from (x0,y0) to (x1,y1) in one motion. Past the threshold in a
// single step, which is all these tests need -- the threshold itself
// gets its own test below.
static void drag(struct rubberband *rb, int x0, int y0, int x1, int y1,
                  enum rb_mode mode) {
    rb_begin(rb, x0, y0, mode);
    rb_motion(rb, x1, y1, &GRID, 0);
}

KTEST("rubberband", "a band selects exactly what it touches") {
    struct rubberband rb;
    rb_clear(&rb);

    // Covers items 0 and 1 (x 0..20 and 40..60 at row 0) and nothing in
    // row 1, which starts at y=40.
    drag(&rb, 0, 0, 45, 25, RB_REPLACE);
    KTEST_ASSERT(rb_end(&rb) == 1); // it WAS a band, not a click

    KTEST_ASSERT(rb_is_selected(&rb, 0));
    KTEST_ASSERT(rb_is_selected(&rb, 1));
    KTEST_ASSERT(!rb_is_selected(&rb, 2));
    KTEST_ASSERT(!rb_is_selected(&rb, 4)); // row 1
    KTEST_ASSERT_EQ(rb_selected_count(&rb), 2);
}

KTEST("rubberband", "pulling the band BACK off an item deselects it") {
    struct rubberband rb;
    rb_clear(&rb);

    // The check an accumulating implementation fails. It looks identical
    // while the band grows; only shrinking tells the two apart.
    rb_begin(&rb, 0, 0, RB_REPLACE);
    rb_motion(&rb, 45, 25, &GRID, 0);
    KTEST_ASSERT_EQ(rb_selected_count(&rb), 2);

    rb_motion(&rb, 15, 15, &GRID, 0); // back to covering item 0 only
    KTEST_ASSERT(rb_is_selected(&rb, 0));
    KTEST_ASSERT(!rb_is_selected(&rb, 1));
    KTEST_ASSERT_EQ(rb_selected_count(&rb), 1);
    rb_end(&rb);
}

KTEST("rubberband", "a band dragged up-and-left is an ordinary band") {
    struct rubberband rb;
    rb_clear(&rb);

    // Same rectangle as the first test, anchored at the opposite corner.
    // A version that forgot to normalise selects nothing here and looks
    // fine in every other test.
    drag(&rb, 45, 25, 0, 0, RB_REPLACE);
    int x, y, w, h;
    KTEST_ASSERT(rb_rect(&rb, &x, &y, &w, &h));
    KTEST_ASSERT_EQ(x, 0);
    KTEST_ASSERT_EQ(y, 0);
    KTEST_ASSERT(w > 0 && h > 0);
    KTEST_ASSERT_EQ(rb_selected_count(&rb), 2);
    rb_end(&rb);
}

KTEST("rubberband", "RB_ADD keeps what was already selected") {
    struct rubberband rb;
    rb_clear(&rb);

    rb_select(&rb, 8, 1); // row 2, nowhere near the band below
    drag(&rb, 0, 0, 45, 25, RB_ADD);
    rb_end(&rb);

    KTEST_ASSERT(rb_is_selected(&rb, 8)); // survived
    KTEST_ASSERT(rb_is_selected(&rb, 0));
    KTEST_ASSERT(rb_is_selected(&rb, 1));
    KTEST_ASSERT_EQ(rb_selected_count(&rb), 3);
}

KTEST("rubberband", "RB_REPLACE drops what was already selected") {
    struct rubberband rb;
    rb_clear(&rb);

    // The control for the test above: same setup, different mode, and
    // the outcome must differ or neither test means anything.
    rb_select(&rb, 8, 1);
    drag(&rb, 0, 0, 45, 25, RB_REPLACE);
    rb_end(&rb);

    KTEST_ASSERT(!rb_is_selected(&rb, 8));
    KTEST_ASSERT_EQ(rb_selected_count(&rb), 2);
}

KTEST("rubberband", "RB_TOGGLE flips only what the band covers") {
    struct rubberband rb;
    rb_clear(&rb);

    rb_select(&rb, 0, 1); // already selected -- the band must UNselect it
    rb_select(&rb, 8, 1); // outside the band -- must be left alone
    drag(&rb, 0, 0, 45, 25, RB_TOGGLE);
    rb_end(&rb);

    KTEST_ASSERT(!rb_is_selected(&rb, 0)); // flipped off
    KTEST_ASSERT(rb_is_selected(&rb, 1));  // flipped on
    KTEST_ASSERT(rb_is_selected(&rb, 8));  // untouched
}

KTEST("rubberband", "a click is not a band, and clears the selection") {
    struct rubberband rb;
    rb_clear(&rb);
    rb_select(&rb, 3, 1);

    // Below the threshold in both axes: this is a click on empty space.
    rb_begin(&rb, 100, 100, RB_REPLACE);
    rb_motion(&rb, 101, 101, &GRID, 0);
    KTEST_ASSERT(!rb_rect(&rb, 0, 0, 0, 0)); // nothing to draw
    KTEST_ASSERT_EQ(rb_end(&rb), 0);          // reported as a click

    KTEST_ASSERT_EQ(rb_selected_count(&rb), 0);
}

KTEST("rubberband", "a modified click leaves the selection alone") {
    struct rubberband rb;
    rb_clear(&rb);
    rb_select(&rb, 3, 1);

    // Ctrl-clicking empty space must not wipe a selection the user is
    // in the middle of building -- the whole point of the modifier.
    rb_begin(&rb, 100, 100, RB_ADD);
    rb_motion(&rb, 101, 101, &GRID, 0);
    KTEST_ASSERT_EQ(rb_end(&rb), 0);
    KTEST_ASSERT(rb_is_selected(&rb, 3));
}

KTEST("rubberband", "the threshold is what separates a click from a band") {
    struct rubberband rb;
    rb_clear(&rb);

    rb_begin(&rb, 0, 0, RB_REPLACE);
    rb_motion(&rb, 1, 1, &GRID, 0);
    KTEST_ASSERT(!rb.active);                  // 2 < RB_DRAG_THRESHOLD

    rb_motion(&rb, 45, 25, &GRID, 0);
    KTEST_ASSERT(rb.active);
    KTEST_ASSERT_EQ(rb_end(&rb), 1);
}

KTEST("rubberband", "an over-large item count cannot walk off the bitset") {
    struct rubberband rb;
    rb_clear(&rb);

    // A caller reporting more items than the bitset holds is clamped,
    // not trusted. The failure this prevents is a silent write past
    // rb.sel into rb.base -- which would look like a selection that
    // mysteriously survives the next drag.
    // The band has to reach every one of the clamped items or the count
    // below measures the band's size instead of the clamp: at 4 columns
    // and a 40px pitch, item 127 sits at y = 1240.
    int huge = RB_MAX_ITEMS * 4;
    rb_begin(&rb, -1000, -1000, RB_REPLACE);
    rb_motion(&rb, 5000, 5000, &GRID, &huge);
    rb_end(&rb);

    KTEST_ASSERT_EQ(rb_selected_count(&rb), RB_MAX_ITEMS);
}

KTEST("rubberband", "an empty band and empty items touch nothing") {
    // rb_overlaps() is the one piece of geometry a caller may use
    // itself, so its edge convention is pinned here rather than left to
    // each caller to rediscover.
    KTEST_ASSERT(!rb_overlaps(0, 0, 0, 0, 0, 0, 10, 10));
    KTEST_ASSERT(!rb_overlaps(0, 0, 10, 10, 0, 0, 0, 0));

    // Touching edges do NOT overlap: item at x=10 is not covered by a
    // band ending at x=10. Half-open, like every other rect in this
    // codebase.
    KTEST_ASSERT(!rb_overlaps(0, 0, 10, 10, 10, 0, 10, 10));
    KTEST_ASSERT(rb_overlaps(0, 0, 11, 10, 10, 0, 10, 10));
}

// The Screenshot app drags a band over a picture rather than over a
// list, so there is nothing to select and no ops table to describe it.
// Refusing that case made the band never go active, and a rectangle
// that never exists reads to the app as a drag that did nothing.
KTEST("rubberband", "a band with no selection model still has a rect") {
    struct rubberband rb;
    rb_clear(&rb);

    rb_begin(&rb, 100, 50, RB_REPLACE);
    rb_motion(&rb, 40, 200, 0, 0);

    int x, y, w, h;
    KTEST_ASSERT(rb_rect(&rb, &x, &y, &w, &h));
    // Normalised: the drag went up-and-left in x and down in y.
    KTEST_ASSERT_EQ(x, 40);
    KTEST_ASSERT_EQ(y, 50);
    KTEST_ASSERT_EQ(w, 60);
    KTEST_ASSERT_EQ(h, 150);
    KTEST_ASSERT(rb_end(&rb) == 1);       // a band, not a click
    KTEST_ASSERT_EQ(rb_selected_count(&rb), 0);
}
