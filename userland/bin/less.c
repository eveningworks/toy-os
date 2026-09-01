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
#include <fcntl.h>
#include <unistd.h>

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
        int64_t n = read(fd, g_buf + g_len, (size_t)(LESS_MAX_BYTES - g_len));
        if (n == SYS_RETRY) continue;   // pipe not ready; ask again
        if (n <= 0) break;              // 0 is EOF on a file or a pipe
        g_len += (int)n;
    }
    if (g_len >= LESS_MAX_BYTES) g_truncated = 1;
    return g_len;
}

// One escape sequence, or any short literal, to stdout.
static void put(const char *s) { write(1, s, strlen(s)); }

// Draws one screenful starting at line `top`, plus a status line.
static char g_frame[LESS_MAX_BYTES / 8];

static void draw(int top, int rows, int cols) {
    // HOME, THEN PAINT, THEN ERASE WHAT IS LEFT -- a page REPLACES the
    // one before it.
    //
    // This used to print a screenful and let it scroll, on the stated
    // reasoning that "the console swallows the escapes it cannot
    // honour". That was a guess and it was wrong: kernel/lib/ansi.c
    // honours cursor addressing and erase, on the console and in a
    // window alike -- /bin/edit has drawn whole screens through the
    // same parser all along. The cost of the guess was that every page
    // was APPENDED below the last, so pressing space added text rather
    // than turning a page, and the file crept upward off the top.
    //
    // ESC[J at the END rather than ESC[2J at the start: erasing first
    // would blank the screen and then paint it, which flickers. Erasing
    // from wherever the paint finished clears exactly the rows a
    // shorter page left behind, and touches nothing that is about to be
    // overwritten anyway.
    put("\x1b[H");
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
        // **ERASE TO END OF LINE BEFORE THE NEWLINE.** A page paints
        // over the one before it, and a line SHORTER than the one it
        // replaces would otherwise leave that line's tail on screen --
        // text from another part of the file, sitting past the end of
        // the current one. It reads as lines that are not full, and it
        // is why two draws of the same page did not match: the
        // leftovers depend on what was there before.
        //
        // Per line rather than one ESC[2J at the top, for the reason
        // the erase at the bottom gives: blanking the whole screen and
        // repainting it flickers, and this touches only what is not
        // about to be overwritten anyway.
        if (used + 4 > cap) break;
        g_frame[used++] = '\x1b';
        g_frame[used++] = '[';
        g_frame[used++] = 'K';
        g_frame[used++] = '\n';
    }

    int last = top + page;
    if (last > g_lines) last = g_lines;
    int pct = g_lines ? (last * 100) / g_lines : 100;

    // THE KEYS ARE ON SCREEN, spelled rather than abbreviated. A pager
    // is the one program where "how do I get out of this" is a real
    // question a person has, and `less` on a real system is famous for
    // not answering it. The line is trimmed to the terminal's width by
    // the caller's `cols`, so a narrow window loses the tail of the
    // legend rather than wrapping and pushing the page up a row.
    char status[200];
    snprintf(status, sizeof status,
             // **NO TRAILING NEWLINE**, and that is not tidiness. The
             // status line is the LAST row, so a newline after it
             // scrolls the terminal by one -- which shifted every
             // repaint up a row, made two draws of the same page
             // differ, and let the top of the file creep off the
             // screen. It read as "it will not go all the way down".
             // The caret is left at the end of this line, which is
             // where a pager's caret belongs anyway.
             "less: %d-%d/%d (%d%%)%s  "
             "space/b page  up/down line  g/G ends  q quit",
             g_lines ? top + 1 : 0, last, g_lines, pct,
             g_truncated ? "  TRUNCATED" : "");
    // A SOLID BAND, not a run of text. ESC[7m swaps foreground and
    // background, and the line is PADDED WITH SPACES to the full width
    // so the inversion covers the whole row -- a bar that stops where
    // its text does reads as unfinished, and as a label rather than as
    // chrome. `edit`'s status bar is the same construction, which is
    // the point: two full-screen programs on this system should not
    // each invent their own idea of a status line.
    //
    // REVERSE VIDEO RATHER THAN A HARDCODED GREY (ESC[47m). Inverting
    // uses whatever colours are in effect, so the bar stays legible if
    // the console's theme ever changes; a fixed pair would not, which is
    // the same argument docs/gui-guidelines.md makes against picking
    // tints by hand.
    int slen = (int)strlen(status);
    if (slen > cols) slen = cols;          // never wrap: that costs a row
    if (used + slen + 12 <= cap) {
        const char *on = "\x1b[7m", *off = "\x1b[0m";
        for (const char *e = on; *e; e++) g_frame[used++] = *e;
        for (int k = 0; k < slen; k++) g_frame[used++] = status[k];
        // Pad to the full width INSIDE the inversion, so the band runs
        // to the right edge.
        for (int k = slen; k < cols && used < cap - 6; k++) g_frame[used++] = ' ';
        for (const char *e = off; *e; e++) g_frame[used++] = *e;
    }

    write(1, g_frame, (size_t)used);
    // Whatever the previous page left below this one. A page shorter
    // than the last (the end of the file) would otherwise show the
    // tail of the old one under it, which reads as text that will not
    // scroll away.
    put("\x1b[J");
}

