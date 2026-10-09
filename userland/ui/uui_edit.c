// The shared editing semantics -- see ui/uui_edit.h for why they are
// here rather than in each widget.
#include "ui/uui_edit.h"
#include "keyboard.h" // KEY_* codes, as delivered by WIN_EV_KEY
#include "ui/uui_widget.h" // uui_key_is_shortcut
#include "ui/uui_undo.h"

void uui_edit_init(struct uui_edit *e) {
    e->cursor = 0;
    e->sel_anchor = 0;
    e->sel_active = 0;
    e->undo = 0;
    e->flags = 0;
}

int uui_edit_insert(struct uui_edit *e, const struct uui_edit_ops *ops, void *text,
                    int index, char c) {
    if (!ops->insert(text, index, c)) return 0;
    if (e->undo) uui_undo_note_insert(e->undo, index, &c, 1);
    return 1;
}

int uui_edit_insert_text(struct uui_edit *e, const struct uui_edit_ops *ops, void *text,
                         int index, const char *s, int n) {
    int put = 0;
    if (ops->insert_text) put = ops->insert_text(text, index, s, n);
    else
        while (put < n && ops->insert(text, index + put, s[put])) put++;
    if (put > 0 && e->undo) uui_undo_note_insert(e->undo, index, s, put);
    return put;
}

void uui_edit_erase(struct uui_edit *e, const struct uui_edit_ops *ops, void *text,
                    int start, int end) {
    int len = ops->len(text);
    if (start < 0) start = 0;
    if (end > len) end = len;
    if (start >= end) return;
    if (e->undo && e->undo->buf) {
        // The bytes must be read BEFORE they go; a big range is copied
        // straight into the history rather than through the stack.
        char small[256];
        int n = end - start;
        if (n <= (int)sizeof small) {
            for (int i = 0; i < n; i++) small[i] = ops->at(text, start + i);
            uui_undo_note_erase(e->undo, start, small, n);
        } else {
            uui_undo_note_erase_from(e->undo, start, n, ops, text);
        }
    }
    ops->erase(text, start, end);
}

static int step(struct uui_edit *e, const struct uui_edit_ops *ops, void *text, int redo) {
    if (!e->undo) return 0;
    int cur = e->cursor;
    if (!uui_undo_apply(e->undo, redo, ops, text, &cur)) return 0;
    e->cursor = cur;
    uui_edit_clear_selection(e);
    return 1;
}

int uui_edit_undo(struct uui_edit *e, const struct uui_edit_ops *ops, void *text) {
    return step(e, ops, text, 0);
}

int uui_edit_redo(struct uui_edit *e, const struct uui_edit_ops *ops, void *text) {
    return step(e, ops, text, 1);
}

int uui_edit_has_selection(const struct uui_edit *e) {
    return e->sel_active && e->sel_anchor != e->cursor;
}

void uui_edit_range(const struct uui_edit *e, int *out_start, int *out_end) {
    int a = e->sel_anchor, b = e->cursor;
    if (a > b) { int t = a; a = b; b = t; }
    if (!e->sel_active) { a = b = e->cursor; }
    if (out_start) *out_start = a;
    if (out_end) *out_end = b;
}

void uui_edit_clear_selection(struct uui_edit *e) {
    e->sel_active = 0;
    e->sel_anchor = e->cursor;
}

void uui_edit_select_all(struct uui_edit *e, const struct uui_edit_ops *ops, void *text) {
    e->sel_anchor = 0;
    e->cursor = ops->len(text);
    e->sel_active = 1;
}

int uui_edit_delete_selection(struct uui_edit *e, const struct uui_edit_ops *ops, void *text) {
    if (!uui_edit_has_selection(e)) return 0;
    int start, end;
    uui_edit_range(e, &start, &end);
    uui_edit_erase(e, ops, text, start, end);
    e->cursor = start;
    uui_edit_clear_selection(e);
    return 1;
}

void uui_edit_place(struct uui_edit *e, const struct uui_edit_ops *ops, void *text,
                     int index, int extend) {
    int len = ops->len(text);
    if (index < 0) index = 0;
    if (index > len) index = len;

    if (extend) {
        // Keep the anchor. A shift-click with no selection yet anchors
        // where the caret already was, which is what makes
        // click-then-shift-click select the span between them.
        if (!e->sel_active) { e->sel_anchor = e->cursor; e->sel_active = 1; }
        e->cursor = index;
    } else {
        e->cursor = index;
        uui_edit_clear_selection(e);
    }
    if (e->undo) uui_undo_break(e->undo);   // a moved caret ends a typing run
}

// Moves the caret and maintains the selection the way every desktop
// does: with Shift the anchor stays and the range grows, without it the
// selection collapses -- and collapsing a NON-EMPTY selection with a
// plain arrow puts the caret at the near end rather than moving one
// character further, which is a detail users feel and never articulate.
static void move(struct uui_edit *e, const struct uui_edit_ops *ops, void *text,
                 int to, int extend) {
    if (!extend && uui_edit_has_selection(e)) {
        int start, end;
        uui_edit_range(e, &start, &end);
        uui_edit_clear_selection(e);
        // Collapsing: which end depends on which way this move went.
        e->cursor = (to <= e->cursor) ? start : end;
        if (e->undo) uui_undo_break(e->undo);
        return;
    }
    uui_edit_place(e, ops, text, to, extend);
}

