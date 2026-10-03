#!/usr/bin/env python3
"""Check userland/ui/utext.c's wrap accounting against a naive oracle, on the HOST.

WHY A HOST HARNESS. utext gained a sparse line index (utext.h's
`struct utext_wrap`) so that a 1.6 MB document costs a screenful of work
per frame instead of a documentful. An accelerator is exactly the kind
of code that is right on the corpus somebody tried and wrong on the one
they did not: the failure is a caret one character off, or a click
landing on the wrong line, in a file with a particular mix of long lines
and hard newlines. That is a sweep over many documents, not a
screenshot.

So this compiles the SAME .c file with the host gcc and checks it two
ways, which prove different things and are worth keeping apart:

**The RULE, against explicit cases.** A handful of crafted inputs with
the lines written out by hand: word wrap breaks at a space and not
mid-word, a word wider than the view breaks hard because it has nowhere
else to go, the spaces a break consumed do not begin the next line, and
wrap OFF does not break at all. These are the only checks that can
catch the wrap rule itself being wrong, and they are hand-written for
exactly that reason.

**The ACCELERATOR, against a naive walk.** utext keeps a sparse
checkpoint table so a 1.6 MB document costs a screenful of work per
frame instead of a documentful, and every lookup starts from a
checkpoint rather than from character 0. The oracle walks the same
spans from 0 with no checkpoints at all, over this repo's own Markdown
and pci.ids, and the two must agree about the total line count and
about where each visible line begins. This half deliberately SHARES the
wrap rule -- it is testing the index, and the previous half is testing
the rule.

Plus, over the same corpus:

  - a ROUND TRIP: the point at which the oracle says character i is
    drawn hit-tests back to i, for every character on screen, at five
    scroll positions. This is what catches draw() and index_at_point()
    drifting apart.
  - scroll_top() really shows line 0 and scroll_bottom() the last screen
  - selection text out and pasted text in, including that '\r' is dropped
  - utext_putc REFUSES at capacity rather than dropping the oldest
    character (the bug that made Notepad open pci.ids as its last 8 KB)
  - the edit HISTORY (ui/uui_undo.c): a word, a Backspace run, a
    replacement and a paste are one step each, clean is a position,
    a new edit drops the redo, a full history forgets its oldest steps
  - the line-number GUTTER takes its share of the box in the hit test

The corpus is this repository's own Markdown plus a synthetic document
long enough to force the checkpoint table to compact (which is the part
with no other coverage), and data/pci.ids when it is present -- the file
the whole change exists for.

--positive-control corrupts one checkpoint in the index and requires the
checks to go RED. Without seeing that, a green run is measuring nothing.

Exit status is non-zero on any violation. Needs gcc.
"""
import argparse
import glob
import os
import subprocess
import sys
import tempfile
import hostcheck  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# utext.h's UTEXT_WRAP_*, so the corpus can name a mode.
OFF, WORD = 0, 1

