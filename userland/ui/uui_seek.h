#ifndef UUI_SEEK_H
#define UUI_SEEK_H

// --- type-ahead, shared by every list-shaped widget -------------------
//
// A printable key jumps to the first item starting with it, and keys
// typed in quick succession build a longer prefix -- 'h', 'e' reaches
// "Helsinki" past "Halifax" and "Hanoi". Pressing the SAME single letter
// again cycles through the items starting with it. This is what Windows'
// list views and KDE's item views both do, and with 92 timezones (or a
// directory of files) it is the difference between a list and a scroll.
//
// The window is why a longer prefix cannot be a mode: after this long
// with no key, the next letter starts a fresh search, so a list can
// never be left in a state where typing 'h' does not go to an 'h'.
//
// It is a CALLBACK rather than an array of strings because uui_table
// stores no rows at all -- it pulls one cell at a time (ui/uui_table.h),
// while uui_listbox holds pointers. One core serves both.
#define UUI_SEEK_MAX 24
#define UUI_SEEK_WINDOW_MS 1000

struct uui_seek {
    // The prefix typed so far, and when its last keystroke arrived; a
    // gap longer than UUI_SEEK_WINDOW_MS starts a new search rather
    // than extending an abandoned one.
    char buf[UUI_SEEK_MAX];
    int len;
    unsigned long long ns;
};

// Item `idx`'s searchable text, written into `out`. Only the first
// UUI_SEEK_MAX characters are ever compared, so a longer cell may be
// truncated -- which is why `cap` is small and a caller must not assume
// it gets a whole filename back.
typedef void (*uui_seek_text_fn)(void *ctx, int idx, char *out, int cap);

void uui_seek_reset(struct uui_seek *s);

// A printable key. Returns the index to select, or -1 for "no move" --
// which covers an empty list, no match, and a match on the item already
// selected. `current` is where the selection is now, in the SAME index
// space the callback speaks; a widget that sorts must pass VIEW
// positions and convert the answer back, or cycling walks the data's
// order while the user watches the screen's.
int uui_seek_key(struct uui_seek *s, int key, int count, int current,
                 uui_seek_text_fn text, void *ctx);

// Is this a key the search takes? Space is excluded: it activates a
// control (and marks a file in uui_fileview), and no item starts with
// one. KEY_* specials are all >= 0x91 (api/keyboard.h), so this cannot
// catch an arrow.
static inline int uui_seek_is_key(int key) { return key > ' ' && key < 0x7F; }

#endif
