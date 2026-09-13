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

// SHARED BY EVERY EDITOR IN A PROCESS, which is readline's behaviour and
// the reason a cut in one prompt yanks into the next. Each entry is
// sized to what was cut: it used to be a fixed KLINE_MAX row, so cutting
// a line longer than that silently yanked back a SHORTER one -- the
// truncation this file refuses everywhere else.
//
// Each entry remembers the allocator that made it, because the editor
// that cut it may be gone by the time the ring evicts the entry.
static struct {
    char *text;
    const struct kline_mem *mem;
} g_kill[KILL_RING_SIZE];
// ONE ROW PER SLOT, not one shared buffer. A single shared row made
// every slot alias it, so a second kill overwrote the first and
// yank-pop rotated back to the same text -- silently turning a ring
// into a single entry. The rows cost what the old fixed ring cost and
// are used only when there is no allocator.
static char g_kill_inline[KILL_RING_SIZE][KLINE_INLINE];
static int g_kill_count = 0; // entries in use, capped at KILL_RING_SIZE
static int g_kill_head = 0;  // index of the most recent entry
static int g_yank_index = 0; // which entry the next yank-pop reaches for

// Returns 0 when the text could not be stored, which the caller must
// treat as a reason NOT to delete it: with the snapshot dropped for
// the same out-of-memory reason, a deletion here is unrecoverable by
// either route -- silent data loss where a refusal was intended.
static int kill_push(struct kline_edit *e, const char *text, int len) {
    if (len <= 0) return 1;
    int head = (g_kill_head + 1) % KILL_RING_SIZE;

    char *copy = 0;
    if (e->mem && e->mem->alloc) copy = e->mem->alloc((unsigned long)len + 1);
    if (copy) {
        k_memcpy(copy, text, (size_t)len);
        copy[len] = '\0';
        if (g_kill[head].text && g_kill[head].mem && g_kill[head].mem->free)
            g_kill[head].mem->free(g_kill[head].text);
        g_kill[head].text = copy;
        g_kill[head].mem = e->mem;
    } else {
        // No allocator, or it said no. One shared inline entry rather
        // than a truncated ring row: a short cut still yanks correctly,
        // and a long one is not silently shortened into the ring where
        // it would look like the text that was cut.
        if (len > KLINE_INLINE - 1) return 0;   // refuse rather than shorten
        k_memcpy(g_kill_inline[head], text, (size_t)len);
        g_kill_inline[head][len] = '\0';
        if (g_kill[head].text && g_kill[head].mem && g_kill[head].mem->free)
            g_kill[head].mem->free(g_kill[head].text);
        g_kill[head].text = g_kill_inline[head];
        g_kill[head].mem = 0;
    }
    g_kill_head = head;
    if (g_kill_count < KILL_RING_SIZE) g_kill_count++;
    g_yank_index = g_kill_head;
    return 1;
}

static const char *kill_at(int index) {
    if (g_kill_count == 0) return 0;
    int i = ((index % KILL_RING_SIZE) + KILL_RING_SIZE) % KILL_RING_SIZE;
    return g_kill[i].text;
}

// Reverses buf[a, b) in place. The building block of the rotation
// transpose-words uses -- see there for why it is a rotate.
static void reverse_span(char *buf, int a, int b) {
    for (int i = a, j = b - 1; i < j; i++, j--) {
        char t = buf[i]; buf[i] = buf[j]; buf[j] = t;
    }
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
        if (e->undo[0].buf && e->mem && e->mem->free) e->mem->free(e->undo[0].buf);
        for (int i = 1; i < KLINE_UNDO_DEPTH; i++) e->undo[i - 1] = e->undo[i];
        e->undo_count--;
        e->undo[e->undo_count].buf = 0;
    }
    // Sized to the line, and DROPPED rather than truncated when it
    // cannot be had: losing an undo step is a small visible loss, while
    // restoring half a line over a whole one is a wrong line.
    char *snap = 0;
    if (e->mem && e->mem->alloc) snap = e->mem->alloc((unsigned long)e->len + 1);
    if (!snap) return;
    k_memcpy(snap, e->buf, (size_t)e->len);
    snap[e->len] = '\0';
    e->undo[e->undo_count].buf = snap;
    e->undo[e->undo_count].len = e->len;
    e->undo[e->undo_count].cursor = e->cursor;
    e->undo_count++;
}


// --- the line's memory --------------------------------------------------
//
// A line starts in `e->inln` and allocates only when it outgrows it, so
// an ordinary command costs nothing. Doubling rather than growing by a
// character: a paste inserts one at a time, and a linear grow would copy
// the line once per character.
//
// A REFUSED GROWTH IS NOT A FAILURE, it is the old behaviour. Every
// caller of this already handled a full line -- insert_char() has always
// refused rather than truncated -- so an allocator that says no leaves
// the editor exactly where it was before allocators existed, which is
// what makes a NULL `mem` a supported answer rather than a crash.
static int ensure_cap(struct kline_edit *e, int need) {
    if (need <= e->cap) return 1;
    if (!e->mem || !e->mem->alloc) return 0;
    // Never past what the front end says it can run.
    if (e->mem->limit && (unsigned long)(need - 1) > e->mem->limit) return 0;

    int want = e->cap ? e->cap : KLINE_INLINE;
    while (want < need) {
        if (want > (1 << 20)) return 0;   // a command line, not a document
        want *= 2;
    }
    char *nb = e->mem->alloc((unsigned long)want);
    if (!nb) return 0;

    k_memcpy(nb, e->buf, (size_t)e->len);
    nb[e->len] = '\0';
    if (e->buf != e->inln && e->mem->free) e->mem->free(e->buf);
    e->buf = nb;
    e->cap = want;
    return 1;
}

