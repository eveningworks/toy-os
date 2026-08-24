// less -- a pager. Shows one screenful at a time and lets you move
// through it.
//
// TWO THINGS ABOUT THIS ARE NOT OBVIOUS.
//
// WHERE ITS KEYS COME FROM IS THE WHOLE DESIGN, and the first answer
// was wrong in a way that only showed up in a window.
//
// It used to read sys_read_key(), on the reasoning that in a pipeline
// fd 0 IS the pipe, so a pager reading keys from stdin would eat the
// text it is meant to display. The reasoning is right and the call is
// not: sys_read_key() is tty_read_key(tty_console()) -- the PHYSICAL
// console, whoever asks. On a text boot that happens to be this
// program's terminal. In a GUI Terminal window it is not: the
// compositor holds the keyboard and this window's input goes to its
// own pty, so `less` there polled an empty console forever and looked
// like a hang. It was reported as `dmesg | less` hanging; the pipe had
// nothing to do with it, and `less <file>` in a window was equally
// dead.
//
// So: THE KEY SOURCE IS THE FIRST OF fd 0 AND fd 1 THAT IS A TERMINAL.
// With a file argument, fd 0 is the terminal and the file is the
// content. In `cmd | less`, fd 0 is the pipe and fd 1 is the terminal.
// Both land on the right fd without this program knowing which case it
// is in. Unix reaches the same place through /dev/tty, which exists
// because the fd-1 fallback is unreliable THERE -- a process can be
// backgrounded away from its terminal, and stdout can be a file while
// the terminal is still wanted. Here neither applies to a pager: if
// fd 1 is not a terminal the output is going to a file, and a pager
// with nowhere to page has nothing to ask about.
//
// **WITH NO TERMINAL IT DUMPS RATHER THAN REFUSES.** `cmd | less >
// out.txt` prints everything and exits, which is what `cat` would have
// done and what the caller plainly wanted; blocking there for a
// keypress that can never come would be the same hang from the other
// direction.
//
// The fd is put into RAW mode and put back before this returns -- a
// pager that left the terminal raw would hand the shell a prompt with
// no echo, which looks exactly like a hung machine. `edit` does the
// same, for the same reason.
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

    // THE KEY SOURCE, decided once -- see the top of this file. fd 2 is
    // deliberately not a candidate: it is the KERNEL LOG here, not a
    // second terminal stream.
    int key_fd = sys_isatty(0) ? 0 : (sys_isatty(1) ? 1 : -1);

    if (key_fd < 0) {
        // Nowhere to page TO. Write what was read, verbatim and in one
        // go -- the buffer already holds the text with its newlines, so
        // this is `cat`, which is exactly what the caller asked for by
        // sending the output somewhere that is not a screen.
        int off = 0;
        while (off < g_len) {
            int64_t n = sys_write(1, g_buf + off, (size_t)(g_len - off));
            if (n <= 0) break;
            off += (int)n;
        }
        return 0;
    }

    struct tty_termios saved;
    int restore = sys_tcgetattr(key_fd, &saved) == 0;
    if (restore) sys_tty_raw(key_fd);

    int cols = 80;
    int rows = sys_console_size(&cols);
    if (rows < 4) rows = 24;      // a console too small to page in
    if (cols < 8) cols = 80;
    int page = rows - 1;

    int top = 0;
    int last_top = -1;
    for (;;) {
        if (top != last_top) { draw(top, rows, cols); last_top = top; }

        // A BLOCKING read of one byte, which is what a pager wants and
        // what the old poll-and-sleep could not be: sys_read_key() had
        // no terminal to block on, so it returned -1 forever and the
        // 20ms sleep was the only thing keeping it off a core.
        //
        // Specials arrive as 0x91-0xA6, which ARE the KEY_* codes below
        // (api/keyboard.h) -- a byte off a terminal needs no
        // translation layer here, the same contract the shared line
        // editor relies on.
        unsigned char ch;
        int64_t n = sys_read(key_fd, &ch, 1);
        if (n <= 0) break;   // the terminal went away -- do not spin on it
        int k = ch;

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

    // PUT BACK on every way out, including the terminal disappearing --
    // one place rather than one per break, because the way this goes
    // wrong is a path somebody adds later that forgets.
    if (restore) sys_tcsetattr(key_fd, &saved);
    return 0;
}
