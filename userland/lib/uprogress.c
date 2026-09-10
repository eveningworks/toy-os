// The transfer meter described in uprogress.h.
//
// INTEGER ONLY -- there is no floating point in ring 3 (-mno-sse), so
// the percentage and the rate are computed by multiplying before
// dividing. The other order truncates to zero for everything small.
#include "uprogress.h"
#include "human.h"

#include "rt/sys.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>

#define REDRAW_NS   250000000ull   // four times a second
#define WIDTH_MIN   20
#define WIDTH_MAX   200

// "9s", "1m02s", "1h02m" -- two units at most, because a third is
// noise at every scale a transfer runs at.
static void fmt_time(char *out, unsigned long cap, unsigned long long secs) {
    if (secs < 60)        snprintf(out, cap, "%llus", secs);
    else if (secs < 3600) snprintf(out, cap, "%llum%02llus", secs / 60, secs % 60);
    else                  snprintf(out, cap, "%lluh%02llum", secs / 3600, (secs / 60) % 60);
}

static int term_width(int fd) {
    struct winsize ws;
    if (tcgetwinsize(fd, &ws) == 0 && ws.ws_col >= WIDTH_MIN)
        return ws.ws_col < WIDTH_MAX ? ws.ws_col : WIDTH_MAX;
    return 80;
}

static void draw(struct uprogress *p, int final) {
    unsigned long long now = sys_monotonic_ns();
    unsigned long long ms = (now - p->start_ns) / 1000000ull;
    if (!ms) ms = 1;
    unsigned long long rate = p->done * 1000ull / ms;   // bytes per second

    char got[16], persec[16], when[16];
    human_size(got, sizeof got, p->done);
    human_size(persec, sizeof persec, rate);

    // The right-hand text is built first; the bar gets whatever columns
    // are left, so a narrow terminal loses the bar rather than wrapping
    // -- a wrapped line cannot be repainted by a `\r`.
    char tail[96];
    int pct = 0;
    if (p->total) {
        char all[16];
        human_size(all, sizeof all, p->total);
        pct = (int)(p->done * 100ull / p->total);
        if (final || rate == 0) {
            fmt_time(when, sizeof when, ms / 1000ull);
            snprintf(tail, sizeof tail, " %3d%% %s/%s %s/s in %s",
                     pct, got, all, persec, when);
        } else {
            unsigned long long left = p->total > p->done ? p->total - p->done : 0;
            fmt_time(when, sizeof when, left / rate);
            snprintf(tail, sizeof tail, " %3d%% %s/%s %s/s eta %s",
                     pct, got, all, persec, when);
        }
    } else {
        fmt_time(when, sizeof when, ms / 1000ull);
        snprintf(tail, sizeof tail, " %s %s/s in %s", got, persec, when);
    }

    char line[WIDTH_MAX + 8];
    int cols = term_width(p->fd);
    int len = 0;
    line[len++] = '\r';

    int bar = cols - 1 - (int)strlen(tail);
    if (p->total && bar >= 8) {
        int fill = bar - 2;                         // the two brackets
        int on = (int)((unsigned long long)fill * (unsigned long long)pct / 100ull);
        line[len++] = '[';
        for (int i = 0; i < fill; i++) {
            if (i < on - 1)       line[len++] = '=';
            else if (i == on - 1) line[len++] = '>';
            else                  line[len++] = ' ';
        }
        line[len++] = ']';
    }
    len += snprintf(line + len, sizeof line - (unsigned)len, "%s", tail);

    // Cover whatever the previous, longer line left behind. Without
    // this a shrinking field (100% after 99%, or a rate losing a digit)
    // leaves its last character on screen forever.
    while (len < p->width && len < (int)sizeof line - 2) line[len++] = ' ';
    p->width = len;
    if (final) line[len++] = '\n';

    (void)!write(p->fd, line, (unsigned)len);
}

void uprogress_begin(struct uprogress *p, int fd, unsigned long long total) {
    memset(p, 0, sizeof *p);
    p->fd = isatty(fd) ? fd : -1;
    p->total = total;
    p->start_ns = p->last_ns = sys_monotonic_ns();
}

void uprogress_add(struct uprogress *p, unsigned long long n) {
    p->done += n;
    if (p->fd < 0) return;
    unsigned long long now = sys_monotonic_ns();
    if (now - p->last_ns < REDRAW_NS) return;
    p->last_ns = now;
    draw(p, 0);
}

void uprogress_end(struct uprogress *p) {
    if (p->fd < 0 || !p->start_ns) return;   // never begun, or disabled
    draw(p, 1);
    p->fd = -1;                 // a second call must not print again
}
