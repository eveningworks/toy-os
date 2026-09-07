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

So this compiles the SAME .c file with the host gcc, beside a NAIVE
implementation of the same wrap rule written out longhand in the driver
-- the pre-index accounting, scanning from character 0 every time -- and
requires the two to agree:

  - total wrapped lines, at several widths
  - a ROUND TRIP: the point at which character i is drawn hit-tests back
    to i, for a spread of i across the document. This is the check that
    catches draw() and index_at_point() drifting apart, which is the
    classic symptom of two copies of the wrap arithmetic.
  - scroll_top() really shows line 0 and scroll_bottom() the last screen
  - selection text out and pasted text in, including that '\r' is dropped
  - utext_putc REFUSES at capacity rather than dropping the oldest
    character (the bug that made Notepad open pci.ids as its last 8 KB)

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

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

DRIVER = r"""
#include "ui/utext.h"
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

// utext draws through ugfx; none of the wrap accounting depends on what
// a pixel does, so four stubs are the whole port. The two metrics ARE
// load-bearing -- the grid is derived from them.
int ugfx_char_w(void) { return 8; }
int ugfx_char_h(void) { return 16; }
void ugfx_fill_rect(struct ugfx_surface *s, int x, int y, int w, int h, uint32_t c)
{ (void)s; (void)x; (void)y; (void)w; (void)h; (void)c; }
void ugfx_draw_char(struct ugfx_surface *s, int x, int y, char ch, uint32_t f, uint32_t b)
{ (void)s; (void)x; (void)y; (void)ch; (void)f; (void)b; }

#define CW 8
#define CH 16

// --- the oracle -------------------------------------------------------
//
// The wrap rule written out longhand, scanning from index 0 -- what
// utext did before the index existed. It shares no code with the
// implementation under test, which is the only reason agreeing means
// anything.

static void naive(const struct utext *t, int cols, int target,
                  int *out_total, int *out_line, int *out_col) {
    int line = 0, col = 0, cl = 0, cc = 0, got = 0;
    for (int i = 0; i <= t->count; i++) {
        if (i == target && !got) { cl = line; cc = col; got = 1; }
        if (i == t->count) break;
        char c = t->buf[i];
        if (c == '\n') { line++; col = 0; continue; }
        if (col >= cols) { line++; col = 0; }
        col++;
    }
    if (out_total) *out_total = line + 1;
    if (out_line) *out_line = cl;
    if (out_col) *out_col = cc;
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

// The visible box for a given column count and row count.
#define ROWS 24

// The oracle's answer to "where does wrapped line `line` begin?", in ONE
// pass. The obvious shape -- call naive() per index until its line
// matches -- is O(n^2), which on pci.ids is a harness that never
// finishes rather than a check that fails.
static int naive_line_begin(const struct utext *t, int cols, int want) {
    if (want <= 0) return 0;
    int line = 0, col = 0;
    for (int i = 0; i < t->count; i++) {
        char c = t->buf[i];
        if (c == '\n') { line++; col = 0; if (line == want) return i + 1; continue; }
        if (col >= cols) { line++; col = 0; if (line == want) return i; }
        col++;
    }
    return t->count;
}

// Every character on the screen shown at `scroll`, round-tripped: the
// point at which the oracle says character i is drawn must hit-test
// back to i. Walking the visible rows only, from the oracle's own
// starting index, is what keeps this O(n) per scroll position.
static void check_screen(struct utext *t, int cols, int w, int h,
                          int total, int scroll, const char *where) {
    t->scroll_offset = scroll;
    utext_metrics(t, w, h, NULL, NULL);   // clamps, as the widget would

    int first = total - ROWS - t->scroll_offset;
    if (first < 0) first = 0;
    int i = naive_line_begin(t, cols, first);

    int line = first, col = 0;
    for (; i < t->count; i++) {
        char c = t->buf[i];
        if (c == '\n') {
            line++; col = 0;
            if (line - first >= ROWS) break;
            continue;
        }
        if (col >= cols) {
            line++; col = 0;
            if (line - first >= ROWS) break;
        }
        int px = col * CW + CW / 2, py = (line - first) * CH + CH / 2;
        int got = utext_index_at_point(t, 0, 0, w, h, px, py);
        if (got != i) {
            printf("FAIL round trip at %s (line %d col %d): %d != %d\n",
                    where, line, col, got, i);
            fails++;
            if (fails > 8) return;
        }
        col++;
    }
}

static void check_doc(const char *path, int cols, int samples, int corrupt) {
    (void)samples;
    load(path);
    int cap = doclen + 4096;
    char *buf = malloc((size_t)cap);
    struct utext t;
    utext_init_buf(&t, buf, cap);
    xcpy(buf, doc, doclen);
    t.count = doclen;
    t.rev++;

    int w = cols * CW, h = ROWS * CH;

    int total = 0, vis = 0;
    utext_metrics(&t, w, h, &total, &vis);

    int want_total;
    naive(&t, cols, 0, &want_total, NULL, NULL);
    if (total != want_total) fail("total_lines", total, want_total);
    if (vis != ROWS) fail("visible_rows", vis, ROWS);
    total = want_total;

    if (corrupt) {
        // A single checkpoint moved by three characters. Every lookup
        // that starts from it lands in the wrong place -- which is
        // exactly what a wrong index does, with nothing else about the
        // text disturbed.
        if (t.wrap.n > 1) t.wrap.idx[1] += 3;
        else { printf("FAIL corpus too small to corrupt: %s\n", path); fails++; }
    }

    // The two ends and three places in between, so a lookup that starts
    // from a checkpoint DEEP in the document is exercised rather than
    // just the first screen.
    int max_scroll = total > ROWS ? total - ROWS : 0;
    check_screen(&t, cols, w, h, total, max_scroll, "top");
    check_screen(&t, cols, w, h, total, max_scroll * 3 / 4, "3/4");
    check_screen(&t, cols, w, h, total, max_scroll / 2, "middle");
    check_screen(&t, cols, w, h, total, max_scroll / 4, "1/4");
    check_screen(&t, cols, w, h, total, 0, "bottom");

    // And the two named ends really are the ends.
    utext_scroll_top(&t);
    utext_metrics(&t, w, h, NULL, NULL);
    if (t.scroll_offset != max_scroll) fail("scroll_top offset", t.scroll_offset, max_scroll);
    utext_scroll_bottom(&t);
    utext_metrics(&t, w, h, NULL, NULL);
    if (t.scroll_offset != 0) fail("scroll_bottom offset", t.scroll_offset, 0);

    free(buf);
    free(doc);
    doc = NULL;
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

    // select-all spans the document whatever the caret was doing.
    utext_init_buf(&t, b, (int)sizeof b);
    utext_insert_text(&t, "one\ntwo", 7);
    t.ed.cursor = 2;
    utext_sel_all(&t);
    int a, e;
    utext_sel_range(&t, &a, &e);
    if (a != 0 || e != 7) fail("sel_all range", e - a, 7);
}

int main(int argc, char **argv) {
    int corrupt = argc > 1 && xeq(argv[1], "--corrupt");
    int first = corrupt ? 2 : 1;
    if (!corrupt) check_semantics();
    for (int i = first; i < argc; i += 2) {
        int cols = atoi(argv[i + 1]);
        check_doc(argv[i], cols, 300, corrupt);
    }
    printf("%s (%d failures)\n", fails ? "RED" : "GREEN", fails);
    return fails ? 1 : 0;
}
"""


