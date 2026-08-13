// See klineedit.h for the API and why this is render-free.
//
// The keymap follows bash/readline, including the details that look
// like inconsistencies until you check them against the real thing:
//
//   - Ctrl-W and Alt-Backspace both "kill a word backwards" but use
//     DIFFERENT word definitions (whitespace-delimited vs
//     alphanumeric). Over "/bin/ls", Ctrl-W kills the whole path and
//     Alt-Backspace kills just "ls". That is readline's
//     unix-word-rubout vs backward-kill-word, and people rely on both.
//   - Ctrl-U kills from the cursor to the START of the line, not the
//     whole line. On an empty-to-the-left line it does nothing.
//   - Ctrl-D on an EMPTY line is end-of-input; anywhere else it is
//     delete-forward. One key, two meanings, decided by the buffer.
//   - Alt-Y (yank-pop) is only legal immediately after a yank or
//     another yank-pop, which is why last_was_yank exists.
//
// Ctrl arrives as a control code and Alt as an ESC prefix (see
// keyboard.h), so the decoder here is the same one a real serial
// terminal would need -- meta_pending holds the ESC until the next key
// decides whether it was Meta or a lone Esc.
#include "klineedit.h"
#include "string.h"
#include "keyboard.h"

// Control codes, named. Writing 0x17 in the switch below and trusting
// the reader to know it is Ctrl-W is how a keymap becomes unreadable.
#define CTRL(c) ((c) - 'a' + 1)
#define KEY_ESC 0x1B
#define KEY_CTRL_UNDERSCORE 0x1F // Ctrl-_ -- readline's undo

// ---- the kill ring ----
//
// Shared between every kline_edit instance rather than per-line, which
// is both what a real shell does (the ring outlives any one command)
// and, here, deliberately cross-front-end: kill a word in the physical
// shell and Ctrl-Y yanks it in the GUI Terminal. It also keeps
// `struct kline_edit` small enough for both front ends to hold by
// value.
#define KILL_RING_SIZE 8
static char g_kill[KILL_RING_SIZE][KLINE_MAX];
static int g_kill_count = 0; // entries in use, capped at KILL_RING_SIZE
static int g_kill_head = 0;  // index of the most recent entry
static int g_yank_index = 0; // which entry the next yank-pop reaches for

static void kill_push(const char *text, int len) {
    if (len <= 0) return;
    g_kill_head = (g_kill_head + 1) % KILL_RING_SIZE;
    int n = len < KLINE_MAX - 1 ? len : KLINE_MAX - 1;
    k_memcpy(g_kill[g_kill_head], text, (size_t)n);
    g_kill[g_kill_head][n] = '\0';
    if (g_kill_count < KILL_RING_SIZE) g_kill_count++;
    g_yank_index = g_kill_head;
}

static const char *kill_at(int index) {
    if (g_kill_count == 0) return 0;
    int i = ((index % KILL_RING_SIZE) + KILL_RING_SIZE) % KILL_RING_SIZE;
    return g_kill[i];
}

// ---- word boundaries (see klineedit.h on why there are two kinds) ----

static int is_word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

int kline_word_start(const char *buf, int len, int from) {
    (void)len;
    int i = from;
    while (i > 0 && !is_word_char(buf[i - 1])) i--; // skip separators left
    while (i > 0 && is_word_char(buf[i - 1])) i--;  // then the word itself
    return i;
}

int kline_word_end(const char *buf, int len, int from) {
    int i = from;
    while (i < len && !is_word_char(buf[i])) i++;
    while (i < len && is_word_char(buf[i])) i++;
    return i;
}

int kline_ws_word_start(const char *buf, int len, int from) {
    (void)len;
    int i = from;
    while (i > 0 && buf[i - 1] == ' ') i--;
    while (i > 0 && buf[i - 1] != ' ') i--;
    return i;
}

// ---- buffer primitives ----

