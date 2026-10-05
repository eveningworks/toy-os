// See ui/uui_confetti.h.
#include "ui/uui_confetti.h"
#include "ui/uui_anim.h"
#include "ui/utheme.h"
#include "fixed.h"

// The rise peaks this far through a piece's life, so most of it is
// spent falling, which is what reads as confetti rather than sparks.
#define PEAK_PERMILLE 300
// The last stretch of a piece's life fades it out.
#define FADE_PERMILLE 300

static uint32_t next(uint32_t *x) {
    uint32_t v = *x;
    v ^= v << 13;
    v ^= v >> 17;
    v ^= v << 5;
    return *x = v;
}

void uui_confetti_start(struct uui_confetti *c, int ox, int oy, int reach_px,
                        int count, unsigned dur_ms, uint32_t seed,
                        unsigned long long t0_ns) {
    c->dur_ms = 0;
    c->count = 0;
    if (dur_ms == 0 || reach_px <= 0 || count <= 0) return;
    if (count > UUI_CONFETTI_MAX) count = UUI_CONFETTI_MAX;
    uint32_t r = seed ? seed : 0x9E3779B9u;

    // Projectile motion solved backwards: a piece launched at vy0
    // peaks after tp, at height vy0 * tp / 2. Gravity is chosen so the
    // FASTEST piece peaks at `reach_px` exactly at tp.
    int tp = (int)(dur_ms * PEAK_PERMILLE / 1000);
    if (tp < 1) tp = 1;
    int vy_max = (int)(2LL * reach_px * 1000 / tp);
    c->g = (int)((long long)vy_max * 1000 / tp);
    int vx_max = (int)((long long)reach_px * 2400 / dur_ms);

    for (int i = 0; i < count; i++) {
        struct uui_confetti_piece *p = &c->p[i];
        p->vy = (int16_t)-(vy_max * (70 + (int)(next(&r) % 31)) / 100);
        p->vx = (int16_t)((int)(next(&r) % (uint32_t)(2 * vx_max + 1)) - vx_max);
        p->w = (uint8_t)(4 + next(&r) % 4);
        p->h = (uint8_t)(7 + next(&r) % 6);
        p->spin = (uint8_t)(10 + next(&r) % 25);
        p->colour = (uint8_t)(i % 8);
        p->delay_ms = (uint16_t)(next(&r) % 120);
    }
    c->count = count;
    c->ox = ox;
    c->oy = oy;
    c->dur_ms = dur_ms;
    c->t0_ns = t0_ns;
}

void uui_confetti_stop(struct uui_confetti *c) { c->dur_ms = 0; c->count = 0; }

// The theme's action roles plus the accent: eight colours that already
// sit together, and follow a theme change.
static uint32_t colour_of(int i) {
    if (i == 0) return UTHEME_ACCENT;
    return utheme_action(UTHEME_ACT_NAV + (i - 1) % (UTHEME_ACT_COUNT - 1));
}

int uui_confetti_draw(struct ugfx_surface *s, struct uui_confetti *c,
                      unsigned long long now_ns) {
    if (c->dur_ms == 0) return 0;
    if (now_ns < c->t0_ns) { uui_anim_request(); return 1; }
    long long since = (long long)((now_ns - c->t0_ns) / 1000000ull);
    int live = 0;
    for (int i = 0; i < c->count; i++) {
        const struct uui_confetti_piece *p = &c->p[i];
        long long t = since - p->delay_ms;
        if (t < 0) { live = 1; continue; }
        if (t >= c->dur_ms) continue;
        live = 1;
        int x = c->ox + (int)(p->vx * t / 1000);
        int y = c->oy + (int)(p->vy * t / 1000 + (long long)c->g * t * t / 2000000);
        // THE TUMBLE is the piece's width foreshortened by cos(angle) --
        // a flat card turning end over end, which is all confetti is.
        fx_t turns = (fx_t)((long long)p->spin * FX_ONE * t / 10000);
        fx_t cs = fx_cos(turns);
        if (cs < 0) cs = -cs;
        int w = (int)(((long long)p->w * cs) >> FX_SHIFT);
        if (w < 1) w = 1;
        int h = p->h;
        unsigned fade_at = c->dur_ms * (1000 - FADE_PERMILLE) / 1000;
        uint32_t col = colour_of(p->colour);
        if ((unsigned)t < fade_at) {
            ugfx_fill_rect(s, x - w / 2, y - h / 2, w, h, col);
        } else {
            int a = (int)(255 - 255 * ((unsigned)t - fade_at) / (c->dur_ms - fade_at));
            if (a <= 0) continue;
            for (int row = 0; row < h; row++)
                ugfx_blend_hspan(s, x - w / 2, y - h / 2 + row, w, col, 0, (uint8_t)a);
        }
    }
    if (!live) { c->dur_ms = 0; return 0; }
    uui_anim_request();
    return 1;
}
