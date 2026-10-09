// The edit core's clipboard verbs -- see ui/uui_edit.h. Split from
// uui_edit.c only so a host build of the core needs no clipboard.
#include "ui/uui_edit.h"
#include "ui/uui_undo.h"
#include "lib/uclip.h"
#include <stdlib.h>

int uui_edit_clip_has_text(void) {
    // STATIC: the snapshot embeds the whole payload (lib/uclip.h).
    static struct uclip c;
    if (uclip_peek_op() == UCLIP_NONE) return 0;
    uclip_load(&c);
    int n = 0;
    return uclip_text(&c, &n) != 0 && n > 0;
}

int uui_edit_copy(struct uui_edit *e, const struct uui_edit_ops *ops, void *text) {
    if ((e->flags & UUI_EDIT_MASKED) || !uui_edit_has_selection(e)) return 0;
    int start, end;
    uui_edit_range(e, &start, &end);
    int n = end - start;
    if (n > UCLIP_TEXT_MAX) return 0;   // uclip_set_text() would refuse it anyway
    char *tmp = (char *)malloc((size_t)n + 1);
    if (!tmp) return 0;
    for (int i = 0; i < n; i++) tmp[i] = ops->at(text, start + i);
    tmp[n] = 0;
    int ok = uclip_set_text(tmp, n);
    free(tmp);
    return ok ? 1 : 0;
}

int uui_edit_cut(struct uui_edit *e, const struct uui_edit_ops *ops, void *text) {
    if (e->flags & UUI_EDIT_READONLY) return 0;
    if (!uui_edit_copy(e, ops, text)) return 0;
    if (e->undo) uui_undo_break(e->undo);
    uui_edit_delete_selection(e, ops, text);
    if (e->undo) uui_undo_break(e->undo);
    return 1;
}

int uui_edit_paste(struct uui_edit *e, const struct uui_edit_ops *ops, void *text) {
    static struct uclip c;
    if (e->flags & UUI_EDIT_READONLY) return 0;
    uclip_load(&c);
    int n = 0;
    const char *s = uclip_text(&c, &n);
    if (!s || n <= 0) return 0;
    int one_line = ops->line_start == 0;

    if (e->undo) { uui_undo_break(e->undo); uui_undo_begin(e->undo); }
    uui_edit_delete_selection(e, ops, text);
    int put = 0;
    // Run by run between the characters that never go in, so a pasted
    // CRLF document costs one insert per line rather than per character.
    for (int i = 0; i < n; ) {
        if (s[i] == '\r') { i++; continue; }
        if (one_line && s[i] == '\n') break;
        int j = i;
        while (j < n && s[j] != '\r' && !(one_line && s[j] == '\n')) j++;
        int got = uui_edit_insert_text(e, ops, text, e->cursor, s + i, j - i);
        e->cursor += got;
        put += got;
        if (got < j - i) break;   // full: what fitted went in
        i = j;
    }
    uui_edit_clear_selection(e);
    if (e->undo) { uui_undo_end(e->undo); uui_undo_break(e->undo); }
    return put;
}
