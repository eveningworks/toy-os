// The pager, extracted from /bin/less when /bin/doc wanted the same
// one. See upager.h for the seam; this file is the machinery.
//
// THREE THINGS HERE ARE NOT OBVIOUS, and two of them were paid for by
// less.c before this file existed.
//
// WHERE THE KEYS COME FROM IS THE WHOLE DESIGN. Reading them from fd 0
// is wrong in a pipeline (that fd is the text) and reading the physical
// console is wrong in a window (the compositor holds the keyboard and
// this window's input goes to its own pty, so the pager polled an empty
// console forever and looked like a hang). The rule that is right
// everywhere: THE KEY SOURCE IS THE FIRST OF fd 0 AND fd 1 THAT IS A
// TERMINAL. With a file, fd 0 is the terminal; in `cmd | pager`, fd 1
// is. Unix reaches the same place through /dev/tty, which exists
// because the fd-1 fallback is unreliable THERE -- a process can be
// backgrounded away from its terminal. Neither case applies to a pager
// here: if fd 1 is not a terminal the output is going to a file.
//
// **WIDTH IS MEASURED IN DISPLAY COLUMNS, NOT BYTES.** A line is
// clipped to the terminal rather than wrapped, because a wrapped line
// pushes the status row off the bottom and makes the page height
// silently wrong -- but an ANSI escape occupies zero columns and this
// used to count its bytes, so `ls --color=always | less` lost the tail
// of every coloured line. doc emits styled text by default, which is
// what made a byte count untenable rather than merely wrong.
//
// The terminal is put into RAW mode and put back before this returns --
// a pager that left it raw hands the shell a prompt with no echo, which
// looks exactly like a hung machine. `edit` does the same.
#include "lib/upager.h"
#include "termkey.h"   // a terminal sends sequences, not private bytes
#include "rt/sys.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <keyboard.h>  // KEY_* -- specials arrive as these codes
#include <unistd.h>
#include "signal_abi.h" // SIGWINCH -- see g_resized

// One escape sequence, or any short literal, to stdout.
static void put(const char *s) { write(1, s, strlen(s)); }

// An ANSI escape at `s` (`n` bytes left): its length, or 0 if there is
// no escape here. Only CSI is recognised, which is all anything on this
// system emits -- an OSC would need its own terminator scan and nothing
// writes one into paged text.
static int esc_len(const char *s, int n) {
    if (n < 2 || s[0] != '\x1b' || s[1] != '[') return 0;
    int i = 2;
    while (i < n && (unsigned char)s[i] >= 0x20 && (unsigned char)s[i] <= 0x3f) i++;
    if (i < n) i++;   // the final byte
    return i;
}

int upager_width(const char *s, int n) {
    int w = 0;
    for (int i = 0; i < n; ) {
        int e = esc_len(s + i, n - i);
        if (e) { i += e; continue; }
        w++;
        i++;
    }
    return w;
}

int upager_clip(const char *s, int n, int cols) {
    int w = 0;
    int i = 0;
    while (i < n) {
        int e = esc_len(s + i, n - i);
        if (e) { i += e; continue; }   // zero width: never the cut point
        if (w == cols) break;
        w++;
        i++;
    }
    return i;
}

const char *upager_keys(void) {
    return "  space, f, PageDown   forward one page\n"
           "  b, PageUp            back one page\n"
           "  down, Enter          forward one line\n"
           "  up                   back one line\n"
           "  g, Home              first page\n"
           "  G, End               last page\n"
           "  q                    quit\n";
}

// Everything one run needs, so the draw helper takes one argument
// instead of eight and nothing here is a file-scope global -- a pager
// that kept its state in .bss would keep it in every process that links
// libuapp.so, whether or not it ever pages anything.
struct pager {
    const char *text;
    int         len;
    const int  *line;      // byte offset of each line
    int         lines;
    char       *frame;
    int         frame_cap;
    const char *label;
    int         truncated;
    int         rows, cols;
};

