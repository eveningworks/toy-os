// See uui_stages.h.
#include <stdio.h>
#include <string.h>
#include "ui/uui_stages.h"
#include "ui/uui_describe.h"
#include "ui/uui_primitives.h"
#include "ui/uui_widget.h"
#include "ui/utheme.h"

static int row_h(void) { return ugfx_char_h() + utheme_gap() / 2; }

void uui_stages_init(struct uui_stages *s) {
    memset(s, 0, sizeof *s);
    uui_progress_init(&s->bar);
    s->bg = UTHEME_WHITE;
}

void uui_stages_set_names(struct uui_stages *s, const char *const *names, int n) {
    if (n > UUI_STAGES_MAX) n = UUI_STAGES_MAX;
    s->count = n < 0 ? 0 : n;
    for (int i = 0; i < UUI_STAGES_MAX; i++) {
        snprintf(s->names[i], sizeof s->names[i], "%s", i < s->count ? names[i] : "");
        s->detail[i][0] = 0;
    }
    s->line[0] = 0;
    s->current = 0;
    uui_progress_set(&s->bar, 0);
}

void uui_stages_set(struct uui_stages *s, int current, int per_mille) {
    s->current = current < 0 ? 0 : current > s->count ? s->count : current;
    if (per_mille < 0) uui_progress_set_busy(&s->bar);
    else uui_progress_set(&s->bar, per_mille);
}

int uui_stages_tick(struct uui_stages *s) { return uui_progress_tick(&s->bar); }

// The headline, the bar, the count line, a rule, then a row per stage.
void uui_stages_natural_size(const struct uui_stages *s, int *out_w, int *out_h) {
    int bw, bh, gap = utheme_gap();
    uui_progress_natural_size(&s->bar, &bw, &bh);
    *out_w = ugfx_char_w() * 46;
    *out_h = row_h() + gap / 2 + bh + gap / 2 + row_h() + (s->count ? gap + s->count * row_h() : 0);
}

// Done: a tick in the theme's "create" green. Running: an accent dot.
// To come: an empty ring.
static void mark(struct ugfx_surface *surf, int x, int y, int d, int state) {
    int r = d / 2 - 1, cx = x + d / 2, cy = y + d / 2;
    if (state < 0) {
        uint32_t ok = utheme_action(UTHEME_ACT_CREATE);
        for (int t = 0; t < 2; t++) {   // two pixels thick
            ugfx_draw_line(surf, x + 1, cy + t, x + d / 3, cy + d / 3 + t, ok, GEOM_AA);
            ugfx_draw_line(surf, x + d / 3, cy + d / 3 + t, x + d - 1, y + 2 + t, ok, GEOM_AA);
        }
    } else if (state == 0) {
        ugfx_fill_circle(surf, cx, cy, r - 1, UTHEME_ACCENT);
    } else {
        ugfx_draw_circle(surf, cx, cy, r - 1, uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED), GEOM_AA);
    }
}

void uui_stages_draw(struct ugfx_surface *surf, const struct uui_stages *s) {
    if (s->w <= 0 || s->h <= 0) return;
    int gap = utheme_gap(), rh = row_h(), y = s->y, cw = ugfx_char_w();
    uint32_t dim = uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED);
    char head[96];
    if (s->count && s->current < s->count)
        snprintf(head, sizeof head, "Stage %d of %d: %s", s->current + 1, s->count, s->names[s->current]);
    else
        snprintf(head, sizeof head, "%s", s->count ? "Done" : "");
    ugfx_draw_string_clipped(surf, s->x, y, s->w, head, UTHEME_TEXT, s->bg);
    y += rh + gap / 2;

    int bw, bh;
    uui_progress_natural_size(&s->bar, &bw, &bh);
    struct uui_progress bar = s->bar;     // placed here; the caller owns the value
    bar.x = s->x; bar.y = y; bar.w = s->w; bar.h = bh;
    uui_progress_draw(surf, &bar);
    y += bh + gap / 2;

    char pct[8] = "";
    if (s->bar.value >= 0) snprintf(pct, sizeof pct, "%d%%", s->bar.value / 10);
    int pw = ugfx_text_width(pct);
    ugfx_draw_string_clipped(surf, s->x, y, s->w - pw - cw, s->line, dim, s->bg);
    ugfx_draw_string_clipped(surf, s->x + s->w - pw, y, pw, pct, dim, s->bg);
    y += rh;
    if (!s->count) return;

    y += gap / 2;
    ugfx_fill_rect(surf, s->x, y, s->w, 1, UTHEME_SEPARATOR);
    y += gap / 2;
    int d = ugfx_char_h() * 3 / 4, tx = s->x + d + cw;
    for (int i = 0; i < s->count; i++, y += rh) {
        int state = i < s->current ? -1 : i == s->current ? 0 : 1;
        mark(surf, s->x, y + (ugfx_char_h() - d) / 2, d, state);
        int dw = ugfx_text_width(s->detail[i]);
        if (dw > s->w / 2) dw = s->w / 2;
        ugfx_draw_string_clipped(surf, tx, y, s->x + s->w - dw - cw - tx, s->names[i],
                                 state > 0 ? dim : UTHEME_TEXT, s->bg);
        ugfx_draw_string_clipped(surf, s->x + s->w - dw, y, dw, s->detail[i], dim, s->bg);
    }
}

static void natural_op(const void *w, int *ow, int *oh) {
    uui_stages_natural_size((const struct uui_stages *)w, ow, oh);
}
static void geometry_op(void *w, int x, int y, int width, int height) {
    struct uui_stages *s = w;
    s->x = x; s->y = y; s->w = width; s->h = height;
}
static void bounds_op(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_stages *s = w;
    *x = s->x; *y = s->y; *ow = s->w; *oh = s->h;
}
static void draw_op(struct ugfx_surface *surf, const void *w) {
    uui_stages_draw(surf, (const struct uui_stages *)w);
}
// Where it has got, so a test reads the stage and the bar from the widget.
static void describe_op(const void *w, const struct uui_describe *d) {
    const struct uui_stages *s = w;
    uui_describe_int(d, "stage", s->current);
    uui_describe_int(d, "stages", s->count);
    uui_describe_int(d, "value", s->bar.value);
    uui_describe_str(d, "line", s->line);
}

// No `hit`: it reports a job, it is not a control. widget-ops-ok: display-only, like uui_progress.
const struct uui_widget_ops uui_stages_ops = {
    .bounds = bounds_op,
    .draw = draw_op,
    .natural_size = natural_op,
    .set_geometry = geometry_op,
    .describe = describe_op,
};
