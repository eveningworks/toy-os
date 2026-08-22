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
// **WHICH IS NOT YET THE SAME AS WORKING IN A WINDOW, and the reason is
// worth knowing.** A full-screen program ADDRESSES its screen -- it
// says "put the caret at row 4, column 12" -- and that needs a GRID.
// The physical console has one (`kernel/lib/ansi.c` parses the escapes
// and vga.c moves a real cursor). The GUI Terminal does not: its screen
// is a character STREAM in a scrollback, which is right for a shell
// transcript and cannot express "go back up three rows". Giving it a
// grid is a named roadmap item, and it is the same item as the
// alternate screen buffer -- not a gap to paper over here.
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
#include "rt/sys.h"
#include "ui/utext.h"
#include "keyboard.h"
#include "lib/cmd.h"
#include <string.h>
#include <stdio.h>

// Static, not local: `struct utext` embeds an 8 KiB buffer, against
// USERLAND_CFLAGS' -Wframe-larger-than=2048 and a 16 KiB ring-3 stack
// with ONE guard page below it. The kernel version made the same call
// for the same reason, and its comment records that a stack-local copy
// blew the stack silently.
static struct utext g_tb;
static char g_io[UTEXT_CAP + 1];

static int g_rows = 25, g_cols = 80;

static void put(const char *s) { sys_write(1, s, strlen(s)); }

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

    ansi("\x1b[H");
    int line = 0, col = 0, row_drawn = 0;
    for (int i = 0; i < g_tb.count; i++) {
        char c = utext_at(&g_tb, i);
        int visible = (line >= first_line && line < first_line + text_rows);
        if (c == '\n') {
            if (visible) { ansi("\x1b[K"); put("\r\n"); row_drawn++; }
            line++; col = 0;
            if (line >= first_line + text_rows) break;
            continue;
        }
        if (visible) sys_write(1, &c, 1);
        if (++col >= g_cols) {
            if (visible) { put("\r\n"); row_drawn++; }
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
    char bar[160];
    snprintf(bar, sizeof bar, "-- %s -- F2 Save  F3 Exit%s%s --",
             path, status && status[0] ? "  -- " : "", status ? status : "");
    ansi_at(g_rows - 1, 0);
    ansi("\x1b[7m");
    for (int i = 0; bar[i] && i < g_cols; i++) sys_write(1, &bar[i], 1);
    ansi("\x1b[0m\x1b[K");

    ansi_at(cur_line - first_line, cur_col);
    (void)row_drawn;
}

// --- the file ---------------------------------------------------------

static void load(const char *path) {
    int fd = sys_open(path, 0);
    if (fd < 0) return; // a new file: an empty buffer is the right answer
    int64_t n = 0, got;
    while (n < (int64_t)sizeof g_io - 1 &&
           (got = sys_read(fd, g_io + n, (size_t)(sizeof g_io - 1 - n))) > 0)
        n += got;
    sys_close(fd);
    for (int64_t i = 0; i < n; i++) utext_putc(&g_tb, g_io[i]);
    g_tb.ed.cursor = 0; // land at the start, not the append point
}

static int save(const char *path) {
    int n = 0;
    for (int i = 0; i < g_tb.count && n < (int)sizeof g_io - 1; i++)
        g_io[n++] = utext_at(&g_tb, i);
    int fd = sys_open(path, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    if (fd < 0) return 0;
    int64_t w = n ? sys_write(fd, g_io, (size_t)n) : 0;
    sys_close(fd);
    return w == (int64_t)n;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        cmd_usage("edit <file>");
        return 1;
    }
    const char *path = argv[1];

    utext_init(&g_tb);
    load(path);
    size_up();

    // RAW, and the terminal is put back before this program returns --
    // an editor that left the terminal in raw mode would hand the shell
    // a prompt with no echo, which looks exactly like a hung machine.
    struct tty_termios saved;
    int is_tty = sys_tcgetattr(0, &saved) == 0;
    if (is_tty) sys_tty_raw(0);

    char status[48];
    status[0] = '\0';
    int rc = 0;

    for (;;) {
        render(path, status);
        status[0] = '\0';

        char c;
        if (sys_read(0, &c, 1) <= 0) break; // end of input closes the editor
        int key = (unsigned char)c;

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
    // Leave a clean screen and the caret at the top, as every full-screen
    // program does on its way out.
    ansi("\x1b[2J\x1b[H");
    return rc;
}