// Draws one screenful starting at line `top`, plus a status line.
//
// HOME, THEN PAINT, THEN ERASE WHAT IS LEFT -- a page REPLACES the one
// before it. ESC[J at the END rather than ESC[2J at the start: erasing
// first would blank the screen and then paint it, which flickers.
static void draw(const struct pager *p, int top) {
    put("\x1b[H");
    int page = p->rows - 1;          // one row reserved for the status line
    int used = 0;
    int cap = p->frame_cap;

    for (int i = 0; i < page; i++) {
        int ln = top + i;
        if (ln < p->lines) {
            int start = p->line[ln];
            int end = (ln + 1 < p->lines) ? p->line[ln + 1] : p->len;
            if (end > start && p->text[end - 1] == '\n') end--;   // drop the newline

            int n = upager_clip(p->text + start, end - start, p->cols);
            if (used + n + 4 > cap) break;
            for (int k = 0; k < n; k++) p->frame[used++] = p->text[start + k];
        }
        // **ERASE TO END OF LINE BEFORE THE NEWLINE.** A page paints
        // over the one before it, and a line SHORTER than the one it
        // replaces would otherwise leave that line's tail on screen --
        // text from another part of the file, sitting past the end of
        // the current one. It is also why two draws of the same page
        // did not match: the leftovers depend on what was there before.
        if (used + 4 > cap) break;
        p->frame[used++] = '\x1b';
        p->frame[used++] = '[';
        p->frame[used++] = 'K';
        p->frame[used++] = '\n';
    }

    int last = top + page;
    if (last > p->lines) last = p->lines;
    int pct = p->lines ? (last * 100) / p->lines : 100;

    // THE KEYS ARE ON SCREEN, spelled rather than abbreviated. A pager
    // is the one program where "how do I get out of this" is a real
    // question a person has, and `less` on a real system is famous for
    // not answering it.
    //
    // **NO TRAILING NEWLINE.** The status line is the LAST row, so a
    // newline after it scrolls the terminal by one -- which shifted
    // every repaint up a row and let the top of the text creep off the
    // screen. It read as "it will not go all the way down".
    char status[200];
    snprintf(status, sizeof status,
             "%s: %d-%d/%d (%d%%)%s  "
             "space/b page  up/down line  g/G ends  q quit",
             p->label, p->lines ? top + 1 : 0, last, p->lines, pct,
             p->truncated ? "  TRUNCATED" : "");

    // A SOLID BAND, not a run of text. ESC[7m swaps foreground and
    // background, and the line is PADDED WITH SPACES to the full width
    // so the inversion covers the whole row -- a bar that stops where
    // its text does reads as a label rather than as chrome. `edit`'s
    // status bar is the same construction, which is the point: two
    // full-screen programs here should not each invent one.
    //
    // Reverse video rather than a hardcoded grey: inverting uses
    // whatever colours are in effect, so the bar stays legible if the
    // console's theme changes.
    int slen = (int)strlen(status);
    if (slen > p->cols) slen = p->cols;    // never wrap: that costs a row
    if (used + slen + 12 <= cap) {
        const char *on = "\x1b[7m", *off = "\x1b[0m";
        for (const char *e = on; *e; e++) p->frame[used++] = *e;
        for (int k = 0; k < slen; k++) p->frame[used++] = status[k];
        for (int k = slen; k < p->cols && used < cap - 6; k++) p->frame[used++] = ' ';
        for (const char *e = off; *e; e++) p->frame[used++] = *e;
    }

    write(1, p->frame, (size_t)used);
    put("\x1b[J");
}

// Write it all through, for a caller with nowhere to page to. This is
// `cat`, which is exactly what was asked for by sending the output
// somewhere that is not a screen.
static int dump(const char *text, int len) {
    int off = 0;
    while (off < len) {
        int64_t n = write(1, text + off, (size_t)(len - off));
        if (n <= 0) return -1;
        off += (int)n;
    }
    return 0;
}

