// See api/termkey.h. Compiled TWICE -- once into the kernel, once into
// libuapp.a -- so the terminal that encodes and the editor that decodes
// are the same table, and a key added here reaches both rings at once.
#include "termkey.h"
#include "termkey_cases.h"

int termkey_encode(int key, char *out, int cap) {
    if (!out || cap <= 0) return 0;

    // An ordinary character is itself -- one byte of Latin-1, accented
    // letters included. That includes the control codes:
    // Ctrl-A really is 0x01 on a terminal, and Alt-<key> really is ESC
    // then the key, so neither needs a table row.
    if (key >= 0 && key < 0x100) { out[0] = (char)key; return 1; }

    for (int i = 0; i < TERMKEY_CASE_COUNT; i++) {
        if (TERMKEY_CASES[i].key != key) continue;
        const char *s = TERMKEY_CASES[i].seq;
        int n = 0;
        while (s[n]) n++;
        if (n > cap) return 0;          // REFUSED, never truncated
        for (int j = 0; j < n; j++) out[j] = s[j];
        return n;
    }
    // A key with no terminal representation -- Super, a modifier press.
    // Sending nothing is right: a program reading a terminal could not
    // have acted on it anyway.
    return 0;
}

const char *termkey_pending(const struct termkey_state *st, int *len) {
    if (len) *len = st ? st->len : 0;
    return st ? st->pending : "";
}

// Does `st->pending` match a case exactly, or could it still grow into
// one? Returns the key, TERMKEY_MORE, or TERMKEY_NONE.
static int match(const struct termkey_state *st) {
    int partial = 0;
    for (int i = 0; i < TERMKEY_CASE_COUNT; i++) {
        const char *s = TERMKEY_CASES[i].seq;
        int n = 0;
        while (s[n]) n++;
        if (n < st->len) continue;
        int j = 0;
        while (j < st->len && s[j] == st->pending[j]) j++;
        if (j != st->len) continue;         // diverged
        if (n == st->len) return TERMKEY_CASES[i].key;
        partial = 1;                        // a prefix of this one
    }
    return partial ? TERMKEY_MORE : TERMKEY_NONE;
}

int termkey_feed(struct termkey_state *st, int byte) {
    if (!st) return TERMKEY_NONE;

    if (st->len == 0) {
        if (byte != 0x1B) return byte;      // an ordinary byte, untouched
        st->pending[0] = 0x1B;
        st->len = 1;
        return TERMKEY_MORE;
    }

    // **THE SECOND BYTE DECIDES WHAT THE ESC WAS.** `[` or `O` begins a
    // sequence; anything else means the ESC was the Esc key or a Meta
    // prefix, so give the ESC back and let the caller re-feed the byte.
    // Resolving on the next byte rather than on a timer is the one real
    // difference from a hardware terminal, and it costs Alt-[ -- the
    // same trade readline makes.
    if (st->len == 1 && byte != '[' && byte != 'O') {
        st->len = 0;
        return 0x1B;
    }

    if (st->len >= TERMKEY_MAX) { st->len = 0; return TERMKEY_NONE; }
    st->pending[st->len++] = (char)byte;

    int r = match(st);
    if (r == TERMKEY_MORE) return TERMKEY_MORE;
    st->len = 0;                            // matched, or gave up
    return r;
}