def build(tmp):
    src = os.path.join(tmp, "utext_host.c")
    with open(src, "w") as fh:
        fh.write(DRIVER)
    exe = os.path.join(tmp, "utext_host")
    subprocess.run(
        ["gcc", "-O2", "-Wall", "-Wextra", "-Werror", "-o", exe, src,
         os.path.join(ROOT, "userland", "ui", "utext.c"),
         os.path.join(ROOT, "userland", "ui", "uui_edit.c"),
         "-I" + os.path.join(ROOT, "userland"),
         "-I" + os.path.join(ROOT, "kernel", "include", "api"),
         "-I" + os.path.join(ROOT, "kernel", "include", "abi")],
        check=True)
    return exe


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
    out.append((long_path, 40))
    out.append((long_path, 200))

    # One paragraph with NO newline at all: every line break is a wrap,
    # which is the opposite corner from the file above.
    wrap_path = os.path.join(tmp, "onelong.txt")
    with open(wrap_path, "w") as fh:
        fh.write("x" * 60000)
    out.append((wrap_path, 37))

    for p in sorted(glob.glob(os.path.join(ROOT, "docs", "commands", "*.md")))[:12]:
        out.append((p, 80))
    for p in (os.path.join(ROOT, "CLAUDE.md"), os.path.join(ROOT, "README.md")):
        if os.path.exists(p):
            out.append((p, 72))

    # The file this whole change exists for.
    pci = os.path.join(ROOT, "data", "pci.ids")
    if os.path.exists(pci):
        out.append((pci, 80))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--positive-control", action="store_true",
                    help="corrupt a checkpoint; the checks MUST go red")
    args = ap.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp)
        docs = corpus(tmp)
        argv = [exe]
        if args.positive_control:
            argv.append("--corrupt")
        for path, cols in docs:
            argv += [path, str(cols)]
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
