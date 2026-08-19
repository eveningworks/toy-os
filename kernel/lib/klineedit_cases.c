// See klineedit_cases.h. The cases are deliberately the bash-fidelity
// details rather than the easy ones -- the places a reimplementation
// (or a second compilation that somehow differed) would go subtly
// wrong: the two different word definitions, a kill ring that yanks
// back what it took, an undo stack, and Ctrl-D meaning two things
// depending on where the cursor is.
#include "klineedit_cases.h"
#include "klineedit.h"
#include "keyboard.h"
#include "string.h"

#define CTRL(c) ((c) - 'a' + 1)
#define ESC 0x1B

// Each array is the literal key stream a front end would feed in, in
// the encoding keyboard.h defines: ASCII for printables and control
// codes, 0x91-0xA6 for specials, and ESC-then-key for a Meta binding.
static const int k_insert_mid[]  = { 'h','e','l','o', KEY_ARROW_LEFT, 'l' };
static const int k_home_end[]    = { 'a','b','c', KEY_HOME, 'x', KEY_END, 'y' };
static const int k_ctrl_a_e[]    = { 'a','b','c', CTRL('a'), 'x', CTRL('e'), 'y' };
static const int k_kill_yank[]   = { 'o','n','e',' ','t','w','o', CTRL('u'), CTRL('y') };
static const int k_ctrl_w[]      = { '/','b','i','n','/','l','s', CTRL('w') };
static const int k_meta_bksp[]   = { '/','b','i','n','/','l','s', ESC, 0x7F };
static const int k_meta_b[]      = { 'o','n','e',' ','t','w','o', ESC, 'b', 'X' };
static const int k_meta_d[]      = { 'o','n','e',' ','t','w','o', ESC, 'b', ESC, 'd' };
static const int k_undo[]        = { 'a','b','c', CTRL('u'), CTRL('x'), CTRL('u') };
static const int k_delete[]      = { 'a','b','c', KEY_HOME, KEY_DELETE };
static const int k_ctrl_k[]      = { 'a','b','c','d', KEY_HOME, KEY_ARROW_RIGHT, CTRL('k') };
static const int k_ctrl_d_mid[]  = { 'a','b','c', KEY_HOME, CTRL('d') };
static const int k_transpose[]   = { 'a','b', CTRL('t') };

#define CASE(n, arr, want_s, want_c) \
    { n, arr, (int)(sizeof(arr) / sizeof((arr)[0])), want_s, want_c }

const struct kline_case kline_cases[] = {
    CASE("insert at the cursor",            k_insert_mid, "hello",    4),
    CASE("Home/End",                        k_home_end,   "xabcy",    5),
    CASE("Ctrl-A / Ctrl-E",                 k_ctrl_a_e,   "xabcy",    5),
    CASE("Ctrl-U kills backwards, Ctrl-Y yanks it back", k_kill_yank, "one two", 7),
    // The two word definitions, which is the pair most worth pinning:
    // Ctrl-W is whitespace-delimited so it takes the whole path, while
    // Alt-Backspace is alphanumeric so it takes only "ls".
    CASE("Ctrl-W takes the whole path",     k_ctrl_w,     "",         0),
    CASE("Alt-Backspace takes one word",    k_meta_bksp,  "/bin/",    5),
    CASE("Alt-B lands at the word start",   k_meta_b,     "one Xtwo", 5),
    CASE("Alt-D kills the word forward",    k_meta_d,     "one ",     4),
    CASE("Ctrl-X Ctrl-U undoes the kill",   k_undo,       "abc",      3),
    CASE("Delete removes forwards",         k_delete,     "bc",       0),
    CASE("Ctrl-K kills to the end",         k_ctrl_k,     "a",        1),
    // Ctrl-D on a NON-empty line is delete-forward; on an empty one it
    // is EOF, which is an action code rather than a buffer change and
    // so is asserted in klineedit_test.c instead.
    CASE("Ctrl-D mid-line deletes forward", k_ctrl_d_mid, "bc",       0),
    CASE("Ctrl-T transposes",               k_transpose,  "ba",       2),
};

const int kline_case_count = (int)(sizeof kline_cases / sizeof kline_cases[0]);

int kline_case_run(const struct kline_case *c, struct kline_edit *e,
                   char *got, int cap, int *got_cursor) {
    kline_init(e);
    for (int i = 0; i < c->nkeys; i++) kline_key(e, c->keys[i]);

    k_strlcpy(got, e->buf, (size_t)cap);
    *got_cursor = e->cursor;
    return k_strcmp(e->buf, c->want) == 0 && e->cursor == c->want_cursor;
}
