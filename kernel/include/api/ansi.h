#ifndef API_ANSI_H
#define API_ANSI_H

#include <stdint.h>
#include "vga.h" // enum vga_color -- what an SGR parameter resolves to

// ANSI escape sequences as a pure state machine: the SGR (colour)
// subset, plus cursor movement and erasing.
//
// WHY THIS EXISTS. Colour used to be a SYSCALL -- `ls` called
// sys_set_color(), which reaches around the byte stream and changes the
// console's state directly. That is a side channel, and it leaks in the
// obvious way: `ls > out.txt` recoloured the console while its bytes went
// to the file, and `ls | cat` coloured whatever the console happened to
// be printing instead. An escape sequence travels IN the stream, so it
// lands wherever the output lands -- which is why every real terminal
// does it this way and why `--color=never` is a thing a program can
// meaningfully offer.
//
// WHAT IS AND IS NOT SUPPORTED. SGR (`ESC [ ... m`, including REVERSE
// VIDEO, which is what a status bar is made of), cursor movement
// (CUP/CUU/CUD/CUF/CUB/CHA/VPA), erasing (ED/EL), save/restore, and
// DECTCEM cursor visibility (`?25h`/`?25l`). That is the set a program
// drawing a screen actually uses -- a board game repainting a grid, a
// progress display rewriting one line.
//
// Cursor movement was swallowed until 2026-08-21, on the reasoning that
// a program moving the cursor would fight the console's scrollback. The
// scrollback part of that is still true and is handled rather than
// avoided: it records the BYTE STREAM, so a cursor-addressed program's
// history is not a meaningful transcript. Real terminals have the same
// problem and solve it with an alternate screen buffer, which this does
// not have. What is NOT here: scrolling regions (DECSTBM), the
// alternate screen, tab stops, and character-set selection. Those
// belong to the TTY milestone (docs/roadmap.md).
//
// NO HARDWARE, ON PURPOSE. This file resolves bytes to colours and knows
// nothing about a screen, so vga.c drives it for the physical console
// AND for any installed sink (a GUI Terminal's scrollback), and a KTEST
// exercises it with no display at all. That is the same split geom.h
// uses for drawing.
//
// The 8 ANSI colours are NOT in the same order as enum vga_color -- ANSI
// counts 0..7 as black/red/green/yellow/blue/magenta/cyan/white while VGA
// interleaves its bits -- so the mapping is a table, not arithmetic.

enum ansi_result {
    ANSI_PASS,   // not part of a sequence -- print this byte normally
    ANSI_EATEN,  // consumed as part of a sequence; nothing to do yet
    ANSI_SGR,    // a complete SGR sequence: apply `fg`/`bg` from the state
    ANSI_CTRL,   // a complete cursor/erase sequence: act on `op`/`a`/`b`
};

// What an ANSI_CTRL asks for. The parser resolves the letter AND applies
// the defaults, so a caller never repeats rules like "a missing or zero
// count means 1" -- which is exactly the sort of thing that ends up
// implemented differently in two places.
enum ansi_op {
    ANSI_OP_NONE = 0,
    ANSI_OP_MOVE_TO,   // CUP/HVP: row `a`, column `b`, both 1-BASED
    ANSI_OP_UP,        // CUU: by `a`
    ANSI_OP_DOWN,      // CUD
    ANSI_OP_RIGHT,     // CUF
    ANSI_OP_LEFT,      // CUB
    ANSI_OP_COLUMN,    // CHA: to column `a`, 1-based
    ANSI_OP_ROW,       // VPA: to row `a`, 1-based
    // Erase. `a` is the mode: 0 = cursor to end, 1 = start to cursor,
    // 2 = all. ED's mode 3 (scrollback too) is reported as 2 -- see
    // vga.c on why the history is not the screen's to erase.
    ANSI_OP_ERASE_DISPLAY, // ED
    ANSI_OP_ERASE_LINE,    // EL
    ANSI_OP_SAVE,      // SCP -- save the position
    ANSI_OP_RESTORE,   // RCP
    ANSI_OP_SHOW,      // DECTCEM `?25h`
    ANSI_OP_HIDE,      // `?25l`
};

#define ANSI_MAX_PARAMS 8

struct ansi_parser {
    uint8_t state;      // internal; zero-initialised is "ground"
    uint8_t nparam;
    uint8_t bold;       // SGR 1 -- brightens the foreground
    uint8_t reverse;    // SGR 7 -- fg and bg swap, and the parser does
                        // the swapping, so a caller never sees this
    uint8_t private;    // a `?`-style private sequence, swallowed whole
    uint16_t param[ANSI_MAX_PARAMS];
    enum vga_color fg;  // the colours a completed ANSI_SGR resolves to
    enum vga_color bg;
    // What a completed ANSI_CTRL resolves to. `a`/`b` already have
    // their defaults applied.
    enum ansi_op op;
    uint16_t a, b;
};

// Arms a parser with the colours the console currently shows, so a
// sequence that only sets a foreground leaves the background alone.
void ansi_init(struct ansi_parser *p, enum vga_color fg, enum vga_color bg);

// One byte in. See enum ansi_result -- the ONLY byte a caller prints is
// one that comes back ANSI_PASS.
enum ansi_result ansi_feed(struct ansi_parser *p, char c);

// The VGA colour an ANSI colour index (0..7) names. Exposed because the
// mapping is the part worth asserting, and a caller writing escapes
// (userland) needs the inverse of what the parser does.
enum vga_color ansi_color(int index, int bright);

#endif // API_ANSI_H
