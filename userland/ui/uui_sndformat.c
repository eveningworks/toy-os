// A sound card's output format as a panel -- see uui_sndformat.h.
#include <stdio.h>
#include <string.h>
#include "ui/uui_sndformat.h"
#include "ui/ugfx.h"
#include "ui/utheme.h"

enum { ID_RATE = 0, ID_BITS = 1, ID_ALLOW = 2 };   // + id_base

void uui_sndformat_init(struct uui_sndformat *s, int id_base) {
    memset(s, 0, sizeof *s);
    s->id_base = id_base;
    uui_label_init(&s->rate_l, "Sample rate");
    uui_label_init(&s->allow_l, "Allowed rates");
    uui_label_init(&s->bits_l, "Bit depth");
    uui_label_init(&s->now_l, s->now_text);
    s->now_l.wrap = 1;      // two rows at most: the rates list is long
    s->now_l.rows = 2;
    uui_dropdown_init(&s->rate_dd, 0, 0, 0, 0, 0, 0);
    uui_dropdown_init(&s->bits_dd, 0, 0, 0, 0, 0, 0);
    for (int i = 0; i < SND_RATE_COUNT; i++)
        uui_checkbox_init(&s->allow[i], 0, 0, 0, s->allow_text[i], UUI_COLOR_UNSET, UUI_COLOR_UNSET);
}

// The captions share one width, so the three controls line up.
static int caption_w(void) {
    int w = ugfx_text_width("Allowed rates");
    int b = ugfx_text_width("Sample rate");
    if (b > w) w = b;
    b = ugfx_text_width("Bit depth");
    if (b > w) w = b;
    return w + 2 * ugfx_char_w();
}

static void build(struct uui_sndformat *s) {
    int cw = caption_w();
    s->rate_row_it[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &s->rate_l, .main_size = cw };
    s->rate_row_it[1] = (struct uui_item){ .ops = &uui_dropdown_ops, .widget = &s->rate_dd,
                                           .id = s->id_base + ID_RATE, .name = "sndfmt_rate" };
    s->rate_row = (struct uui_layout){ .dir = UUI_ROW, .items = s->rate_row_it, .count = 2 };

    for (int i = 0; i < s->nallow; i++)
        s->allow_grid_it[i] = (struct uui_item){ .ops = &uui_checkbox_ops, .widget = &s->allow[i],
                                                 .id = s->id_base + ID_ALLOW + i,
                                                 .name = s->allow_name[i] };
    // THREE ACROSS: a Device Manager pane is ~460 px, and a card with all
    // eleven rates (the G6) still makes only four rows.
    s->allow_grid = (struct uui_layout){ .dir = UUI_GRID, .cols = 3, .items = s->allow_grid_it,
                                         .count = s->nallow };
    s->allow_row_it[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &s->allow_l, .main_size = cw };
    s->allow_row_it[1] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &s->allow_grid };
    s->allow_row = (struct uui_layout){ .dir = UUI_ROW, .items = s->allow_row_it, .count = 2 };

    s->bits_row_it[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &s->bits_l, .main_size = cw };
    s->bits_row_it[1] = (struct uui_item){ .ops = &uui_dropdown_ops, .widget = &s->bits_dd,
                                           .id = s->id_base + ID_BITS, .name = "sndfmt_bits" };
    s->bits_row = (struct uui_layout){ .dir = UUI_ROW, .items = s->bits_row_it, .count = 2 };

    s->col_it[0] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &s->rate_row };
    s->col_it[1] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &s->allow_row,
                                      .hidden = !s->f.match || !s->nallow };
    s->col_it[2] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &s->bits_row };
    s->col_it[3] = (struct uui_item){ .ops = &uui_label_ops, .widget = &s->now_l,
                                      .flags = UUI_FILL_W, .name = "sndfmt_now" };
    s->col = (struct uui_layout){ .dir = UUI_COLUMN, .items = s->col_it, .count = 4 };
}

static void list_rates(char *out, size_t cap, uint32_t mask) {
    size_t n = 0;
    out[0] = 0;
    char b[16];
    for (int i = 0; i < SND_RATE_COUNT && n < cap; i++)
        if (mask & (1u << i)) {
            const char *t = usndfmt_rate_label(snd_rate_hz(i), b, sizeof b);
            // "44.1 kHz" -> "44.1" in a list; the unit once, at the end.
            n += (size_t)snprintf(out + n, cap - n, "%s%.*s", n ? ", " : "",
                                  (int)(strlen(t) - 4), t);
        }
    if (n < cap) snprintf(out + n, cap - n, " kHz");
}

