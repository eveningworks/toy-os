// edit -- the full-screen text editor, as a RING-3 PROGRAM.
//
// It was `apps/editor.c`: kernel-side, drawing with `vga_putc()`,
// reading with `keyboard_getchar()`, and reached through a shell
// builtin. Nothing about editing a file needs ring 0, and everything it
// used to need from there now exists as a terminal:
//
//     vga_clear(), vga_set_color()  ->  ANSI escapes on fd 1
//     vga_rows(), vga_cols()        ->  SYS_TCGETWINSZ
//     keyboard_getchar()            ->  a raw fd 0
//
// **IT TALKS TO WHATEVER TERMINAL IT WAS GIVEN**, rather than to the
// framebuffer -- which is the whole point of moving it. The kernel
// editor drew with `vga_putc()`, so it was the physical console or
// nothing.
//
// A full-screen program ADDRESSES its screen ("caret to row 4, column
// 12"), so it needs a terminal with a GRID: the physical console has
// one, and so does the GUI Terminal since it grew one (with the
// alternate screen, so quitting restores the prompt).
//
// **THE TEXT MODEL IS `utext`, SHARED WITH NOTEPAD.** Not a third one:
// `utext.h` says outright that Notepad hand-wrote sixty lines of key
// handling before the shared edit core existed and "a second editor
// would have written them again, differently". So Ctrl+A, Shift+arrows,
// Backspace-over-a-selection and typing-replaces-selection mean the same
// thing here as in Notepad, because they ARE the same code. What is
// local to this file is the rendering, which is the only part a terminal
// makes different.
//
// WHAT IT DELIBERATELY DOES NOT DO: no syntax highlighting, no search,
// no undo beyond what the shared core has, and no second buffer. It is
// `nano` at the size this OS actually needs one.
#include <stdint.h>
#include "termkey.h"   // fd 0 is a terminal: specials arrive as sequences
#include "rt/sys.h"
#include "ui/utext.h"
#include "keyboard.h"
#include "lib/cmd.h"
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

// Static, not local: the document buffer and the I/O buffer are 8 KiB
// each, against USERLAND_CFLAGS' -Wframe-larger-than=2048 and a 16 KiB
// ring-3 stack with ONE guard page below it.
//
// A FIXED 8 KiB is this editor's deliberate ceiling. `edit` is the
// console fallback -- it draws with escape sequences and repaints the
// screen per keystroke -- so a file it cannot show is a file it should
// not open; the GUI Notepad is the one that sizes its buffer to the
// file (utext.h).
static struct utext g_tb;
static char g_doc[UTEXT_CAP];
static char g_io[UTEXT_CAP + 1];

static int g_rows = 25, g_cols = 80;

static void put(const char *s) { write(1, s, strlen(s)); }

// --- the terminal ----------------------------------------------------
//
// COLOUR AND CURSOR MOVEMENT ARE ESCAPE SEQUENCES, NOT SYSCALLS -- the
// console parses them (kernel/lib/ansi.c) and so does any other terminal
// worth the name. That is what lets one binary draw on the physical
// console and inside a window.

static void ansi(const char *seq) { put(seq); }

static void ansi_at(int row, int col) {
    char b[24];
    snprintf(b, sizeof b, "\x1b[%u;%uH", (unsigned)row + 1, (unsigned)col + 1);
    put(b);
}

static void size_up(void) {
    struct tty_winsize ws;
    if (sys_tcgetwinsz(0, &ws) == 0 && ws.rows > 1 && ws.cols > 1) {
        g_rows = ws.rows;
        g_cols = ws.cols;
    }
    // ...and a sane floor if fd 0 is not a terminal at all (a pipe, a
    // test harness). Drawing into nothing is better than dividing by it.
    if (g_rows < 3) g_rows = 3;
    if (g_cols < 8) g_cols = 8;
}

// --- rendering --------------------------------------------------------
//
// Wrapping is done here rather than through utext_metrics(), which
// measures in PIXELS because its other callers draw with a font. A
// terminal is a grid, so the arithmetic is columns and it is eight
// lines. Sharing the pixel version would have meant teaching it about a
// second coordinate system to save nothing.

// Which wrapped line index `pos` falls on, and the column within it.
static void locate(int pos, int *out_line, int *out_col) {
    int line = 0, col = 0;
    for (int i = 0; i < pos && i < g_tb.count; i++) {
        if (utext_at(&g_tb, i) == '\n') { line++; col = 0; continue; }
        if (++col >= g_cols) { line++; col = 0; }
    }
    *out_line = line;
    *out_col = col;
}