static void snapshot(struct kline_edit *e) {
    if (e->undo_count == KLINE_UNDO_DEPTH) {
        // Full: drop the oldest so the most RECENT steps survive, which
        // is the useful end of the stack.
        for (int i = 1; i < KLINE_UNDO_DEPTH; i++) e->undo[i - 1] = e->undo[i];
        e->undo_count--;
    }
    k_memcpy(e->undo[e->undo_count].buf, e->buf, (size_t)e->len);
    e->undo[e->undo_count].buf[e->len] = '\0';
    e->undo[e->undo_count].len = e->len;
    e->undo[e->undo_count].cursor = e->cursor;
    e->undo_count++;
}

static void delete_range(struct kline_edit *e, int start, int end, int save_to_kill) {
    if (start < 0) start = 0;
    if (end > e->len) end = e->len;
    if (start >= end) return;
    snapshot(e);
    if (save_to_kill) kill_push(e->buf + start, end - start);
    k_memmove(e->buf + start, e->buf + end, (size_t)(e->len - end));
    e->len -= (end - start);
    e->buf[e->len] = '\0';
    e->cursor = start;
}

static void insert_char(struct kline_edit *e, char c) {
    if (e->len >= KLINE_MAX - 1) return; // full -- refuse rather than truncate elsewhere
    k_memmove(e->buf + e->cursor + 1, e->buf + e->cursor, (size_t)(e->len - e->cursor));
    e->buf[e->cursor] = c;
    e->len++;
    e->cursor++;
    e->buf[e->len] = '\0';
}

void kline_insert_str(struct kline_edit *e, const char *s) {
    if (!s || !*s) return;
    snapshot(e);
    for (; *s; s++) insert_char(e, *s);
}

void kline_init(struct kline_edit *e) {
    k_memset(e, 0, sizeof(*e));
}

void kline_set(struct kline_edit *e, const char *s) {
    if (!s) s = "";
    size_t n = k_strlcpy(e->buf, s, KLINE_MAX);
    e->len = (int)(n < KLINE_MAX ? n : KLINE_MAX - 1);
    e->cursor = e->len; // end of line, same as readline's history recall
    e->last_was_yank = 0;
}

// Replaces the region the last yank inserted with the next-older kill
// ring entry. Only reachable directly after a yank (readline's rule),
// enforced by the caller checking last_was_yank.
static void yank_pop(struct kline_edit *e) {
    if (g_kill_count < 2) return; // nothing else in the ring to rotate to
    g_yank_index--;
    const char *text = kill_at(g_yank_index);
    if (!text) return;

    // Remove what the previous yank put in, then insert the new one at
    // the same place. No snapshot: the previous yank already took one,
    // and undoing a yank-pop chain back to the original is more useful
    // than undoing each rotation.
    k_memmove(e->buf + e->yank_start, e->buf + e->yank_end,
               (size_t)(e->len - e->yank_end));
    e->len -= (e->yank_end - e->yank_start);
    e->cursor = e->yank_start;
    for (const char *p = text; *p; p++) insert_char(e, *p);
    e->buf[e->len] = '\0';
    e->yank_end = e->cursor;
    e->last_was_yank = 1;
}

static void yank(struct kline_edit *e) {
    const char *text = kill_at(g_yank_index);
    if (!text || !*text) return;
    snapshot(e);
    e->yank_start = e->cursor;
    for (const char *p = text; *p; p++) insert_char(e, *p);
    e->yank_end = e->cursor;
    e->last_was_yank = 1;
}

static void undo(struct kline_edit *e) {
    if (e->undo_count == 0) return;
    e->undo_count--;
    k_strlcpy(e->buf, e->undo[e->undo_count].buf, KLINE_MAX);
    e->len = e->undo[e->undo_count].len;
    e->cursor = e->undo[e->undo_count].cursor;
}

// Applies `fn` to each character of the word at/after the cursor and
// leaves the cursor past it -- the shape Alt-U/Alt-L/Alt-C share.
// `first_only` capitalizes (Alt-C): first character up, rest down.
static void case_word(struct kline_edit *e, int upper, int first_only) {
    int start = e->cursor;
    while (start < e->len && !is_word_char(e->buf[start])) start++;
    int end = kline_word_end(e->buf, e->len, e->cursor);
    if (start >= end) { e->cursor = end; return; }
    snapshot(e);
    for (int i = start; i < end; i++) {
        char c = e->buf[i];
        int up = first_only ? (i == start) : upper;
        if (up) { if (c >= 'a' && c <= 'z') e->buf[i] = (char)(c - 32); }
        else    { if (c >= 'A' && c <= 'Z') e->buf[i] = (char)(c + 32); }
    }
    e->cursor = end;
}

