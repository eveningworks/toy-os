#ifndef UUI_EDIT_H
#define UUI_EDIT_H

// uui_edit -- what EDITING TEXT means, in one place.
//
// WHY THIS EXISTS
// ---------------
// Editable text existed three times over and behaved differently in
// each: `uui_textbox` (single-line) had a caret and no selection at all,
// `utext` (multi-line) had selection primitives but no keymap, and
// Notepad hand-wrote the keymap on top of them -- roughly sixty lines
// deciding what Ctrl+A means, what Shift+Left does, and what typing with
// a selection active should do. A second app wanting a text field would
// have written that a fourth time, differently.
//
// The semantics are the shared thing, not the storage. So this owns the
// CURSOR, the SELECTION and the KEYMAP, and delegates every actual read
// or write of characters to whoever owns the buffer. A 48-byte line and
// an 8 KB ring do not share storage; they do share what Backspace with
// a selection active is supposed to do.
//
// This is the same split `kernel/lib/klineedit.c` already makes on the
// kernel side, where the physical shell and the GUI Terminal share one
// line editor and each only paints the result.
//
// THE BEHAVIOUR, which is Windows' and KDE's
// ------------------------------------------
// Every point below is what both of those do, and what a user reaching
// for a text field expects without being told:
//
//   * Typing with a selection REPLACES it.
//   * Backspace and Delete remove the selection if there is one, and a
//     single character otherwise.
//   * Ctrl+A selects everything.
//   * Shift+arrow (and Shift+Home/End) EXTENDS the selection from where
//     it was anchored; a plain arrow collapses it.
//   * Clicking places the caret and clears the selection; dragging from
//     a click extends one.
//   * A selection is a RANGE, not a highlight: it survives cursor
//     movement that extends it and dies on movement that does not.

struct uui_undo;

struct uui_edit {
    // Where the caret is, as a logical index into whatever the storage
    // is -- "just before character `cursor`", the convention utext
    // already used.
    int cursor;

    // The selection spans [min(anchor, cursor), max(anchor, cursor)),
    // computed on demand rather than tracked, so cursor movement cannot
    // leave the two disagreeing. `active` is a separate flag rather
    // than "anchor != cursor" so an active-but-empty selection -- the
    // instant a drag starts -- stays distinguishable from none at all.
    int sel_anchor;
    int sel_active;

    // The edit HISTORY (ui/uui_undo.h), or NULL for none. Every change
    // this core makes is recorded there, and Ctrl+Z / Ctrl+Y walk it.
    struct uui_undo *undo;

    // UUI_EDIT_* below. Re-asserted by the widget before each call, the
    // way `undo` is, because uui_edit_init() clears it.
    unsigned flags;
};

// A PASSWORD: nothing leaves it through the clipboard -- Cut and Copy
// are refused, Paste is not (Win32's ES_PASSWORD, GTK's invisible entry).
#define UUI_EDIT_MASKED   0x01u
// Selectable and copyable, never changed: typing, deletion, Cut, Paste
// and the history are all declined.
#define UUI_EDIT_READONLY 0x02u

// How this core reaches the caller's characters. Four slots, all
// required: a widget that cannot answer these is not editable text.
//
// `insert` returns 1 if the character went in (0 = full, which is a
// normal answer for a fixed-size field, not an error). `erase` removes
// the half-open range [start, end).
struct uui_edit_ops {
    int  (*len)(void *text);
    char (*at)(void *text, int index);
    int  (*insert)(void *text, int index, char c);
    void (*erase)(void *text, int start, int end);

    // MULTI-LINE only, and optional: a single-line field leaves these
    // NULL and the core simply never offers Up/Down or line-wise Home
    // and End. That is the honest behaviour -- a one-line field has no
    // line above -- rather than a special case at every call site.
    int (*line_start)(void *text, int index);
    int (*line_end)(void *text, int index);
    int (*line_up)(void *text, int index);
    int (*line_down)(void *text, int index);

    // OPTIONAL: insert `n` characters at once, returning how many went
    // in. Without it a paste or an undo inserts one character at a time,
    // which in a flat buffer moves the whole tail once per character.
    int (*insert_text)(void *text, int index, const char *s, int n);
};

void uui_edit_init(struct uui_edit *e);