// THE SIZE COMES FROM THE TERMINAL THIS IS ON, not from the console.
// sys_console_size() reports the PHYSICAL console -- so in a Terminal
// window a pager sized its page to a screen it was not drawing on: too
// many rows for the window and the wrong width. It looked like three
// separate bugs and was one wrong question.
//
// Asked of the KEY descriptor rather than of fd 1, because that is the
// one already established to be a terminal.
int upager_term(int *rows, int *cols) {
    int r = 0, c = 0;
    int fd = sys_isatty(0) ? 0 : (sys_isatty(1) ? 1 : -1);
    struct tty_winsize ws;
    if (fd >= 0 && sys_tcgetwinsz(fd, &ws) == 0 && ws.rows > 1 && ws.cols > 1) {
        r = ws.rows;
        c = ws.cols;
    } else if (fd >= 0) {
        c = 80;
        r = sys_console_size(&c);
    }
    if (r < 4) r = 24;      // too small to page in
    if (c < 8) c = 80;
    if (rows) *rows = r;
    if (cols) *cols = c;
    return fd;
}

// **THE WINDOW CHANGED SIZE.** The handler does nothing but say so --
// the async-signal-safety rule, same as every other handler here: it may
// not print, and it may not touch the page state the loop below is
// halfway through.
//
// Without this the pager asked its terminal for a size ONCE, before the
// loop, and then blocked in a read forever -- so `doc` and `less` in a
// window that was resized went on drawing the old page at the old width
// with no way to make them notice. It re-asks now.
static volatile int g_resized;

static void on_sigwinch(int sig) { (void)sig; g_resized = 1; }

// Byte offset of each line, into `line` (capacity `cap`). Returns how
// many there are. A trailing newline produces a final empty line index;
// harmless, and dropping it would make the last real line unreachable
// at the bottom of the file.
static int index_lines(const char *text, int len, int *line, int cap) {
    int lines = 0;
    line[lines++] = 0;
    for (int i = 0; i < len && lines < cap; i++)
        if (text[i] == '\n' && i + 1 <= len) line[lines++] = i + 1;
    return lines;
}

// How many lines `len` bytes can hold, for the index's capacity.
static int count_lines(const char *text, int len) {
    int nl = 1;
    for (int i = 0; i < len; i++) if (text[i] == '\n' && i + 1 <= len) nl++;
    return nl;
}

int upager_run(const char *text, int len, const char *label, int truncated) {
    return upager_run_src(text, len, label, truncated, 0);
}