// LINE NUMBERS DOWN THE LEFT, off unless `-n` asked for them.
//
// Two separate things answer "where am I", and they answer different
// questions. The status bar's `Ln N, Col N` is always on and costs no
// horizontal space -- that is Notepad's and VS Code's answer, and it
// tells you where the CARET is. The gutter tells you which line any
// row on screen is, which is the question you have when something else
// named a line number at you, and it costs columns that an 80-column
// terminal does not have to spare. So the cheap one is unconditional
// and the expensive one is opt-in.
static int g_gutter;

// Wide enough for the highest line number the file can show, plus one
// space. DERIVED from the count rather than fixed at 4, so a short file
// does not pay for digits it will never use -- and RIGHT-ALIGNED below,
// which is what keeps the text from shifting sideways as the numbers
// grow past a power of ten.
static int gutter_w(int total_lines) {
    if (!g_gutter) return 0;
    int digits = 1;
    for (int n = total_lines; n >= 10; n /= 10) digits++;
    if (digits < 3) digits = 3;   // a floor, so short files do not jitter
    return digits + 1;            // + the separating space
}

static void render(const char *path, const char *status) {
    int text_rows = g_rows - 1; // the last row is the status bar
    int cur_line, cur_col;
    locate(g_tb.ed.cursor, &cur_line, &cur_col);

    // Scroll only as far as it takes to keep the caret on screen, which
    // is what makes a long file feel like one screen moving rather than
    // jumping about.
    static int first_line;
    if (cur_line < first_line) first_line = cur_line;
    if (cur_line >= first_line + text_rows) first_line = cur_line - text_rows + 1;
    if (first_line < 0) first_line = 0;

    // Total lines, for the gutter's width. Counted rather than tracked
    // because the buffer is edited through utext and a stale count
    // would size the gutter for a file that no longer exists.
    int total_lines = 1;
    for (int i = 0; i < g_tb.count; i++)
        if (utext_at(&g_tb, i) == '\n') total_lines++;
    int gw = gutter_w(total_lines);
    // The text gets what the gutter leaves. Everything below wraps
    // against THIS, not g_cols, or a numbered file wraps in a different
    // place from the one the caret is placed at.
    int text_cols = g_cols - gw;
    if (text_cols < 8) { gw = 0; text_cols = g_cols; }   // too narrow to number

    ansi("\x1b[H");
    int line = 0, col = 0, row_drawn = 0;
    // A number is printed at the START of a visible row, and a WRAPPED
    // row gets blanks instead -- a continuation is not a new line, and
    // numbering it would make the file look longer than it is. `vim`
    // and `nano -l` both do this.
    int need_num = 1;
    for (int i = 0; i < g_tb.count; i++) {
        char c = utext_at(&g_tb, i);
        int visible = (line >= first_line && line < first_line + text_rows);
        if (visible && gw && need_num) {
            char num[16];
            snprintf(num, sizeof num, "%*d ", gw - 1, line + 1);
            put(num);
            need_num = 0;
        }
        if (c == '\n') {
            if (visible) { ansi("\x1b[K"); put("\r\n"); row_drawn++; }
            line++; col = 0; need_num = 1;
            if (line >= first_line + text_rows) break;
            continue;
        }
        if (visible) write(1, &c, 1);
        if (++col >= text_cols) {
            if (visible) {
                put("\r\n");
                row_drawn++;
                // A wrapped continuation: pad the gutter so the text
                // stays in its column.
                if (gw) { for (int k = 0; k < gw; k++) put(" "); }
            }
            line++; col = 0;
            if (line >= first_line + text_rows) break;
        }
    }
    // Clear to the bottom of the text area: ESC[J erases from the caret
    // down, so one sequence covers however many rows the file left
    // blank. Without it a shorter file leaves the last one's tail on
    // screen, which reads as text that will not delete.
    ansi("\x1b[K\x1b[J");

    // The status bar, in inverse video so it cannot be mistaken for the
    // file. On the LAST row, addressed absolutely rather than reached by
    // padding with newlines -- padding was how the kernel version did it
    // and it needed a paragraph of comment to explain why the count came
    // out right.
    // **Ln/Col IS ALWAYS ON**, gutter or not: it costs no horizontal
    // space, and "where is the caret" is the question you have most
    // often. 1-BASED, because every editor and every compiler error
    // that will ever name a line to you counts from 1 -- reporting the
    // internal 0-based index here would make this the one place that
    // disagreed.
    char bar[160];
    snprintf(bar, sizeof bar, "-- %s -- F2 Save  F3 Exit  Ln %d, Col %d%s%s --",
             path, cur_line + 1, cur_col + 1,
             status && status[0] ? "  -- " : "", status ? status : "");
    ansi_at(g_rows - 1, 0);
    ansi("\x1b[7m");
    for (int i = 0; bar[i] && i < g_cols; i++) write(1, &bar[i], 1);
    ansi("\x1b[0m\x1b[K");

    // The caret sits past the gutter, or the column it reports and the
    // column it is drawn at disagree by exactly the gutter's width.
    ansi_at(cur_line - first_line, cur_col + gw);
    (void)row_drawn;
}