static void usage(void) {
    // The same keys the status line shows, in the same order, because
    // two lists of one keymap drift and the one on screen is the one
    // people actually read.
    put("usage: less [file]\n"
        "  With no file, reads stdin -- `dmesg | less`.\n"
        "\n"
        "  space, f, PageDown   forward one page\n"
        "  b, PageUp            back one page\n"
        "  down, Enter          forward one line\n"
        "  up                   back one line\n"
        "  g, Home              first page\n"
        "  G, End               last page\n"
        "  q                    quit\n");
}

int main(int argc, char **argv) {
    int fd = 0;   // stdin by default, so `cmd | less` works
    if (argc > 1 && (strcmp(argv[1], "--help") == 0
                     || strcmp(argv[1], "-h") == 0)) {
        usage();
        return 0;
    }
    if (argc > 1) {
        fd = open(argv[1], O_RDONLY);
        if (fd < 0) {
            char msg[160];
            snprintf(msg, sizeof msg, "less: cannot open %s: %s\n",
                     argv[1], sys_strerror(sys_errno()));
            sys_eprint(msg);
            return 1;
        }
    }

    read_all(fd);
    if (fd > 0) close(fd);
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
            int64_t n = write(1, g_buf + off, (size_t)(g_len - off));
            if (n <= 0) break;
            off += (int)n;
        }
        return 0;
    }

    struct tty_termios saved;
    int restore = sys_tcgetattr(key_fd, &saved) == 0;
    if (restore) sys_tty_raw(key_fd);

    // THE ALTERNATE SCREEN, so quitting leaves the terminal exactly as
    // it was found -- the shell's prompt and whatever was above it,
    // untouched, with no page left behind and no prompt drawn on top of
    // a status bar. It is what every pager and editor on a real
    // terminal does, and the reason `less` does not litter a session.
    //
    // A terminal that does not implement it swallows the sequence, so
    // this is safe everywhere: the physical console ignores both, and
    // the only thing lost there is the restore.
    put("\x1b[?1049h");

    // THE SIZE COMES FROM THE TERMINAL THIS IS ON, not from the
    // console, and that distinction is the same one the key source
    // above turns on. sys_console_size() reports the PHYSICAL console
    // -- so in a Terminal window a pager sized its page to a screen it
    // was not drawing on: too many rows for the window (the tail fell
    // off the bottom and a page step jumped a screen and a half) and
    // the wrong width (lines cut in the wrong column). It looked like
    // three separate bugs and was one wrong question.
    //
    // Asked of the KEY descriptor rather than of fd 1, because that is
    // the one already established to be a terminal -- in `cmd | less`
    // fd 1 is the terminal and fd 0 is a pipe, and asking a pipe for a
    // window size gets a refusal, not a size. `edit` sizes itself the
    // same way.
    struct tty_winsize ws;
    int rows = 0, cols = 0;
    if (sys_tcgetwinsz(key_fd, &ws) == 0 && ws.rows > 1 && ws.cols > 1) {
        rows = ws.rows;
        cols = ws.cols;
    } else {
        // No terminal answer: fall back to the console's idea, which is
        // right on a text boot and is the best guess anywhere else.
        cols = 80;
        rows = sys_console_size(&cols);
    }
    if (rows < 4) rows = 24;      // too small to page in
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
        int64_t n = read(key_fd, &ch, 1);
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
    // wrong is a path somebody adds later that forgets. The screen
    // first, then the mode: leaving raw mode while still on the
    // alternate screen would show the shell's echo on a screen that is
    // about to be thrown away.
    put("\x1b[?1049l");
    if (restore) sys_tcsetattr(key_fd, &saved);
    return 0;
}
