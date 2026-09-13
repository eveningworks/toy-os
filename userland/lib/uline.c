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
