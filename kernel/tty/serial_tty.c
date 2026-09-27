// The serial debug console's terminal: a tty_driver over serial.c's
// debug port (COM2 when present, else COM1).
//
// What it adds over the console reading the port itself is everything
// above the driver line: canonical input and echo, Ctrl-C through the
// discipline -- in the RX IRQ, so it reaches a command that is running
// -- and a terminal a program can be handed as fds 0/1/2, read, ask its
// size, and put in raw mode. Linux's shape is a getty on ttyS1 beside
// `console=ttyS0`; debug_console.c is the getty, and each command a
// session (tty_attach_kernel_session(), tty_hangup()).
#include "tty_internal.h"
#include "serial.h"
#include "serial_tty.h"

// A serial terminal expects CR LF; the discipline and programs emit LF.
// OPOST/ONLCR's job, done in the driver because nothing else here has a
// line that needs it.
//
// DRAINED AS IT GOES, not only at the end: a program's write can be far
// bigger than the port's queue, which drops what does not fit -- the tail
// of a long `ls` listing, truncation notice and all. Every chunk is
// pushed out before the next is queued; a stalled reader still costs one
// bounded flush and then nothing (serial.c's latch).
#define SERIAL_TTY_CHUNK 256

static void serial_output(struct tty *t, const char *buf, unsigned len) {
    (void)t;
    for (unsigned i = 0; i < len; i++) {
        if (buf[i] == '\n') serial_dbg_putc('\r');
        serial_dbg_putc(buf[i]);
        if (i % SERIAL_TTY_CHUNK == SERIAL_TTY_CHUNK - 1) serial_dbg_flush();
    }
    // PUSHED OUT NOW, bounded (a stalled reader latches it off). Left
    // queued, a backlog keeps the tickless idle on its periodic tick
    // (clockevent.c, serial_tx_pending()) -- the idle-deadline KTEST
    // failed 2 runs in 20 without this and 0 in 20 with it.
    serial_dbg_flush();
}

static const struct tty_driver serial_driver = {
    .name = "serial",
    .output = serial_output,
    .winsize = 0, // a serial line cannot say; the stored 80x24 is used
};

static struct tty *g_tty;

// STRAIGHT TO THE DISCIPLINE, not through tty_input(): that one expands
// the console keyboard's KEY_* codes (0x80 and up) into escape
// sequences, and a serial line's bytes are terminal bytes already --
// UTF-8 typed at a program must arrive as typed.
static void serial_rx(uint8_t c) { tty_ldisc_input(g_tty, c, 0); }

// What the line is set to at the start of every session. DEL is what a
// terminal emulator sends for Backspace; the console's '\b' is what a PC
// keyboard driver produces. Linux's default is DEL.
static void serial_defaults(struct tty *t) {
    struct tty_termios tio;
    tty_termios_defaults(&tio);
    tio.cc[TTY_VERASE] = 0x7F;
    tty_set_termios(t, &tio);
}

void serial_tty_reset(void) {
    if (g_tty) serial_defaults(g_tty);
}

struct tty *serial_tty_create(void) {
    if (g_tty) return g_tty;
    struct tty *t = tty_create(&serial_driver, 0);
    if (!t) return 0;
    serial_defaults(t);
    struct tty_winsize ws = { .rows = 24, .cols = 80 };
    tty_set_winsize(t, &ws);
    g_tty = t;
    serial_dbg_set_rx(serial_rx);
    return t;
}