DRIVER = r"""
#include <stdio.h>
#include <stdlib.h>

// NOT <string.h>: kernel/include/api is on the include path and carries
// a string.h of its own, which is the one that would be found.
static void xcpy(char *d, const char *s, int n) { for (int i = 0; i < n; i++) d[i] = s[i]; }
static int xcmp(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return 1;
    return 0;
}
static int xlen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int xeq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

#include "ui/ugfx.h"

// utext draws through ugfx; none of the wrap accounting depends on what
// a pixel does, so four stubs are the whole port. The two metrics ARE
// load-bearing -- the grid is derived from them.
int ugfx_char_w(void) { return 8; }
int ugfx_char_h(void) { return 16; }
void ugfx_fill_rect(struct ugfx_surface *s, int x, int y, int w, int h, uint32_t c)
{ (void)s; (void)x; (void)y; (void)w; (void)h; (void)c; }
void ugfx_draw_char(struct ugfx_surface *s, int x, int y, char ch, uint32_t f, uint32_t b)
{ (void)s; (void)x; (void)y; (void)ch; (void)f; (void)b; }
// Smooth scrolling, the caret blink and the clip stack are drawing and
// animation, not wrap accounting -- and the real ones drag in the
// settings client. Stubbed with the no-animation answer: sync returns
// the position it was given.
#include "ui/uui_scrollanim.h"
void uui_scrollanim_init(struct uui_scrollanim *a) { (void)a; }
void uui_scrollanim_arm(struct uui_scrollanim *a) { (void)a; }
void uui_scrollanim_cancel(struct uui_scrollanim *a) { (void)a; }
int uui_scrollanim_sync(struct uui_scrollanim *a, int pos_px) { (void)a; return pos_px; }
int uui_caret_visible(void) { return 1; }
void ugfx_clip_save(const struct ugfx_surface *s, struct ugfx_clip *out)
{ (void)s; out->x0 = out->y0 = out->x1 = out->y1 = out->active = 0; }
void ugfx_clip_restore(struct ugfx_surface *s, const struct ugfx_clip *c) { (void)s; (void)c; }
void ugfx_clip_intersect(struct ugfx_surface *s, int x, int y, int w, int h)
{ (void)s; (void)x; (void)y; (void)w; (void)h; }

// **INCLUDED, NOT LINKED.** line_span() and line_begin() are static --
// they are the rule and the accelerator, and testing an accelerator
// from outside means inferring its answers from the hit test, which
// cannot see where a FULL line ends. Including the translation unit is
// the ordinary white-box move and it keeps the oracle honest.
#include "ui/utext.c"

#define CW 8
#define CH 16

// --- the oracle -------------------------------------------------------
//
// The visual lines, walked from character 0 with NO checkpoint table.
// That is the whole point: the implementation under test starts every
// lookup from the nearest checkpoint, and this starts from the
// beginning, so a wrong checkpoint or a wrong stride shows up as a
// disagreement. It shares the wrap RULE deliberately -- the rule is
// checked by the explicit cases in main(), not here.

#define MAX_LINES 200000
static int ln_start[MAX_LINES];
static int ln_end[MAX_LINES];   // one past the last DRAWN character
static int ln_count;

// Every visual line, walked from character 0 with NO checkpoint table.
// That is the whole point: the implementation under test starts every
// lookup from the nearest checkpoint, so a wrong checkpoint or a wrong
// stride shows up here as a disagreement.
static void oracle_lines(struct utext *t, int cols) {
    ln_count = 0;
    int i = 0;
    for (;;) {
        int draw_end, next;
        line_span(t, cols, i, &draw_end, &next);
        if (ln_count < MAX_LINES) {
            ln_start[ln_count] = i;
            ln_end[ln_count] = draw_end;
            ln_count++;
        }
        if (last_span(t, draw_end, next) || next <= i) break;
        i = next;
    }
}

static int fails;

static void fail(const char *what, long a, long b) {
    printf("FAIL %s: %ld != %ld\n", what, a, b);
    fails++;
}

static char *doc;
static int doclen;

static void load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("FAIL cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    doc = malloc((size_t)n + 1);
    if (!doc || fread(doc, 1, (size_t)n, f) != (size_t)n) { printf("FAIL read\n"); exit(2); }
    fclose(f);
    doclen = (int)n;
}

#define ROWS 24

static struct utext *fill(struct utext *t, const char *text, int n, int wrap) {
    static char *buf;
    free(buf);
    buf = malloc((size_t)n + 4096);
    utext_init_buf(t, buf, n + 4096);
    xcpy(buf, text, n);
    t->count = n;
    t->rev++;
    utext_set_wrap(t, wrap);
    return t;
}

// Every character on the screen shown at `scroll`, round-tripped: the
// point at which the widget draws character i must hit-test back to i.
static void check_screen(struct utext *t, int cols, int total, int scroll,
                          const char *where) {
    int w = cols * CW, h = ROWS * CH;
    t->scroll_offset = scroll;
    utext_metrics(t, w, h, NULL, NULL);

    int first = total - ROWS - t->scroll_offset;
    if (first < 0) first = 0;

    for (int row = 0; row < ROWS && first + row < ln_count; row++) {
        int s0 = ln_start[first + row];
        int e0 = ln_end[first + row];
        for (int k = s0; k <= e0 && k - s0 < cols; k++) {
            int px = (k - s0) * CW + CW / 2, py = row * CH + CH / 2;
            int got = utext_index_at_point(t, 0, 0, w, h, px, py);
            if (got != k) {
                printf("FAIL round trip at %s (row %d col %d): %d != %d\n",
                        where, row, k - s0, got, k);
                fails++;
                if (fails > 8) return;
            }
        }
    }
}

static void check_doc(const char *path, int cols, int wrap, int corrupt) {
    load(path);
    struct utext t;
    fill(&t, doc, doclen, wrap);

    int w = cols * CW, h = ROWS * CH;
    int total = 0, vis = 0;
    utext_metrics(&t, w, h, &total, &vis);
    if (vis != ROWS) fail("visible_rows", vis, ROWS);

    oracle_lines(&t, cols);
    if (total != ln_count) fail("total_lines vs the naive walk", total, ln_count);

    if (corrupt) {
        // EVERY checkpoint but the first, moved by three characters.
        // Corrupting only ONE was too narrow to be a control: the
        // sampled scroll positions each start from a different
        // checkpoint, and none of them happened to be that one -- so a
        // deliberately broken index reported as GREEN, which is exactly
        // the failure a positive control exists to catch.
        if (t.wrap_cache.n > 1)
            for (int k = 1; k < t.wrap_cache.n; k++) t.wrap_cache.idx[k] += 3;
        else { printf("FAIL corpus too small to corrupt: %s\n", path); fails++; }
    }

    // THE ACCELERATOR, directly: where the checkpointed lookup says a
    // line begins must be where walking from 0 says it does. Sampled
    // across the document, so a checkpoint deep in it is exercised.
    int step = ln_count > 400 ? ln_count / 400 : 1;
    for (int L = 0; L < ln_count; L += step) {
        int got = line_begin(&t, cols, L);
        if (got != ln_start[L]) {
            fail("line_begin", got, ln_start[L]);
            if (fails > 8) break;
        }
    }

    // The two ends and three places in between, so a lookup that starts
    // from a checkpoint DEEP in the document is exercised rather than
    // just the first screen.
    int max_scroll = total > ROWS ? total - ROWS : 0;
    check_screen(&t, cols, total, max_scroll, "top");
    check_screen(&t, cols, total, max_scroll * 3 / 4, "3/4");
    check_screen(&t, cols, total, max_scroll / 2, "middle");
    check_screen(&t, cols, total, max_scroll / 4, "1/4");
    check_screen(&t, cols, total, 0, "bottom");

    utext_scroll_top(&t);
    utext_metrics(&t, w, h, NULL, NULL);
    if (t.scroll_offset != max_scroll) fail("scroll_top offset", t.scroll_offset, max_scroll);
    utext_scroll_bottom(&t);
    utext_metrics(&t, w, h, NULL, NULL);
    if (t.scroll_offset != 0) fail("scroll_bottom offset", t.scroll_offset, 0);

    free(doc);
    doc = NULL;
}

// --- the RULE, against lines written out by hand ----------------------

static void expect_lines(const char *text, int cols, int wrap,
                          const char *const *want, int nwant, const char *what) {
    struct utext t;
    fill(&t, text, xlen(text), wrap);
    oracle_lines(&t, cols);
    if (ln_count != nwant) {
        printf("FAIL %s: %d lines, wanted %d\n", what, ln_count, nwant);
        for (int i = 0; i < ln_count; i++)
            printf("      [%d] \"%.*s\"\n", i, ln_end[i] - ln_start[i], t.buf + ln_start[i]);
        fails++;
        return;
    }
    for (int i = 0; i < nwant; i++) {
        int n = ln_end[i] - ln_start[i];
        if (n != xlen(want[i]) || xcmp(t.buf + ln_start[i], want[i], n) != 0) {
            printf("FAIL %s line %d: \"%.*s\" != \"%s\"\n",
                    what, i, n, t.buf + ln_start[i], want[i]);
            fails++;
        }
    }
}

static void check_rule(void) {
    // WORD WRAP BREAKS AT A SPACE. 10 columns; "the quick" is 9 and
    // "brown" would take it to 15, so the break is after "quick" and
    // the space itself is not carried onto the next line.
    static const char *const a[] = { "the quick", "brown fox" };
    expect_lines("the quick brown fox", 10, UTEXT_WRAP_WORD, a, 2,
                  "word wrap breaks at a space");

    // A WORD WIDER THAN THE VIEW breaks hard: there is nowhere else.
    static const char *const b[] = { "abcdefghij", "klmnop" };
    expect_lines("abcdefghijklmnop", 10, UTEXT_WRAP_WORD, b, 2,
                  "a too-long word breaks hard");

    // ...and a too-long word AFTER a fitting one still starts its own
    // line rather than being split at the margin.
    static const char *const c[] = { "hi", "abcdefghij", "klmn" };
    expect_lines("hi abcdefghijklmn", 10, UTEXT_WRAP_WORD, c, 3,
                  "a too-long word starts its own line");

    // A HARD NEWLINE always breaks, however short the line.
    static const char *const d[] = { "a", "b" };
    expect_lines("a\nb", 10, UTEXT_WRAP_WORD, d, 2, "a newline breaks");

    // WRAP OFF DOES NOT BREAK, whatever the width.
    static const char *const e[] = { "the quick brown fox" };
    expect_lines("the quick brown fox", 10, UTEXT_WRAP_OFF, e, 1,
                  "wrap off does not break");
    static const char *const f[] = { "one two", "three" };
    expect_lines("one two\nthree", 10, UTEXT_WRAP_OFF, f, 2,
                  "wrap off still breaks at a newline");
}

static void check_semantics(void) {
    static char b[64];
    struct utext t;

    // putc REFUSES at capacity. The old ring dropped the oldest, which
    // is how a 1.6 MB file arrived as its last 8 KB.
    utext_init_buf(&t, b, 8);
    for (int i = 0; i < 20; i++) utext_putc(&t, (char)('a' + i % 26));
    if (t.count != 8) fail("putc cap", t.count, 8);
    if (b[0] != 'a') fail("putc kept the head", b[0], 'a');

    // Selection out, verbatim, with the TRUE length reported even when
    // the caller's buffer is too small to take it.
    utext_init_buf(&t, b, (int)sizeof b);
    const char *s = "hello world";
    utext_insert_text(&t, s, xlen(s));
    if (t.count != 11) fail("insert_text len", t.count, 11);
    t.ed.sel_anchor = 0; t.ed.sel_active = 1; t.ed.cursor = 5;
    char out[4];
    int n = utext_sel_text(&t, out, (int)sizeof out);
    if (n != 5) fail("sel_text true length", n, 5);
    if (xcmp(out, "hell", 4) != 0) fail("sel_text bytes", 1, 0);

    // A paste REPLACES the selection, and '\r' is dropped so CRLF text
    // arrives as this buffer's own line endings.
    utext_init_buf(&t, b, (int)sizeof b);
    utext_insert_text(&t, "abcdef", 6);
    t.ed.sel_anchor = 1; t.ed.sel_active = 1; t.ed.cursor = 4;
    utext_insert_text(&t, "X\r\nY", 4);
    if (t.count != 6) fail("paste replaced selection", t.count, 6);
    if (xcmp(b, "aX\nYef", 6) != 0) {
        printf("FAIL paste bytes: %.6s != aX\\nYef\n", b);
        fails++;
    }

    // **A CLICK ARMS A SELECTION, AND AN INSERT MUST DISARM IT.**
    // utext_sel_start() sets the anchor to the caret, which is
    // invisible only while the two are equal -- so an insert that moved
    // the caret without clearing turned the text it passed over into a
    // selection. Reported as "pasted text comes out highlighted, but
    // only when the caret was moved by CLICKING": the arrow keys go
    // through the shared keymap, which already clears.
    utext_init_buf(&t, b, (int)sizeof b);
    utext_insert_text(&t, "abcdef", 6);
    t.ed.cursor = 3;
    utext_sel_start(&t);              // what a click leaves behind
    utext_insert_text(&t, "XY", 2);
    if (utext_sel_present(&t)) fail("paste after a click selects nothing", 1, 0);
    if (t.ed.cursor != 5) fail("the caret lands after the paste", t.ed.cursor, 5);

    // ...and the same for a single character, which is Enter's path.
    utext_init_buf(&t, b, (int)sizeof b);
    utext_insert_text(&t, "abcdef", 6);
    t.ed.cursor = 3;
    utext_sel_start(&t);
    utext_insert(&t, '\n');
    if (utext_sel_present(&t)) fail("Enter after a click selects nothing", 1, 0);

    // select-all spans the document whatever the caret was doing.
    utext_init_buf(&t, b, (int)sizeof b);
    utext_insert_text(&t, "one\ntwo", 7);
    t.ed.cursor = 2;
    utext_sel_all(&t);
    int a, e;
    utext_sel_range(&t, &a, &e);
    if (a != 0 || e != 7) fail("sel_all range", e - a, 7);
}

// --- the edit history (ui/uui_undo.h) ----------------------------------

static int is(const struct utext *t, const char *want) {
    int n = xlen(want);
    return t->count == n && xcmp(t->buf, want, n) == 0;
}
static void type(struct utext *t, const char *s) {
    for (; *s; s++) {
        if (*s == '\n') { utext_sel_delete(t); utext_insert(t, '\n'); }
        else utext_key(t, *s, 0);
    }
}
#define EXPECT(t, want, what) \
    do { if (!is(t, want)) { printf("FAIL %s: \"%.*s\" != \"%s\"\n", what, \
                                    (t)->count, (t)->buf, want); fails++; } } while (0)

static void check_undo(void) {
    static char b[256];
    static unsigned char hist[4096];
    struct utext t;
    struct uui_undo u;

    // A typed word is ONE step, and the space after it belongs to it.
    utext_init_buf(&t, b, (int)sizeof b);
    uui_undo_init(&u, hist, (int)sizeof hist);
    t.ed.undo = &u;
    type(&t, "hello world");
    utext_undo(&t); EXPECT(&t, "hello ", "undo a word");
    utext_undo(&t); EXPECT(&t, "", "undo the first word");
    if (utext_undo(&t)) fail("undo with nothing left", 1, 0);
    utext_redo(&t); EXPECT(&t, "hello ", "redo a word");
    utext_redo(&t); EXPECT(&t, "hello world", "redo the second");
    if (t.ed.cursor != 11) fail("redo leaves the caret after it", t.ed.cursor, 11);

    // Clean is a POSITION: undoing back to it is clean again.
    uui_undo_mark_clean(&u);
    type(&t, "!");
    if (uui_undo_is_clean(&u)) fail("an edit makes it dirty", 1, 0);
    utext_undo(&t);
    if (!uui_undo_is_clean(&u)) fail("undo back to the save is clean", 0, 1);
    EXPECT(&t, "hello world", "undo past the save point's edit");

    // Typing over a selection is one step: the erase and the insert.
    t.ed.sel_anchor = 6; t.ed.sel_active = 1; t.ed.cursor = 11;
    utext_key(&t, 'Z', 0);
    EXPECT(&t, "hello Z", "type over a selection");
    utext_undo(&t); EXPECT(&t, "hello world", "undo a replacement in one step");

    // A new edit after an undo drops what could have been redone.
    type(&t, "?");
    if (utext_redo(&t)) fail("redo after a new edit", 1, 0);

    // A run of Backspaces is one step.
    utext_init_buf(&t, b, (int)sizeof b);
    uui_undo_init(&u, hist, (int)sizeof hist);
    t.ed.undo = &u;
    type(&t, "abc");
    utext_key(&t, '\b', 0); utext_key(&t, '\b', 0); utext_key(&t, '\b', 0);
    EXPECT(&t, "", "three backspaces");
    utext_undo(&t); EXPECT(&t, "abc", "undo a backspace run");

    // A paste over a selection is one step, CRs and all.
    t.ed.sel_anchor = 0; t.ed.sel_active = 1; t.ed.cursor = 2;
    utext_insert_text(&t, "X\r\nY", 4);
    EXPECT(&t, "X\nYc", "paste over a selection");
    utext_undo(&t); EXPECT(&t, "abc", "undo a paste in one step");
    utext_redo(&t); EXPECT(&t, "X\nYc", "redo the paste");

    // A full history forgets its OLDEST steps and keeps working: every
    // remaining step undoes, and redoing them all lands on the end.
    static unsigned char tiny[96];
    utext_init_buf(&t, b, (int)sizeof b);
    uui_undo_init(&u, tiny, (int)sizeof tiny);
    t.ed.undo = &u;
    type(&t, "one two three four five six seven eight nine ten");
    int steps = 0;
    while (utext_undo(&t) && steps < 100) steps++;
    if (steps == 0 || steps >= 10) fail("a full history keeps some steps", steps, 3);
    while (utext_redo(&t)) {}
    EXPECT(&t, "one two three four five six seven eight nine ten", "redo all after forgetting");

    // A load forgets the history.
    utext_clear(&t);
    if (uui_undo_can_undo(&u)) fail("clear forgets the history", 1, 0);
}

// The gutter takes its share of the box, and the hit test knows it.
static void check_gutter(void) {
    static char b[64];
    struct utext t;
    utext_init_buf(&t, b, (int)sizeof b);
    utext_insert_text(&t, "abcdef\nghij", 11);
    t.gutter = 1;
    // Three digits plus a cell either side: 5 cells of 8 px.
    int at = utext_index_at_point(&t, 0, 0, 400, 64, 5 * CW + 3 * CW + 1, 1);
    if (at != 3) fail("a click right of the gutter", at, 3);
    at = utext_index_at_point(&t, 0, 0, 400, 64, 5 * CW + 1, CH + 1);
    if (at != 7) fail("a click on line 2 past the gutter", at, 7);
    if (utext_line_count(&t) != 2) fail("logical lines", utext_line_count(&t), 2);
    int ln, col;
    utext_line_col(&t, 9, &ln, &col);
    if (ln != 1 || col != 2) fail("line_col", ln * 100 + col, 102);
    if (utext_line_index(&t, 1) != 7) fail("line_index", utext_line_index(&t, 1), 7);
}

int main(int argc, char **argv) {
    int corrupt = argc > 1 && xeq(argv[1], "--corrupt");
    int first = corrupt ? 2 : 1;
    if (!corrupt) { check_semantics(); check_rule(); check_undo(); check_gutter(); }
    for (int i = first; i + 1 < argc; i += 3) {
        int cols = atoi(argv[i + 1]);
        int wrap = atoi(argv[i + 2]);
        check_doc(argv[i], cols, wrap, corrupt);
    }
    printf("%s (%d failures)\n", fails ? "RED" : "GREEN", fails);
    return fails ? 1 : 0;
}
"""