// --- the file ---------------------------------------------------------

static void load(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return; // a new file: an empty buffer is the right answer
    int64_t n = 0, got;
    while (n < (int64_t)sizeof g_io - 1 &&
           (got = read(fd, g_io + n, (size_t)(sizeof g_io - 1 - n))) > 0)
        n += got;
    close(fd);
    for (int64_t i = 0; i < n; i++) utext_putc(&g_tb, g_io[i]);
    g_tb.ed.cursor = 0; // land at the start, not the append point
}

static int save(const char *path) {
    int n = 0;
    for (int i = 0; i < g_tb.count && n < (int)sizeof g_io - 1; i++)
        g_io[n++] = utext_at(&g_tb, i);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return 0;
    int64_t w = n ? write(fd, g_io, (size_t)n) : 0;
    close(fd);
    return w == (int64_t)n;
}

// The terminal decoder's state -- one per input stream.
static struct termkey_state g_keys;

int main(int argc, char **argv) {
    // `-n` before or after the path, and exactly one path. A flag-only
    // invocation is a usage error rather than an empty editor: `edit`
    // with nothing to edit has nothing to do, and guessing a filename
    // is the sort of help nobody wants.
    const char *path = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0) { g_gutter = 1; continue; }
        if (path) { cmd_usage("edit [-n] <file>"); return 1; }
        path = argv[i];
    }
    if (!path) {
        cmd_usage("edit [-n] <file>");
        return 1;
    }

    utext_init_buf(&g_tb, g_doc, (int)sizeof g_doc);
    load(path);
    size_up();

    // RAW, and the terminal is put back before this program returns --
    // an editor that left the terminal in raw mode would hand the shell
    // a prompt with no echo, which looks exactly like a hung machine.
    struct tty_termios saved;
    int is_tty = sys_tcgetattr(0, &saved) == 0;
    if (is_tty) sys_tty_raw(0);
    ansi("\x1b[?1049h");   // the alternate screen: quitting gives the transcript back

    char status[48];
    status[0] = '\0';
    int rc = 0;

    for (;;) {
        render(path, status);
        status[0] = '\0';

        // **DECODED, because fd 0 is a TERMINAL.** A special key arrives
        // as an ANSI sequence (api/termkey.h) and is reassembled here
        // into the code the shared edit core switches on. A byte that
        // is not part of one passes through untouched; a sequence in
        // progress simply asks for the next byte.
        char c;
        int key;
        for (;;) {
            if (read(0, &c, 1) <= 0) { key = -1; break; }
            key = termkey_feed(&g_keys, (unsigned char)c);
            if (key == TERMKEY_MORE) continue;
            if (key == TERMKEY_NONE) continue;   // a sequence we do not know
            break;
        }
        if (key < 0) break;                      // end of input closes the editor

        if (key == KEY_F3 || key == 27) break;      // Esc, or nano's Ctrl+X
        if (key == KEY_F2) {                        // nano's Ctrl+O
            snprintf(status, sizeof status, save(path) ? "saved" : "SAVE FAILED");
            continue;
        }
        // EVERYTHING ELSE IS THE SHARED EDIT CORE'S. Arrows, Home/End,
        // Backspace, Delete, selection, typing -- one keymap, the same
        // one Notepad uses (ui/uui_edit.h). Enter is deliberately not
        // consumed by it, because a field commits and a document inserts
        // a newline, and this is a document.
        if (key == '\n' || key == '\r') { utext_insert(&g_tb, '\n'); continue; }
        utext_key(&g_tb, key, 0);
    }

    if (is_tty) sys_tcsetattr(0, &saved);
    ansi("\x1b[?1049l");
    return rc;
}
