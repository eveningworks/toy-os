#ifndef ABI_TTY_ABI_H
#define ABI_TTY_ABI_H

#include <stdint.h>

// What a terminal's behaviour looks like from ring 3 -- `termios`, cut
// to what this OS actually has.
//
// In abi/ rather than kernel/ because SYS_TCGETATTR and SYS_TCSETATTR
// hand this struct across the syscall boundary, so its layout is a
// contract. Fixed-size and pointer-free, like every other message here.
//
// **TWO FLAGS AND ONE CONTROL CHARACTER, NOT POSIX'S FOUR WORDS AND 32.**
// A real `struct termios` carries iflag/oflag/cflag/lflag and NCCS
// control characters, nearly all describing hardware that stopped
// existing decades ago -- parity, modem control lines, baud rates,
// XON/XOFF. Copying the SHAPE and not the SIZE is this project's
// standing rule (CLAUDE.md), and the shape here is: a word of behaviour
// flags, and a table of characters that mean something other than
// themselves. It grows when a second real caller wants a field, not
// before.
//
// The names are POSIX's on purpose. Somebody who knows `termios` should
// recognise `ICANON` and `VINTR` and be right about what they do; a
// private spelling would be a new thing to learn that behaves like an
// old one.

// lflag -- "local" modes, POSIX's word for how the line discipline
// behaves rather than how the wire does.
#define TTY_ICANON (1u << 0) // assemble a LINE; a read returns nothing
                             // until one is complete. Off = raw: every
                             // byte is readable the moment it arrives.
#define TTY_ECHO   (1u << 1) // echo input back to the terminal's output
#define TTY_ISIG   (1u << 2) // INTR generates a signal instead of being
                             // delivered as an ordinary byte

// Control-character slots. POSIX has 32 of these; this has the ones
// with an implementation. An unused slot is not reserved for a future
// meaning -- adding one is a define and a branch in the discipline.
#define TTY_VINTR  0 // ^C -- SIGINT to the foreground group (needs ISIG)
#define TTY_VERASE 1 // backspace -- rub out the last character (ICANON)
#define TTY_VKILL  2 // ^U -- discard the whole pending line (ICANON)
#define TTY_VEOF   3 // ^D -- END OF INPUT (ICANON). On a line with text
                     // it delivers that text WITHOUT a newline; on an
                     // empty line the reader gets 0, which is what makes
                     // `cat` with no arguments end when you press it
#define TTY_NCCS   4 // slots in the table

struct tty_termios {
    uint32_t lflag;            // TTY_*
    uint8_t  cc[TTY_NCCS];     // TTY_V* -> the byte that means it
};

// How big the terminal is, in CHARACTER CELLS. What every full-screen
// program asks for first, and what `ioctl(TIOCGWINSZ)` answers on a real
// system.
//
// **CELLS, NOT PIXELS.** A terminal's size is a grid; a window's is in
// pixels, and the conversion belongs to whoever knows the font. For the
// physical console that is the kernel (it has the framebuffer and the
// glyph metrics); for a pty it is the terminal emulator, which measures
// its own window and says so. Neither answer is derivable from the
// other, which is why this is asked rather than computed.
struct tty_winsize {
    uint16_t rows;
    uint16_t cols;
};

// The state a new terminal starts in: POSIX's defaults, deliberately.
//
// A PROGRAM THAT KNOWS NOTHING ABOUT TERMINALS SHOULD GET WHAT IT
// EXPECTS -- a line at a time, echoed, and interruptible -- rather than
// this OS's own habits. The three shells here all edit for themselves
// and turn ICANON and ECHO off at startup, exactly as `readline` does on
// Linux; that is a property of shells, not of terminals, and baking it
// into the default would make every future program that just wants a
// line inherit a shell's preferences.
#define TTY_LFLAG_DEFAULT (TTY_ICANON | TTY_ECHO | TTY_ISIG)

#endif // ABI_TTY_ABI_H