static void transpose_chars(struct kline_edit *e) {
    // readline: at end of line, transposes the last two characters;
    // otherwise the one before the cursor with the one under it, then
    // advances. Nothing to do with fewer than two characters.
    if (e->len < 2) return;
    snapshot(e);
    int i = (e->cursor >= e->len) ? e->len - 1 : e->cursor;
    if (i == 0) i = 1;
    char t = e->buf[i - 1];
    e->buf[i - 1] = e->buf[i];
    e->buf[i] = t;
    e->cursor = (i + 1 <= e->len) ? i + 1 : e->len;
}

// Meta (Alt) bindings -- reached after an ESC prefix.
static enum kline_action meta_key(struct kline_edit *e, int key) {
    switch (key) {
    case 'b': e->cursor = kline_word_start(e->buf, e->len, e->cursor); return KLINE_REDRAW;
    case 'f': e->cursor = kline_word_end(e->buf, e->len, e->cursor); return KLINE_REDRAW;
    case 'd': delete_range(e, e->cursor, kline_word_end(e->buf, e->len, e->cursor), 1);
              return KLINE_REDRAW;
    case 'u': case_word(e, 1, 0); return KLINE_REDRAW;
    case 'l': case_word(e, 0, 0); return KLINE_REDRAW;
    case 'c': case_word(e, 0, 1); return KLINE_REDRAW;
    case 'y':
        // Only after a yank -- otherwise readline rings the bell, and
        // here it simply does nothing.
        if (!e->last_was_yank) return KLINE_IGNORED;
        yank_pop(e);
        return KLINE_REDRAW;
    case 't': { // transpose-words
        int end2 = kline_word_end(e->buf, e->len, e->cursor);
        int start2 = kline_word_start(e->buf, e->len, end2);
        int start1 = kline_word_start(e->buf, e->len, start2);
        int end1 = kline_word_end(e->buf, e->len, start1);
        if (start1 >= end1 || start2 >= end2 || end1 > start2) return KLINE_IGNORED;
        // Rebuild the span between the two word starts: second word,
        // the separator that was between them, then the first word.
        char tmp[KLINE_MAX];
        int n = 0;
        for (int i = start2; i < end2 && n < KLINE_MAX - 1; i++) tmp[n++] = e->buf[i];
        for (int i = end1; i < start2 && n < KLINE_MAX - 1; i++) tmp[n++] = e->buf[i];
        for (int i = start1; i < end1 && n < KLINE_MAX - 1; i++) tmp[n++] = e->buf[i];
        snapshot(e);
        k_memcpy(e->buf + start1, tmp, (size_t)n);
        e->cursor = end2;
        return KLINE_REDRAW;
    }
    case 0x7F: case '\b': // Alt-Backspace: alnum-word kill (cf. Ctrl-W)
        delete_range(e, kline_word_start(e->buf, e->len, e->cursor), e->cursor, 1);
        return KLINE_REDRAW;
    case '.': case '_':
        return KLINE_LAST_ARG; // the front end owns history; it inserts the word
    default:
        return KLINE_IGNORED;
    }
}

enum kline_action kline_key(struct kline_edit *e, int key) {
    // A pending ESC decides here: this key makes it a Meta binding.
    if (e->meta_pending) {
        e->meta_pending = 0;
        e->last_was_yank = (key == 'y') ? e->last_was_yank : 0;
        return meta_key(e, key);
    }
    if (e->ctrl_x_pending) {
        e->ctrl_x_pending = 0;
        if (key == CTRL('u')) { undo(e); return KLINE_REDRAW; } // Ctrl-X Ctrl-U
        return KLINE_IGNORED;
    }

    // Any key that isn't a yank ends the yank sequence -- checked
    // before the switch so every branch below doesn't have to. ESC is
    // exempt: it's the first half of a Meta sequence that might BE the
    // yank-pop, and clearing the flag here would make Alt-Y impossible
    // to reach (it did, until a test caught it).
    if (key != CTRL('y') && key != KEY_ESC) e->last_was_yank = 0;