def build(tmp):
    drv = hostcheck.write(tmp, "utext_host.c", DRIVER)
    return hostcheck.compile(
        tmp, "utext_host", [drv, os.path.join(ROOT, "userland", "ui", "uui_edit.c"),
                            os.path.join(ROOT, "userland", "ui", "uui_undo.c")],
        includes=[os.path.join(ROOT, "userland"), os.path.join(ROOT, "kernel", "include", "api"),
                  os.path.join(ROOT, "kernel", "include", "abi")], tool="utext_hostcheck")


def corpus(tmp):
    """Documents to sweep, each with the wrap width to read it at."""
    out = []
    # A synthetic document long enough to fill the checkpoint table and
    # force it to compact -- the branch nothing else here reaches.
    long_path = os.path.join(tmp, "long.txt")
    with open(long_path, "w") as fh:
        for i in range(4000):
            fh.write("line %d of four thousand, numbered so a wrong "
                     "line is legible rather than plausible\n" % i)
    out.append((long_path, 40, WORD))
    out.append((long_path, 200, WORD))
    out.append((long_path, 40, OFF))

    # One paragraph with NO newline at all: every line break is a wrap,
    # which is the opposite corner from the file above.
    wrap_path = os.path.join(tmp, "onelong.txt")
    with open(wrap_path, "w") as fh:
        fh.write("x" * 60000)
    out.append((wrap_path, 37, WORD))

    for p in sorted(glob.glob(os.path.join(ROOT, "docs", "commands", "*.md")))[:8]:
        out.append((p, 80, WORD))
    for p in (os.path.join(ROOT, "CLAUDE.md"), os.path.join(ROOT, "README.md")):
        if os.path.exists(p):
            out.append((p, 72, WORD))
            out.append((p, 72, OFF))

    # The file this whole change exists for.
    pci = os.path.join(ROOT, "data", "pci.ids")
    if os.path.exists(pci):
        out.append((pci, 80, WORD))
        out.append((pci, 80, OFF))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--positive-control", action="store_true",
                    help="corrupt the wrap index; the checks MUST go red")
    args = ap.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp)
        docs = corpus(tmp)
        argv = [exe]
        if args.positive_control:
            argv.append("--corrupt")
        for path, cols, wrap in docs:
            argv += [path, str(cols), str(wrap)]
        r = subprocess.run(argv, capture_output=True)
        sys.stdout.write(r.stdout.decode("utf-8", errors="replace"))
        sys.stdout.write(r.stderr.decode("utf-8", errors="replace"))

        if args.positive_control:
            if r.returncode == 0:
                print("positive control: FAILED -- a corrupt index changed "
                      "nothing, so this harness is measuring nothing")
                return 1
            print("positive control: OK (the checks went red)")
            return 0
        print("utext_hostcheck: %d documents" % len(docs))
        return r.returncode


if __name__ == "__main__":
    sys.exit(main())