static void delete_range(struct kline_edit *e, int start, int end, int save_to_kill) {
    if (start < 0) start = 0;
    if (end > e->len) end = e->len;
    if (start >= end) return;
    // BEFORE the snapshot and before the delete: text that cannot be
    // put in the kill ring must not be removed from the line.
    if (save_to_kill && !kill_push(e, e->buf + start, end - start)) return;
    snapshot(e);
    k_memmove(e->buf + start, e->buf + end, (size_t)(e->len - end));
    e->len -= (end - start);
    e->buf[e->len] = '\0';
    e->cursor = start;
}

static void insert_char(struct kline_edit *e, char c) {
    // Refuse rather than truncate elsewhere, exactly as before -- what
    // changed is that "full" is now a cap that can move.
    if (!ensure_cap(e, e->len + 2)) return;
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

void kline_init_mem(struct kline_edit *e, const struct kline_mem *mem) {
    k_memset(e, 0, sizeof(*e));
    e->buf = e->inln;
    e->cap = KLINE_INLINE;
    e->buf[0] = '\0';
    e->mem = mem;
}

void kline_init(struct kline_edit *e) { kline_init_mem(e, 0); }

void kline_free(struct kline_edit *e) {
    if (e->mem && e->mem->free) {
        for (int i = 0; i < e->undo_count; i++)
            if (e->undo[i].buf) { e->mem->free(e->undo[i].buf); e->undo[i].buf = 0; }
        if (e->buf && e->buf != e->inln) e->mem->free(e->buf);
    }
    // RESET ON EVERY PATH, including the one that frees nothing. Pointing
    // buf back at the 128-byte inline array while len still says 400
    // makes the next `buf[len] = 0` write past the struct.
    e->undo_count = 0;
    e->buf = e->inln;
    e->cap = KLINE_INLINE;
    e->len = e->cursor = 0;
    e->inln[0] = '\0';
}

void kline_set(struct kline_edit *e, const char *s) {
    if (!s) s = "";
    size_t want = k_strlen(s) + 1;
    if (want > (size_t)e->cap) (void)ensure_cap(e, (int)want);
    size_t n = k_strlcpy(e->buf, s, (uint32_t)e->cap);
    e->len = (int)(n < (size_t)e->cap ? n : (size_t)e->cap - 1);
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
    char *snap = e->undo[e->undo_count].buf;
    int len = e->undo[e->undo_count].len;
    if (snap && ensure_cap(e, len + 1)) {
        k_memcpy(e->buf, snap, (size_t)len);
        e->buf[len] = '\0';
        e->len = len;
        e->cursor = e->undo[e->undo_count].cursor;
    }
    if (snap && e->mem && e->mem->free) e->mem->free(snap);
    e->undo[e->undo_count].buf = 0;
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
        e->buf[i] = (char)(up ? k_toupper((unsigned char)c) : k_tolower((unsigned char)c));
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
        // REBUILT IN PLACE THROUGH A ROTATION, not through a staging
        // buffer. It used to stage into a fixed KLINE_MAX array and cap
        // each copy at 127, which rewrote only the first 127 bytes of
        // the span and left the ORIGINAL text in the tail -- a silently
        // mangled line, and impossible only while a line could not
        // exceed 128. A rotate needs no second buffer at all.
        //
        // The span is [start1, end2): word1, gap, word2 becomes word2,
        // gap, word1. Three reversals do that in place, which is the
        // standard rotate and is what makes it length-independent.
        snapshot(e);
        reverse_span(e->buf, start1, end1);   // word1
        reverse_span(e->buf, start2, end2);   // word2
        reverse_span(e->buf, end1, start2);   // the gap between them
        reverse_span(e->buf, start1, end2);   // then the whole span
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

    // dispatch-ok: a KEYMAP, bounded by the key set rather than by the
    // system's capabilities -- it does not grow when toy-os gains a
    // feature, which is the growth a table would be protecting against.
    // Each arm is also two or three lines of editing, not a subsystem
    // wanting somewhere else to live. See tools/check_dispatch.py.
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

// See klineedit.h. The decode belongs here rather than in each of the
// three front ends, for the same reason the keymap does.
enum kline_action kline_feed(struct kline_edit *e, struct termkey_state *st,
                             int byte) {
    int k = termkey_feed(st, byte);
    if (k == TERMKEY_MORE) return KLINE_IGNORED;   // mid-sequence
    if (k == TERMKEY_NONE) return KLINE_IGNORED;   // a sequence we do not know
    // A LONE ESC comes back as ESC and the byte that ended it has NOT
    // been consumed -- feed it again, which is what makes Alt-<key>
    // still work (termkey.h says why it resolves this way).
    if (k == 0x1B) {
        enum kline_action a = kline_key(e, 0x1B);
        enum kline_action b = kline_feed(e, st, byte);
        return b == KLINE_IGNORED ? a : b;
    }
    return kline_key(e, k);
}
