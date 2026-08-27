// type-ahead, shared by uui_listbox and uui_table. See ui/uui_seek.h.
#include "ui/uui_seek.h"
#include "rt/sys.h"   // sys_monotonic_ns() -- the type-ahead window

// Only a prefix is ever compared, so one more than the longest prefix
// is all a candidate needs. A cell formatted longer than this is
// truncated by the callback and the extra bytes could not have been
// matched anyway.
#define SEEK_TEXT_MAX (UUI_SEEK_MAX + 1)

static char fold(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

// Does item `idx` start with `prefix`? ASCII case-folded, because what
// is typed is lowercase and what is shown is "Los Angeles" -- and this
// matches what the user can SEE. The text may carry a suffix
// (uui_fileview appends '/' to a directory; System Settings appends
// "   (current)" to the row in effect); a prefix test is unaffected by
// that, which is why this is a prefix test and not a substring one.
static int matches(int idx, const char *prefix, int len,
                   uui_seek_text_fn text, void *ctx) {
    char buf[SEEK_TEXT_MAX];
    buf[0] = '\0';
    text(ctx, idx, buf, (int)sizeof buf);
    for (int i = 0; i < len; i++) {
        if (!buf[i]) return 0;
        if (fold(buf[i]) != prefix[i]) return 0;
    }
    return 1;
}

// From `start`, wrapping once, so the same letter pressed twice reaches
// the NEXT match rather than sitting on the first.
static int find(const char *prefix, int len, int start, int count,
                uui_seek_text_fn text, void *ctx) {
    if (len <= 0 || count <= 0) return -1;
    if (start < 0) start = 0;
    for (int n = 0; n < count; n++) {
        int idx = start + n;
        while (idx >= count) idx -= count;
        if (matches(idx, prefix, len, text, ctx)) return idx;
    }
    return -1;
}

void uui_seek_reset(struct uui_seek *s) {
    s->len = 0;
    s->ns = 0;
}

int uui_seek_key(struct uui_seek *s, int key, int count, int current,
                 uui_seek_text_fn text, void *ctx) {
    if (count <= 0 || !text) return -1;

    char c = fold((char)key);
    unsigned long long now = sys_monotonic_ns();
    unsigned long long window = (unsigned long long)UUI_SEEK_WINDOW_MS * 1000000ull;

    // A pause ends a multi-letter search, checked against the LAST
    // keystroke rather than the first so a slow typist still builds a
    // prefix as long as they keep going.
    //
    // CYCLING DOES NOT EXPIRE, and that is deliberate: 'h' pressed twice
    // a minute apart should still reach the second h, which is what
    // Windows Explorer and KDE both do. Only the PREFIX is what a pause
    // abandons -- 'h', a long pause, then 'e' must mean "an e", not "he".
    int expired = (s->len > 0 && now - s->ns > window);
    s->ns = now;

    int from;
    if (s->len == 1 && s->buf[0] == c) {
        from = current + 1;  // the same single letter again: the next match
    } else if (expired || s->len == 0 || s->len >= UUI_SEEK_MAX) {
        s->buf[0] = c;
        s->len = 1;
        from = 0;
    } else {
        s->buf[s->len++] = c;
        // A GROWN prefix searches from the top, not from the current
        // row: "h" then "o" must be free to go backwards to "Honolulu"
        // from wherever "Halifax" left the selection.
        from = 0;
    }

    int idx = find(s->buf, s->len, from, count, text, ctx);
    if (idx < 0 && s->len > 1) {
        // Nothing starts with the whole prefix. Treat the new key as the
        // start of a fresh search rather than as a dead end -- the
        // alternative is a list that stops responding to the keyboard
        // until the window expires.
        s->buf[0] = c;
        s->len = 1;
        idx = find(s->buf, 1, 0, count, text, ctx);
    }
    if (idx == current) return -1;
    return idx;
}