    switch (key) {
    case KEY_ESC:
        e->meta_pending = 1;
        return KLINE_IGNORED; // nothing visible happens until the next key
    case CTRL('x'):
        e->ctrl_x_pending = 1;
        return KLINE_IGNORED;

    // ---- accept / abandon ----
    case '\r':
    case '\n':
        return KLINE_ACCEPT;
    case CTRL('c'):
        return KLINE_CANCEL;
    case CTRL('d'):
        // Empty line = end of input; otherwise delete-forward. See this
        // file's top comment.
        if (e->len == 0) return KLINE_EOF;
        if (e->cursor >= e->len) return KLINE_IGNORED;
        delete_range(e, e->cursor, e->cursor + 1, 0);
        return KLINE_REDRAW;

    // ---- motion ----
    case KEY_ARROW_LEFT:
    case CTRL('b'):
        if (e->cursor == 0) return KLINE_IGNORED;
        e->cursor--;
        return KLINE_REDRAW;
    case KEY_ARROW_RIGHT:
    case CTRL('f'):
        if (e->cursor >= e->len) return KLINE_IGNORED;
        e->cursor++;
        return KLINE_REDRAW;
    case KEY_HOME:
    case CTRL('a'):
        if (e->cursor == 0) return KLINE_IGNORED;
        e->cursor = 0;
        return KLINE_REDRAW;
    case KEY_END:
    case CTRL('e'):
        if (e->cursor == e->len) return KLINE_IGNORED;
        e->cursor = e->len;
        return KLINE_REDRAW;
    case KEY_CTRL_ARROW_LEFT:
        e->cursor = kline_word_start(e->buf, e->len, e->cursor);
        return KLINE_REDRAW;
    case KEY_CTRL_ARROW_RIGHT:
        e->cursor = kline_word_end(e->buf, e->len, e->cursor);
        return KLINE_REDRAW;

    // ---- editing ----
    case '\b':
    case 0x7F: // some keyboards/terminals send DEL for backspace
        if (e->cursor == 0) return KLINE_IGNORED;
        delete_range(e, e->cursor - 1, e->cursor, 0);
        return KLINE_REDRAW;
    case KEY_DELETE:
        if (e->cursor >= e->len) return KLINE_IGNORED;
        delete_range(e, e->cursor, e->cursor + 1, 0);
        return KLINE_REDRAW;
    case CTRL('k'): // kill to end of line
        if (e->cursor >= e->len) return KLINE_IGNORED;
        delete_range(e, e->cursor, e->len, 1);
        return KLINE_REDRAW;
    case CTRL('u'): // kill to start of line -- NOT the whole line, see top comment
        if (e->cursor == 0) return KLINE_IGNORED;
        delete_range(e, 0, e->cursor, 1);
        return KLINE_REDRAW;
    case CTRL('w'): // whitespace-delimited word kill (cf. Alt-Backspace)
        if (e->cursor == 0) return KLINE_IGNORED;
        delete_range(e, kline_ws_word_start(e->buf, e->len, e->cursor), e->cursor, 1);
        return KLINE_REDRAW;
    case CTRL('y'):
        yank(e);
        return KLINE_REDRAW;
    case CTRL('t'):
        transpose_chars(e);
        return KLINE_REDRAW;
    case KEY_CTRL_UNDERSCORE:
        undo(e);
        return KLINE_REDRAW;

    // ---- things the front end owns ----
    case '\t':
        return KLINE_COMPLETE;
    case CTRL('l'):
        return KLINE_CLEAR_SCREEN;
    case CTRL('r'):
        return KLINE_SEARCH;
    case KEY_ARROW_UP:
    case CTRL('p'):
        return KLINE_HISTORY_PREV;
    case KEY_ARROW_DOWN:
    case CTRL('n'):
        return KLINE_HISTORY_NEXT;

    default:
        if (IS_PRINTABLE_KEY(key)) {
            insert_char(e, (char)key);
            return KLINE_REDRAW;
        }
        return KLINE_IGNORED;
    }
}