static int clamp(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

int uui_edit_key(struct uui_edit *e, const struct uui_edit_ops *ops, void *text,
                  int key, unsigned mods) {
    // Shift arrives as its own key code, not as a bit -- see below. Ctrl
    // or Alt with a character is a shortcut, declined (uui_widget.h).
    if (uui_key_is_shortcut(key, mods)) return 0;
    int len = ops->len(text);
    e->cursor = clamp(e->cursor, 0, len);
    int ro = (e->flags & UUI_EDIT_READONLY) != 0;

    // ---- the clipboard. Consumed even when there is nothing to do (no
    //      selection, a masked field): a Ctrl+C that fell through to the
    //      app would reach whatever else it binds the key to.
    //      Shift+Delete and Shift+Insert are matched before the plain
    //      keys below; Ctrl+Insert is unambiguous.
    if (key == 0x03 || (key == KEY_INSERT && (mods & KEY_MOD_CTRL))) {   // Ctrl-C
        uui_edit_copy(e, ops, text);
        return 1;
    }
    if (key == 0x18 || (key == KEY_DELETE && (mods & KEY_MOD_SHIFT))) {  // Ctrl-X
        if (ro) uui_edit_copy(e, ops, text);   // a read-only view still copies
        else uui_edit_cut(e, ops, text);
        return 1;
    }
    if (key == 0x16 || (key == KEY_INSERT && (mods & KEY_MOD_SHIFT))) {  // Ctrl-V
        if (!ro) uui_edit_paste(e, ops, text);
        return 1;
    }

    // Nothing below changes a read-only text except by selecting: decline
    // the rest of the editing keys so they stay the app's.
    if (ro && (key == 0x1A || key == 0x19 || key == '\b' || key == KEY_DELETE ||
               IS_PRINTABLE_KEY(key)))
        return 0;

    switch (key) {
    // ---- selection ------------------------------------------------
    case 0x01: // Ctrl-A. Ctrl+letter reaches an app as a CONTROL CODE --
               // see api/keyboard.h's "Ctrl and Alt".
        uui_edit_select_all(e, ops, text);
        return 1;

    // ---- history: declined when there is none, so an app may use them
    case 0x1A: // Ctrl-Z
        if (!e->undo) return 0;
        uui_edit_undo(e, ops, text);
        return 1;
    case 0x19: // Ctrl-Y
        if (!e->undo) return 0;
        uui_edit_redo(e, ops, text);
        return 1;

    // ---- deletion -------------------------------------------------
    case '\b':
        if (uui_edit_delete_selection(e, ops, text)) return 1;
        if (e->cursor > 0) {
            uui_edit_erase(e, ops, text, e->cursor - 1, e->cursor);
            e->cursor--;
        }
        uui_edit_clear_selection(e);
        return 1;

    case KEY_DELETE:
        if (uui_edit_delete_selection(e, ops, text)) return 1;
        if (e->cursor < len) uui_edit_erase(e, ops, text, e->cursor, e->cursor + 1);
        uui_edit_clear_selection(e);
        return 1;

    // ---- motion. The Shift variants are distinct KEY CODES here
    //      (api/keyboard.h), which is why this switch reads as pairs.
    case KEY_ARROW_LEFT:        move(e, ops, text, e->cursor - 1, 0); return 1;
    case KEY_SHIFT_ARROW_LEFT:  move(e, ops, text, e->cursor - 1, 1); return 1;
    case KEY_ARROW_RIGHT:       move(e, ops, text, e->cursor + 1, 0); return 1;
    case KEY_SHIFT_ARROW_RIGHT: move(e, ops, text, e->cursor + 1, 1); return 1;

    case KEY_HOME:
    case KEY_SHIFT_HOME: {
        int extend = (key == KEY_SHIFT_HOME);
        int to = ops->line_start ? ops->line_start(text, e->cursor) : 0;
        move(e, ops, text, to, extend);
        return 1;
    }
    case KEY_END:
    case KEY_SHIFT_END: {
        int extend = (key == KEY_SHIFT_END);
        int to = ops->line_end ? ops->line_end(text, e->cursor) : len;
        move(e, ops, text, to, extend);
        return 1;
    }

    // Up/Down exist only where there ARE lines. A single-line field
    // leaves these ops NULL and declines the key, so the app can use it
    // for something else rather than having it silently swallowed.
    case KEY_ARROW_UP:
    case KEY_SHIFT_ARROW_UP: {
        if (!ops->line_up) return 0;
        move(e, ops, text, ops->line_up(text, e->cursor), key == KEY_SHIFT_ARROW_UP);
        return 1;
    }
    case KEY_ARROW_DOWN:
    case KEY_SHIFT_ARROW_DOWN: {
        if (!ops->line_down) return 0;
        move(e, ops, text, ops->line_down(text, e->cursor), key == KEY_SHIFT_ARROW_DOWN);
        return 1;
    }

    default:
        break;
    }

    // ---- typing ---------------------------------------------------
    //
    // A printable character REPLACES the selection. Deleting first and
    // then inserting is the whole implementation, and doing it in this
    // order is what stops a replacement from being an insert next to
    // text the user thought they had just overwritten.
    if (IS_PRINTABLE_KEY(key)) {   // ASCII or Latin-1, api/keyboard.h
        // Replacing a selection is ONE step: the erase and the insert.
        int replacing = uui_edit_has_selection(e);
        if (replacing && e->undo) uui_undo_begin(e->undo);
        uui_edit_delete_selection(e, ops, text);
        if (uui_edit_insert(e, ops, text, e->cursor, (char)key)) e->cursor++;
        if (replacing && e->undo) uui_undo_end(e->undo);
        uui_edit_clear_selection(e);
        return 1;
    }

    // Notably Enter: committing a field, inserting a newline or ignoring
    // it is the widget's decision, not this core's.
    return 0;
}
