#ifndef KERNEL_SERIAL_TTY_H
#define KERNEL_SERIAL_TTY_H

struct tty;

// The debug port's terminal, created on first call and wired to the
// port's RX IRQ from then on. NULL only if every tty slot is taken,
// which cannot happen at boot. kernel/tty/serial_tty.c.
struct tty *serial_tty_create(void);

// Puts the line back to its defaults -- canonical, echoing, DEL erases.
// What getty does before each login: a command that went raw (tosh, an
// editor) and exited without restoring would otherwise leave the NEXT
// one a line with no echo, on which a terminal's Enter (CR) never ends
// a command.
void serial_tty_reset(void);

#endif
