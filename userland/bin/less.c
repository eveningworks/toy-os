// less -- a pager. Shows one screenful at a time and lets you move
// through it.
//
// TWO THINGS ABOUT THIS ARE NOT OBVIOUS.
//
// IT READS KEYS WITH sys_read_key(), NOT FROM fd 0. That is what makes
// `cmd | less` work at all: in a pipeline fd 0 IS the pipe, so a pager
// that read its keys from stdin would consume the text it is supposed
// to be showing and then block forever waiting for a keystroke that
// arrives on a descriptor it isn't holding. Real less opens /dev/tty
// for exactly this reason; this OS has no /dev/tty, but SYS_READ_KEY
// reaches the keyboard directly and independently of any descriptor,
// which answers the same need.
//
// USE `spawn`, NOT `run`. The legacy `run` loader has no scheduler
// slot, so SYS_SLEEP returns -1 there (see its ABI comment) and the
// key-poll loop below spins hot instead of sleeping between checks --
// which looks exactly like a hung pager. Under `spawn` it idles at
// ~0.01s of CPU. Measured both ways.
//
// IT ASKS THE CONSOLE HOW BIG IT IS (sys_console_size). The console is
// font-derived here -- rows and columns come from the active font, and
// `font_size` is a runtime setting -- so a baked 80x25 would page
// wrongly on any machine whose font was changed. Same rule the GUI's
// whole layout follows.
#include "rt/sys.h"
#include <string.h>
#include <stdio.h>
#include <keyboard.h>  // KEY_* -- specials arrive as these codes

// printf() and putchar() deliberately do not exist here (lib/stdio.h
// explains why: they need a buffered stream layer). A whole screen is
// formatted into one buffer and written with a single sys_write, which
// is what a pager wants anyway -- one syscall per frame rather than one
// per character, and no partial screen if something goes wrong midway.

// The whole input is held in memory so it can be scrolled BACKWARD. A
// pipe cannot be rewound -- once read, a byte is gone -- so a pager
// that supports going back has no choice but to keep what it has seen.
// (This is the difference between `less` and `more`: more is
// forward-only and needs no buffer at all.)
//
// Static rather than malloc'd: it is a fixed ceiling either way, and a
// ring-3 stack has a 2 KiB frame budget so it could not live there.
// Bigger than this is truncated with a message rather than silently cut
// -- a pager that quietly drops the end of a file is worse than one
// that says it did.
#define LESS_MAX_BYTES (256 * 1024)
#define LESS_MAX_LINES 8192

static char g_buf[LESS_MAX_BYTES];
static int  g_len = 0;
static int  g_line_start[LESS_MAX_LINES];  // offset of each line
static int  g_lines = 0;
static int  g_truncated = 0;

static void index_lines(void) {
    g_lines = 0;
    if (g_len == 0) return;
    g_line_start[g_lines++] = 0;
    for (int i = 0; i < g_len && g_lines < LESS_MAX_LINES; i++) {
        if (g_buf[i] == '\n' && i + 1 <= g_len) g_line_start[g_lines++] = i + 1;
    }
    // A trailing newline produces a final empty line index; harmless,
    // and dropping it would make the last real line unreachable at the
    // bottom of the file.
}

static int read_all(int fd) {
    while (g_len < LESS_MAX_BYTES) {
        int64_t n = sys_read(fd, g_buf + g_len, (size_t)(LESS_MAX_BYTES - g_len));
        if (n == SYS_RETRY) continue;   // pipe not ready; ask again
        if (n <= 0) break;              // 0 is EOF on a file or a pipe
        g_len += (int)n;
    }
    if (g_len >= LESS_MAX_BYTES) g_truncated = 1;
    return g_len;
}

// Draws one screenful starting at line `top`, plus a status line.
static char g_frame[LESS_MAX_BYTES / 8];

static void draw(int top, int rows, int cols) {
    // No cursor addressing: the console swallows the escapes it cannot
    // honour (kernel/lib/ansi.c), so repainting means printing a
    // screenful and letting it scroll. Cheap, and correct on a console
    // that has no addressing to offer.
    int page = rows - 1;             // one row reserved for the status line
    int used = 0;
    int cap = (int)sizeof g_frame;

    for (int i = 0; i < page; i++) {
        int ln = top + i;
        if (ln < g_lines) {
            int start = g_line_start[ln];
            int end = (ln + 1 < g_lines) ? g_line_start[ln + 1] : g_len;
            if (end > start && g_buf[end - 1] == '\n') end--;   // drop the newline

            // Clip to the console width rather than letting it wrap:
            // a wrapped line pushes the status row off the bottom and
            // makes the page height silently wrong.
            int n = end - start;
            if (n > cols) n = cols;
            if (used + n + 1 > cap) break;
            for (int k = 0; k < n; k++) g_frame[used++] = g_buf[start + k];
        }
        if (used + 1 > cap) break;
        g_frame[used++] = '\n';
    }

    int last = top + page;
    if (last > g_lines) last = g_lines;
    int pct = g_lines ? (last * 100) / g_lines : 100;

    char status[160];
    snprintf(status, sizeof status,
             "less: lines %d-%d/%d (%d%%)%s  [space/b, arrows, g/G, q]\n",
             g_lines ? top + 1 : 0, last, g_lines, pct,
             g_truncated ? "  TRUNCATED" : "");
    int slen = (int)strlen(status);
    if (used + slen <= cap) {
        for (int k = 0; k < slen; k++) g_frame[used++] = status[k];
    }

    sys_write(1, g_frame, (size_t)used);
}

int main(int argc, char **argv) {
    int fd = 0;   // stdin by default, so `cmd | less` works
    if (argc > 1) {
        fd = sys_open(argv[1], 0)  /* read-only is the default; there is no SYS_O_READ */;
        if (fd < 0) {
            char msg[160];
            snprintf(msg, sizeof msg, "less: cannot open %s: %s\n",
                     argv[1], sys_strerror(sys_errno()));
            sys_eprint(msg);
            return 1;
        }
    }

    read_all(fd);
    if (fd > 0) sys_close(fd);
    index_lines();

    if (g_lines == 0) return 0;   // nothing to page

    int cols = 80;
    int rows = sys_console_size(&cols);
    if (rows < 4) rows = 24;      // a console too small to page in
    if (cols < 8) cols = 80;
    int page = rows - 1;

    int top = 0;
    int last_top = -1;
    for (;;) {
        if (top != last_top) { draw(top, rows, cols); last_top = top; }

        // sys_read_key() is NON-BLOCKING and returns -1 when nothing is
        // waiting, so sleep between polls rather than spinning a core
        // flat. 20ms is well under human reaction time and costs
        // essentially nothing.
        int k = sys_read_key();
        if (k < 0) { sys_sleep_ms(20); continue; }

        int max_top = g_lines - page;
        if (max_top < 0) max_top = 0;

        if (k == 'q' || k == 'Q') break;
        else if (k == ' ' || k == KEY_PAGE_DOWN || k == 'f') top += page;
        else if (k == 'b' || k == KEY_PAGE_UP) top -= page;
        else if (k == KEY_ARROW_DOWN || k == '\n' || k == '\r') top += 1;
        else if (k == KEY_ARROW_UP) top -= 1;
        else if (k == KEY_HOME || k == 'g') top = 0;
        else if (k == KEY_END || k == 'G') top = max_top;
        else continue;   // unknown key: do not repaint

        if (top > max_top) top = max_top;
        if (top < 0) top = 0;
    }
    return 0;
}
