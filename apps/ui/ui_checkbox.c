// See ui_checkbox.h for the design writeup.
#include "ui_checkbox.h"
#include "ui_primitives.h" // widget_hit(), ui_state_bg()
#include "kapi.h"

#define CHECKBOX_LABEL_GAP 6

void ui_checkbox_natural_size(const struct ui_checkbox *cb, int *out_w, int *out_h) {
    if (out_w) {
        *out_w = cb->label ? cb->size + CHECKBOX_LABEL_GAP + gfx_text_width(cb->label)
                            : cb->size;
    }
    // The box or the text, whichever is taller -- a checkbox drawn at a
    // small `size` next to a large font must not clip its own label.
    if (out_h) *out_h = cb->size > gfx_char_h() ? cb->size : gfx_char_h();
}

void ui_checkbox_set_geometry(struct ui_checkbox *cb, int x, int y) {
    cb->x = x;
    cb->y = y;
    ui_checkbox_natural_size(cb, &cb->w, &cb->h);
}

void ui_checkbox_init(struct ui_checkbox *cb, int x, int y, int size,
                       const char *label, uint32_t bg, uint32_t fg) {
    cb->size = size;
    cb->label = label;
    cb->bg = bg;
    cb->fg = fg;
    cb->checked = 0;
    cb->hovered = 0;
    cb->disabled = 0;
    ui_checkbox_set_geometry(cb, x, y);
}

void ui_checkbox_draw(const struct ui_checkbox *cb, int origin_x, int origin_y) {
    int x = origin_x + cb->x;
    int y = origin_y + cb->y;
    uint32_t bg = cb->bg;
    uint32_t fg = cb->disabled ? ui_state_bg(cb->fg, UI_STATE_DISABLED) : cb->fg;

    if (cb->hovered && !cb->disabled) {
        // The WHOLE clickable area, not just the box -- the hit test is
        // box+label, and a highlight smaller than the target it
        // describes is a lie about where to click. w/h ARE the natural
        // size, so this cannot drift from what hit() accepts.
        bg = ui_state_bg(bg, UI_STATE_HOVER);
        gfx_fill_rect(x, y, cb->w, cb->h, bg);
    }

    gfx_draw_rect(x, y, cb->size, cb->size, fg);
    if (cb->checked) {
        int inset = cb->size / 4 > 0 ? cb->size / 4 : 1;
        gfx_fill_rect(x + inset, y + inset, cb->size - 2 * inset, cb->size - 2 * inset, fg);
    }
    if (cb->label) {
        int label_y = y + (cb->size - gfx_char_h()) / 2;
        // Clipped to what the control actually reserves: an over-long
        // label would otherwise draw straight past its own hit area.
        gfx_draw_string_clipped(x + cb->size + CHECKBOX_LABEL_GAP, label_y,
                                 cb->w - cb->size - CHECKBOX_LABEL_GAP,
                                 cb->label, fg, bg);
    }
}

int ui_checkbox_hit(const struct ui_checkbox *cb, int cx, int cy) {
    // Against the stored w/h -- one geometry, shared by the draw, the
    // hover wash and the hit test. Three separate derivations of that
    // same rectangle is what the object form removes.
    return widget_hit(cb->x, cb->y, cb->w, cb->h, cx, cy);
}

int ui_checkbox_hover(struct ui_checkbox *cb, int cx, int cy) {
    int hit = !cb->disabled && ui_checkbox_hit(cb, cx, cy);
    if (cb->hovered == hit) return 0;
    cb->hovered = hit;
    return 1;
}

int ui_checkbox_toggle(struct ui_checkbox *cb) {
    if (cb->disabled) return cb->checked;
    cb->checked = !cb->checked;
    return cb->checked;
}