void uui_sndformat_load(struct uui_sndformat *s, const struct query_sound *q) {
    strncpy(s->card, q->name, sizeof s->card - 1);
    s->rates = q->rates ? q->rates : SND_RATE_48000;
    s->depths = q->depths;
    usndfmt_load(s->card, &s->f);

    int n = 0, sel = 0;
    s->rate_hz[n] = 0;
    strcpy(s->rate_text[n], "Match what plays");
    n++;
    s->nallow = 0;
    for (int i = 0; i < SND_RATE_COUNT; i++) {
        if (!(s->rates & (1u << i))) continue;
        uint32_t hz = snd_rate_hz(i);
        s->rate_hz[n] = hz;
        usndfmt_rate_label(hz, s->rate_text[n], sizeof s->rate_text[n]);
        if (!s->f.match && s->f.fixed == hz) sel = n;
        n++;
        s->allow_hz[s->nallow] = hz;
        usndfmt_rate_label(hz, s->allow_text[s->nallow], sizeof s->allow_text[0]);
        snprintf(s->allow_name[s->nallow], sizeof s->allow_name[0], "sndfmt_allow_%u", (unsigned)hz);
        s->allow[s->nallow].checked = (s->f.allowed & (1u << i)) != 0;
        s->nallow++;
    }
    for (int i = 0; i < n; i++) s->rate_ptr[i] = s->rate_text[i];
    uui_dropdown_set_items(&s->rate_dd, s->rate_ptr, n);
    uui_dropdown_set_selected(&s->rate_dd, sel);

    static const uint32_t widths[] = { 16, 20, 24, 32 };
    int deepest = 0;
    for (unsigned i = 0; i < 4; i++) if (s->depths & snd_depth_mask(widths[i])) deepest = (int)widths[i];
    n = 0;
    sel = 0;
    s->bits_of[n] = 0;
    if (deepest) snprintf(s->bits_text[n], sizeof s->bits_text[n], "Automatic (%d-bit)", deepest);
    else strcpy(s->bits_text[n], "Automatic");
    n++;
    for (unsigned i = 0; i < 4; i++) {
        if (!(s->depths & snd_depth_mask(widths[i]))) continue;
        s->bits_of[n] = widths[i];
        snprintf(s->bits_text[n], sizeof s->bits_text[n], "%u-bit", (unsigned)widths[i]);
        if (s->f.bits == widths[i]) sel = n;
        n++;
    }
    for (int i = 0; i < n; i++) s->bits_ptr[i] = s->bits_text[i];
    uui_dropdown_set_items(&s->bits_dd, s->bits_ptr, n);
    uui_dropdown_set_selected(&s->bits_dd, sel);

    char takes[96], b[16];
    list_rates(takes, sizeof takes, s->rates);
    char depth[32] = "";
    size_t dn = 0;
    for (unsigned i = 0; i < 4; i++)
        if (s->depths & snd_depth_mask(widths[i]))
            dn += (size_t)snprintf(depth + dn, sizeof depth - dn, "%s%u", dn ? " or " : "",
                                   (unsigned)widths[i]);
    if (q->active)
        snprintf(s->now_text, sizeof s->now_text, "Playing now: %u-bit, %s. The card takes %s%s%s%s.",
                 (unsigned)q->bits, usndfmt_rate_label(q->rate ? q->rate : SND_RATE, b, sizeof b),
                 takes, dn ? " at " : "", depth, dn ? " bits" : "");
    else
        snprintf(s->now_text, sizeof s->now_text, "Not the output now. The card takes %s%s%s%s.",
                 takes, dn ? " at " : "", depth, dn ? " bits" : "");
    uui_label_set_text(&s->now_l, s->now_text);
    build(s);
}

struct uui_item uui_sndformat_item(struct uui_sndformat *s) {
    if (!s->col.items) build(s);
    return (struct uui_item){ .ops = &uui_layout_ops, .widget = &s->col, .flags = UUI_FILL_W };
}

int uui_sndformat_focusables(struct uui_sndformat *s, struct uui_focusable *out, int max) {
    int n = 0;
    if (n < max) out[n++] = (struct uui_focusable){ &s->rate_dd, &uui_dropdown_ops };
    if (s->f.match)
        for (int i = 0; i < s->nallow && n < max; i++)
            out[n++] = (struct uui_focusable){ &s->allow[i], &uui_checkbox_ops };
    if (n < max) out[n++] = (struct uui_focusable){ &s->bits_dd, &uui_dropdown_ops };
    return n;
}

int uui_sndformat_on_widget(struct uui_sndformat *s, int id) {
    int k = id - s->id_base;
    if (k < 0 || k >= UUI_SNDFORMAT_IDS || !s->card[0]) return 0;
    if (k == ID_RATE) {
        int sel = uui_dropdown_selected(&s->rate_dd);
        s->f.match = sel <= 0;
        if (sel > 0) s->f.fixed = s->rate_hz[sel];
        s->col_it[1].hidden = !s->f.match || !s->nallow;
    } else if (k == ID_BITS) {
        int sel = uui_dropdown_selected(&s->bits_dd);
        s->f.bits = sel > 0 ? s->bits_of[sel] : 0;
    } else if (k - ID_ALLOW < s->nallow) {
        uint32_t m = 0;
        for (int i = 0; i < s->nallow; i++)
            if (s->allow[i].checked) m |= snd_rate_mask(s->allow_hz[i]);
        // NEVER NONE: unticking the last allowed rate would leave Match
        // nothing to match, so the tick stays.
        if (!m) {
            s->allow[k - ID_ALLOW].checked = 1;
            m = snd_rate_mask(s->allow_hz[k - ID_ALLOW]);
        }
        s->f.allowed = m;
    } else {
        return 0;
    }
    usndfmt_save(s->card, &s->f);
    return 1;
}
