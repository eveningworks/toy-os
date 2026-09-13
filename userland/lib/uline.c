// See uline.h. Extracted from /bin/tosh's redraw() when /bin/dash needed
// the same paint -- the code is that function's, unchanged.
#include "uline.h"
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include "rt/sys.h"   // sys_tcgetwinsz -- the width comes from the kernel

static void put(const char *s) { write(1, s, (size_t)strlen(s)); }

int uline_cols(void) {
    struct tty_winsize ws;
    if (sys_tcgetwinsz(0, &ws) < 0 || ws.cols < 20) return 80;
    return ws.cols;
}

static void cursor_up(int n) {
    if (n <= 0) return;
    char buf[16];
    snprintf(buf, sizeof buf, "\x1b[%dA", n);
    put(buf);
}

void uline_erase(int *row_shown) {
    cursor_up(*row_shown);
    put("\r");
    put("\x1b[J");
    *row_shown = 0;
}

void uline_paint(const char *prompt, const struct kline_edit *ed, int *row_shown) {
    int w = uline_cols();
    const char *p = prompt ? prompt : "";
    int plen  = (int)strlen(p);
    int total = plen + ed->len;
    int cur   = plen + ed->cursor;

    cursor_up(*row_shown);
    put("\r");
    put("\x1b[J");   // this row and every row below it

    int rows = total / w;   // one past the last content row when total
                            // fills its last row exactly -- which is the
                            // row the caret then belongs on
    for (int r = 0; r <= rows; r++) {
        int from = r * w, to = from + w;
        if (to > total) to = total;
        if (from < plen)
            write(1, p + from, (size_t)((to < plen ? to : plen) - from));
        if (to > plen) {
            int a = from > plen ? from : plen;
            write(1, ed->buf + (a - plen), (size_t)(to - a));
        }
        if (r < rows) put("\n");
    }

    int cur_row = cur / w;
    cursor_up(rows - cur_row);
    char col[16];
    snprintf(col, sizeof col, "\x1b[%dG", cur % w + 1);
    put(col);

    *row_shown = cur_row;
}

void uline_end(const char *prompt, struct kline_edit *ed, int *row_shown) {
    int save = ed->cursor;
    ed->cursor = ed->len;
    uline_paint(prompt, ed, row_shown);
    ed->cursor = save;
    put("\n");
    *row_shown = 0;
}

void uline_clear_screen(const char *prompt, struct kline_edit *ed, int *row_shown) {
    // ESC[2J ESC[H -- erase the display, caret to the top -- then put
    // the prompt and the half-typed line back. Both ring-3 front ends
    // reach the same behaviour this way; the kernel shell calls the
    // console directly, which is the split klineedit.h describes.
    put("\x1b[2J\x1b[H");
    *row_shown = 0;
    uline_paint(prompt, ed, row_shown);
}

void uline_insert_last_arg(struct kline_edit *ed, const char *previous) {
    if (!previous || !*previous) return;
    int n = (int)strlen(previous);
    int start = kline_ws_word_start(previous, n, n);
    kline_insert_str(ed, previous + start);
}

void uline_list_candidates(const char *prompt, struct kline_edit *ed, int *row_shown,
                           const char *candidates, int count, int stride) {
    if (count <= 0 || stride <= 0 || !candidates) return;
    uline_end(prompt, ed, row_shown);

    int widest = 0;
    for (int i = 0; i < count; i++) {
        int l = (int)strlen(candidates + (size_t)i * stride);
        if (l > widest) widest = l;
    }
    int colw = widest + 2;
    int cols = uline_cols() / colw;
    if (cols < 1) cols = 1;   // a candidate wider than the window gets a row

    int col = 0;
    for (int i = 0; i < count; i++) {
        const char *c = candidates + (size_t)i * stride;
        put(c);
        if (++col == cols || i + 1 == count) { put("\n"); col = 0; continue; }
        for (int pad = (int)strlen(c); pad < colw; pad++) put(" ");
    }
    uline_paint(prompt, ed, row_shown);
}