// Is there a selection with anything actually in it? An active but
// empty one answers 0, which is what a caller asking "is there
// something to delete or highlight" means.
int uui_edit_has_selection(const struct uui_edit *e);

// The selection as a sorted half-open range. Meaningful only when
// uui_edit_has_selection(); harmless otherwise (both become `cursor`).
void uui_edit_range(const struct uui_edit *e, int *out_start, int *out_end);

void uui_edit_select_all(struct uui_edit *e, const struct uui_edit_ops *ops, void *text);
void uui_edit_clear_selection(struct uui_edit *e);

// Removes the selected range, leaves the caret where it started, and
// drops the selection. No-op with nothing selected -- so a caller can
// call it unconditionally before inserting, which is exactly what
// "typing replaces the selection" is.
int uui_edit_delete_selection(struct uui_edit *e, const struct uui_edit_ops *ops, void *text);

// Puts the caret at `index`. `extend` non-zero keeps the existing anchor
// (a shift-click, or a drag), zero collapses the selection -- which is
// the whole difference between clicking and shift-clicking.
void uui_edit_place(struct uui_edit *e, const struct uui_edit_ops *ops, void *text,
                     int index, int extend);

// THE ONLY WAY TEXT CHANGES, so the history sees every change: insert
// one character (1 if it went in), insert a run, erase a range. A
// widget's own entry points call these rather than its raw ops.
int  uui_edit_insert(struct uui_edit *e, const struct uui_edit_ops *ops, void *text,
                     int index, char c);
int  uui_edit_insert_text(struct uui_edit *e, const struct uui_edit_ops *ops, void *text,
                          int index, const char *s, int n);
void uui_edit_erase(struct uui_edit *e, const struct uui_edit_ops *ops, void *text,
                    int start, int end);

// Undo / redo one step of `e->undo`, leaving the caret where it
// happened. 0 when there is no history or nothing to step.
int uui_edit_undo(struct uui_edit *e, const struct uui_edit_ops *ops, void *text);
int uui_edit_redo(struct uui_edit *e, const struct uui_edit_ops *ops, void *text);

// One keypress, with the modifiers as delivered (KEY_MOD_*). Returns 1
// if it was consumed. Ctrl+Z and Ctrl+Y are undo and redo when there
// is a history, and declined when there is none. Ctrl+X/C/V and the CUA
// trio (Shift+Delete, Ctrl+Insert, Shift+Insert) are the clipboard,
// below. Deliberately does NOT consume Enter: whether a
// field commits, inserts a newline or ignores it is the widget's
// decision, not this core's.
int uui_edit_key(struct uui_edit *e, const struct uui_edit_ops *ops, void *text,
                  int key, unsigned mods);

// --- the system clipboard (lib/uclip.h), in ui/uui_edit_clip.c ---------
//
// A file of its own so the host checks that compile this core
// (tools/utext_hostcheck.py) need no clipboard service.
//
// Copy: 1 = the selection is on the clipboard; 0 = nothing selected, a
// masked field, or the clipboard refused (too big, no clipboardd).
// Cut: copy, then delete -- and only if the copy landed. Paste: the
// characters inserted, replacing the selection as ONE undo step; a
// field with no line ops takes the clipboard's FIRST LINE only (Win32's
// single-line EDIT does the same), and '\r' never goes in.
int uui_edit_copy(struct uui_edit *e, const struct uui_edit_ops *ops, void *text);
int uui_edit_cut(struct uui_edit *e, const struct uui_edit_ops *ops, void *text);
int uui_edit_paste(struct uui_edit *e, const struct uui_edit_ops *ops, void *text);
// Does the clipboard hold text right now? One shared-memory read.
int uui_edit_clip_has_text(void);

// --- a widget's editable text, as the shared edit menu sees it ---------
//
// What uui_widget_ops.edit_target fills: enough for the toolkit's
// right-click menu (ui/uui_editmenu.h) to run Cut/Copy/Paste/Undo on
// a widget it knows nothing else about. (x, y) is the caret's foot in
// content coordinates -- where the Menu key opens that menu.
struct uui_edit_target {
    struct uui_edit *ed;
    const struct uui_edit_ops *ops;
    void *text;
    int x, y;
};

#endif