int upager_run_src(const char *text, int len, const char *label, int truncated,
                   const struct upager_source *src) {
    if (len <= 0) return 0;
    if (!label) label = "pager";

    int rows, cols;
    int key_fd = upager_term(&rows, &cols);
    struct termkey_state keys = {0};   // the terminal decoder, per stream
    if (key_fd < 0) return dump(text, len);

    // **RENDERED AT THE PAGER'S OWN WIDTH FIRST**, so a caller need not
    // measure the terminal itself and then hope the two agree.
    if (src && src->render) {
        const char *fresh = 0;
        int n = src->render(src->ctx, cols, &fresh);
        if (n > 0 && fresh) { text = fresh; len = n; }
    }

    int nl = count_lines(text, len);
    // Sized from the input and from the real terminal rather than from
    // a ceiling, and freed before this returns. A frame row can be
    // longer in BYTES than in columns (escapes cost bytes and no
    // columns), so the allowance is generous and draw() still stops
    // when the next row would not fit.
    int   *line  = malloc((size_t)nl * sizeof(int));
    int    fcap  = rows * (cols * 4 + 16) + 512;
    char  *frame = malloc((size_t)fcap);
    if (!line || !frame) {
        free(line);
        free(frame);
        return dump(text, len);   // no memory to page with: still deliver it
    }

    int lines = index_lines(text, len, line, nl);
    struct pager p = {
        .text = text, .len = len, .line = line, .lines = lines,
        .frame = frame, .frame_cap = fcap, .label = label,
        .truncated = truncated, .rows = rows, .cols = cols,
    };

    struct tty_termios saved;
    int restore = sys_tcgetattr(key_fd, &saved) == 0;
    if (restore) sys_tty_raw(key_fd);

    // **NO SA_RESTART, AND THAT IS THE POINT.** With it the kernel
    // rewinds the interrupted read and this loop never learns anything
    // happened; the interruption IS the message. `sys_signal()` would
    // set the flag, so the action is installed by hand.
    struct k_sigaction winch = {
        .handler  = (uint64_t)(uintptr_t)on_sigwinch,
        .restorer = (uint64_t)(uintptr_t)__sigrestore,
        .flags    = 0,
    };
    struct k_sigaction winch_saved;
    int had_winch = sys_sigaction(SIGWINCH, &winch, &winch_saved) == 0;

    // THE ALTERNATE SCREEN, so quitting leaves the terminal exactly as
    // it was found. A terminal that does not implement it swallows the
    // sequence, so this is safe everywhere.
    put("\x1b[?1049h");

    int page = rows - 1;
    int top = 0;
    int last_top = -1;
    for (;;) {
        if (g_resized) {
            g_resized = 0;
            // Re-ask, re-size, redraw. The frame buffer is grown when
            // the new page needs more room and KEPT when that fails --
            // draw() stops when the next row would not fit, so a failed
            // realloc costs rows off the bottom rather than a overrun.
            int nr = rows, nc = cols;
            upager_term(&nr, &nc);

            // **THE TEXT IS RE-WRAPPED HERE, or not at all.** The pager
            // re-pages on its own -- new rows, new page, redraw -- but
            // the wrapping belongs to whoever RENDERED the text, at the
            // width the terminal was when they asked. Without this a
            // widened window showed the old narrow paragraphs with the
            // rest of the window empty. A caller with nothing to
            // re-render (less) supplies no source and this is skipped.
            if (src && src->render && nc != cols) {
                const char *fresh = 0;
                int n = src->render(src->ctx, nc, &fresh);
                if (n > 0 && fresh) {
                    // The index is sized to the NEW text: a narrower
                    // width wraps into more lines than the old one had,
                    // and re-indexing into the old array would stop at
                    // its capacity and hide the tail of the page.
                    int nnl = count_lines(fresh, n);
                    int *bigger = (nnl > nl)
                        ? realloc(line, (size_t)nnl * sizeof(int)) : line;
                    if (bigger) {
                        line = p.line = bigger;
                        if (nnl > nl) nl = nnl;
                        p.text = fresh;
                        p.len = n;
                        lines = p.lines = index_lines(fresh, n, line, nl);
                        // WHERE THE READER WAS is a line number in text
                        // that no longer exists. Clamping is all that
                        // can be honestly done -- keeping a byte offset
                        // would need the renderer to map old to new.
                        if (top > lines) top = lines;
                    }
                }
            }

            int need = nr * (nc * 4 + 16) + 512;            if (need > p.frame_cap) {
                char *bigger = realloc(p.frame, (size_t)need);
                if (bigger) { frame = p.frame = bigger; p.frame_cap = need; }
            }
            rows = p.rows = nr;
            cols = p.cols = nc;
            page = rows - 1;
            if (page < 1) page = 1;
            // The bottom moved, so where the reader is may no longer be
            // a legal position -- and `last_top` is reset either way,
            // because the page has to be repainted even when `top` did
            // not move. That cache is why a keypress after a resize was
            // not enough to recover on its own.
            int max_top = lines - page;
            if (max_top < 0) max_top = 0;
            if (top > max_top) top = max_top;
            last_top = -1;
        }
        if (top != last_top) { draw(&p, top); last_top = top; }

        // A BLOCKING read of one byte. A special key arrives as an ANSI
        // sequence (api/termkey.h), reassembled below into the KEY_*
        // the cases switch on.
        unsigned char ch;
        int64_t n = read(key_fd, &ch, 1);
        // INTERRUPTED, NOT BROKEN: a resize lands here as EINTR, and
        // treating it as the terminal going away would close the pager
        // on every window drag.
        if (n < 0 && sys_errno() == EINTR) continue;
        if (n <= 0) break;   // the terminal went away -- do not spin on it
        // DECODED: a special key is an ANSI sequence on a terminal, so
        // reassemble it into the KEY_* the cases below switch on.
        int k = termkey_feed(&keys, ch);
        if (k == TERMKEY_MORE || k == TERMKEY_NONE) continue;

        int max_top = lines - page;
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
    // one place rather than one per break. The screen first, then the
    // mode: leaving raw mode while still on the alternate screen would
    // show the shell's echo on a screen about to be thrown away.
    put("\x1b[?1049l");
    if (had_winch) sys_sigaction(SIGWINCH, &winch_saved, 0);
    if (restore) sys_tcsetattr(key_fd, &saved);
    free(line);
    free(frame);
    return 0;
}
