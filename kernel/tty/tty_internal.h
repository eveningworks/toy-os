#ifndef KERNEL_TTY_INTERNAL_H
#define KERNEL_TTY_INTERNAL_H

// `struct tty`'s layout, shared by the two files that make up this
// subsystem and by nothing else.
//
// The `userland/wm/` pattern (see docs/decisions.md): tty.c owns the
// object, its registry and who owns a terminal; ldisc.c owns what
// happens to a byte on its way in and out. That is two concerns, not
// two components -- there is no boundary between them worth a public
// header, so the state is shared through this file of internals rather
// than through accessors nobody outside would call.

#include "tty.h"

#define TTY_INQ_MAX   512 // readable bytes queued. Bounds how far behind
                          // a reader may fall before keystrokes are
                          // dropped -- the keyboard ring's job, which
                          // this took over.
#define TTY_CANON_MAX 256 // a line being assembled. An over-long line is
                          // discarded rather than truncated -- ldisc.c.

// **A COMPLETE LINE MUST FIT IN THE QUEUE IT IS DELIVERED INTO**, or
// the discipline's promise not to truncate is broken one layer further
// down: a full canonical line plus its newline would be queued, the
// ring would fill, and the tail would be dropped with nobody the wiser.
// The queue was the same size as the buffer when this was written, and
// the "an over-long line is discarded, not truncated" KTEST is what
// found it -- it asked for 257 bytes back and got 255.
//
// A ring holds one fewer than its slot count (head == tail means
// empty), hence the -1.
_Static_assert(TTY_INQ_MAX - 1 >= TTY_CANON_MAX + 1,
               "a full canonical line plus its newline must fit in the input queue");

struct tty {
    int used;
    int index;
    const struct tty_driver *drv;
    void *drv_data;

    struct tty_termios tio;
    struct tty_winsize ws; // only consulted when the driver has no winsize hook

    // The process that claimed it, kept for reporting (QUERY_TTYS) and
    // for tty_check_background_read()'s fallback.
    int owner_pid;
    // **THE SESSION THIS TERMINAL BELONGS TO, and what permission is
    // keyed on.** Any process in it may move the foreground group; the
    // owning PID may not, on its own, because a shell's children are
    // different processes and must be able to take job control (see
    // tty_set_fg_pgid()).
    int sid;
    int fg_pgid;

    // See tty.h's tty_set_bypass(): a compositor holding the keyboard
    // mutes the discipline, which is KDSKBMODE/K_OFF on a Linux VT.
    int bypass;

    // The READABLE queue. Each slot is (mods << 16) | byte, which is
    // what the keyboard ring has always carried; tty.h says why a byte
    // stream is carrying modifiers at all.
    //
    // `volatile` and a head/tail pair rather than a lock: the producer
    // is the keyboard IRQ and the consumer is ordinary kernel code, so
    // this is the same single-producer/single-consumer ring the
    // keyboard driver had, moved rather than redesigned.
    volatile uint32_t inq[TTY_INQ_MAX];
    volatile unsigned in_head, in_tail;

    // The line being typed in canonical mode. Not readable until it is
    // finished, which is what ICANON means.
    char     canon[TTY_CANON_MAX];
    unsigned canon_len;

    // VEOF arrived on an EMPTY line: the next read that finds nothing
    // queued reports end of input instead of parking, and clears this.
    //
    // A FLAG RATHER THAN A BYTE IN THE QUEUE, because end of input is
    // not a character -- a reader must see it as a zero-length read, and
    // anything queued would arrive as data. It is consumed by one read,
    // so a second Ctrl-D is needed to end input twice, exactly as on a
    // real terminal.
    int eof_pending;
};

// ldisc.c's entry points, called by tty.c.
void tty_ldisc_input(struct tty *t, uint8_t byte, uint8_t mods);
void tty_ldisc_discard_line(struct tty *t);

// tty.c's, called by ldisc.c.
void tty_enqueue(struct tty *t, uint8_t byte, uint8_t mods);
void tty_enqueue_wake(struct tty *t);

#endif // KERNEL_TTY_INTERNAL_H
