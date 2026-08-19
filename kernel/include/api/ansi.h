#ifndef API_ANSI_H
#define API_ANSI_H

#include <stdint.h>
#include "vga.h" // enum vga_color -- what an SGR parameter resolves to

// ANSI escape sequences: the SGR (colour) subset, as a pure state machine.
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
// WHAT IS AND IS NOT SUPPORTED. SGR (`ESC [ ... m`) only: the colour and
// attribute sequence. Cursor movement, clearing and scrolling regions
// are RECOGNISED and swallowed rather than printed as garbage, but do
// nothing -- this kernel's console owns its own cursor (vga.c), and a
// program moving it around would fight the scrollback. Full terminal
// emulation belongs to the TTY milestone (docs/roadmap.md), not here.
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
};

#define ANSI_MAX_PARAMS 8

struct ansi_parser {
    uint8_t state;      // internal; zero-initialised is "ground"
    uint8_t nparam;
    uint8_t bold;       // SGR 1 -- brightens the foreground
    uint8_t private;    // a `?`-style private sequence, swallowed whole
    uint16_t param[ANSI_MAX_PARAMS];
    enum vga_color fg;  // the colours a completed ANSI_SGR resolves to
    enum vga_color bg;
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
