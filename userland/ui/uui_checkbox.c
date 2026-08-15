// checkbox. See ui/uui_checkbox.h.
#include "ui/uui_checkbox.h"

#define CHECKBOX_LABEL_GAP 6

void uui_checkbox_natural_size(const struct uui_checkbox *cb, int *out_w, int *out_h) {
    if (out_w) {
        *out_w = cb->label ? cb->size + CHECKBOX_LABEL_GAP + ugfx_text_width(cb->label)
                            : cb->size;
    }
    // The box or the text, whichever is taller.
    if (out_h) *out_h = cb->size > ugfx_char_h() ? cb->size : ugfx_char_h();
}

void uui_checkbox_set_geometry(struct uui_checkbox *cb, int x, int y) {
    cb->x = x;
    cb->y = y;
    uui_checkbox_natural_size(cb, &cb->w, &cb->h);
}

void uui_checkbox_init(struct uui_checkbox *cb, int x, int y, int size,
                        const char *label, uint32_t bg, uint32_t fg) {
    cb->size = size;
    cb->label = label;
    cb->bg = bg;
    cb->fg = fg;
    cb->checked = 0;
    cb->hovered = 0;
    cb->disabled = 0;
    uui_checkbox_set_geometry(cb, x, y);
}

void uui_checkbox_draw(struct ugfx_surface *s, const struct uui_checkbox *cb) {
    uint32_t bg = cb->bg;
    uint32_t fg = cb->disabled ? uui_state_bg(cb->fg, UUI_STATE_DISABLED) : cb->fg;

    if (cb->hovered && !cb->disabled) {
        // The WHOLE clickable area -- the hit test is box+label, and a
        // highlight smaller than its target misleads about where to
        // click. w/h ARE the natural size, so the two cannot drift.
        bg = uui_state_bg(bg, UUI_STATE_HOVER);
        ugfx_fill_rect(s, cb->x, cb->y, cb->w, cb->h, bg);
    }

    ugfx_draw_rect(s, cb->x, cb->y, cb->size, cb->size, fg);
    if (cb->checked) {
        int inset = cb->size / 4 > 0 ? cb->size / 4 : 1;
        ugfx_fill_rect(s, cb->x + inset, cb->y + inset,
                        cb->size - 2 * inset, cb->size - 2 * inset, fg);
    }
    if (cb->label) {
        ugfx_draw_string_clipped(s, cb->x + cb->size + CHECKBOX_LABEL_GAP,
                                  cb->y + (cb->size - ugfx_char_h()) / 2,
                                  cb->w - cb->size - CHECKBOX_LABEL_GAP,
                                  cb->label, fg, bg);
    }
}

int uui_checkbox_hit(const struct uui_checkbox *cb, int cx, int cy) {
    // One geometry, shared by the draw, the hover wash and the hit test.
    return uui_hit(cb->x, cb->y, cb->w, cb->h, cx, cy);
}

int uui_checkbox_hover(struct uui_checkbox *cb, int cx, int cy) {
    int hit = !cb->disabled && uui_checkbox_hit(cb, cx, cy);
    if (cb->hovered == hit) return 0;
    cb->hovered = hit;
    return 1;
}

int uui_checkbox_toggle(struct uui_checkbox *cb) {
    if (cb->disabled) return cb->checked;
    cb->checked = !cb->checked;
    return cb->checked;
}
