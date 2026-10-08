#ifndef ULIB_UPILE_H
#define ULIB_UPILE_H

// DEBRIS THAT FALLS, BOUNCES AND PILES UP at the bottom of a picture --
// the Desktop cube's chips (gui/savers/cube.c), and anything else that
// sheds bits onto a floor.
//
// Two kinds of piece. A GRAIN lands and joins the pile at once; a SHARD
// is a small spinning triangle that bounces, slows, and then either
// settles where it lies or CRUMBLES into grains.
//
// **THE PILE IS A HEIGHT PER COLUMN, NOT A SIMULATION OF EVERY GRAIN.** A
// grain rolls toward a lower neighbour until the step is no more than one
// grain -- the sandpile rule, so the pile takes a natural slope -- and is
// then PAINTED INTO THE CALLER'S BACKDROP. A settled pile costs nothing per
// frame: only pieces still in the air are drawn. That is how falling-sand
// games keep a screenful of sand cheap, cut down to one row of cells.
//
// The pile SINKS once it is taller than `sink_at`: a pixel at a time,
// painting back what was there before it, so it never buries the screen.

#include <stdint.h>
#include "ui/ugfx.h"

#define UPILE_PARTS 512

enum { UPILE_GRAIN, UPILE_SHARD };

struct upile_part {
    int32_t x, y;        // 1/256 px
    int32_t vx, vy;      // 1/256 px per second
    uint16_t ang;        // a shard's turn, 1/65536 of a revolution
    int16_t va;          // ...per second, same units / 4
    uint8_t kind, size;  // size in px
    uint32_t color;
};

struct upile {
    int w, h, cell, cols;
    uint16_t *top;           // the pile's height per column, px
    uint32_t *bake;          // the caller's backdrop, w x h, the pile painted in
    uint32_t *clean;         // ...as it was before any of it, for sinking
    int crumble;             // shards turn to grains when they come to rest
    int sink_at;             // px; 0 never sinks
    int sink_ms;
    uint32_t rng;
    int n;
    struct upile_part p[UPILE_PARTS];
    // What changed in `bake` since upile_take_dirty(): the caller's
    // buffers must repaint it from the backdrop.
    int dx0, dy0, dx1, dy1;
};

// `bake` is borrowed and must be complete: its current pixels are copied
// as the clean backdrop. `cell` is a grain's size. Returns 0 out of memory.
int upile_init(struct upile *p, uint32_t *bake, int w, int h, int cell);
void upile_free(struct upile *p);

// A piece at (x, y) px moving (vx, vy) px/s. Dropped when the pool is full.
void upile_add(struct upile *p, int kind, int x, int y, int vx, int vy, int size, uint32_t color);

// Gravity, bounces, landing, settling, sinking: `dt_ms` of it.
void upile_step(struct upile *p, int dt_ms);

// The flying pieces onto `s`; their box goes in *x0..*y1 (empty: x1 <= x0).
void upile_draw(struct upile *p, struct ugfx_surface *s, int *x0, int *y0, int *x1, int *y1);

// The top of the pile over columns x0..x1 px: the floor a body above it lands on.
int upile_floor(const struct upile *p, int x0, int x1);

// The box of `bake` changed since the last call; 0 when nothing did.
int upile_take_dirty(struct upile *p, int *x0, int *y0, int *x1, int *y1);

#endif // ULIB_UPILE_H
