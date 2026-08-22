#ifndef KERNEL_TTY_H
#define KERNEL_TTY_H

#include <stdint.h>
#include "tty_abi.h" // struct tty_termios -- shared with ring 3

// The TTY layer: a terminal is an OBJECT, and there is more than one.
//
// **WHAT A TERMINAL IS HERE.** An input queue, a line discipline over
// it, an output sink, a `termios` describing the discipline's
// behaviour, an owning process and a FOREGROUND GROUP. What varies
// between terminals is only what sits underneath -- the DRIVER:
//
//     tty0   input from the keyboard IRQ, output to vga_write()
//     ttyN   input from a pty master's write, output to its read
//
// Everything above that line -- canonical mode, echo, erase and kill,
// what Ctrl-C means -- is written once and behaves identically for the
// physical console and for a terminal window. That is the whole point
// of the layer, and the test of whether it is real: if a shell has to
// know which kind it is talking to, this has failed.
//
// See docs/tty-design.md for the staging and for what this deliberately
// is NOT (a /dev namespace, job control, a portable termios).
//
// **WHY NOT ONE CONSOLE ANY MORE.** This file used to hold two globals
// and a comment predicting that they would become per-terminal "which is
// bookkeeping, because everything below is already asked through
// functions rather than read as globals". That prediction is what made
// this cheap: the console-shaped accessors at the bottom still exist and
// still mean what they meant, they just ask tty0.

struct tty;

// What a terminal's bytes ride on. A registry entry, the same shape as
// `display_driver` and `block_device`: the layer above knows nothing
// about keyboards or framebuffers, and a new kind of terminal is a
// driver rather than a branch.
struct tty_driver {
    const char *name; // "console", "pty" -- what /bin/tty prints

    // Called with bytes the terminal wants to EMIT: a program's output
    // through a slave, and the discipline's own echo. For tty0 this is
    // vga_write(); for a pty it appends to the buffer the master reads.
    //
    // MAY BE CALLED FROM AN INTERRUPT (echo happens where the key
    // arrives), so it may not block, allocate, or touch user memory.
    void (*output)(struct tty *t, const char *buf, unsigned len);
};

#define TTY_MAX 8 // terminals, kernel-wide: tty0 plus one per window

// Sets up tty0 -- the physical console -- and nothing else. Called from
// kernel_main() before anything can type.
//
// **tty0 STARTS RAW**, unlike every terminal made after it, and the
// reason is that its readers predate the layer: the kernel shell and
// `/bin/tosh` both run `kernel/lib/klineedit.c`, which does its own
// editing and its own echo. Canonical mode underneath them would buffer
// the line a second time and echo it twice. They will ask for raw
// explicitly once SYS_TCSETATTR exists (docs/tty-design.md, stage 2) and
// this special case goes away with that.
void tty_init(void);

// tty0. Never NULL after tty_init().
struct tty *tty_console(void);

// The terminal at `index`, or NULL. 0 is the console; the rest are ptys
// as they are created. For the QUERY_TTY provider and for tests.
struct tty *tty_at(int index);
int tty_index(const struct tty *t);
int tty_count(void); // how many slots exist at all, not how many are live

// Allocates a terminal on `drv`, or NULL when TTY_MAX are live. Starts
// with TTY_LFLAG_DEFAULT -- POSIX's, see abi/tty_abi.h.
struct tty *tty_create(const struct tty_driver *drv, void *drv_data);
// Releases one. Refuses tty0, which nothing may destroy.
void tty_destroy(struct tty *t);
void *tty_driver_data(struct tty *t);
const char *tty_driver_name(const struct tty *t);

// --- what a terminal is parked on ------------------------------------
//
// A CHANNEL PER TERMINAL, which is the rule scheduler.h states: an
// address naming the object waited on, so a keystroke on the console
// does not wake every window's shell. The terminal's own address is
// that name -- unique, and it outlives every wait on it because a tty
// is never freed while a reader is parked (tty_destroy() wakes them).
const void *tty_wait_chan(const struct tty *t);

// --- the driver's side -----------------------------------------------

// One byte has arrived AT the terminal -- a key on the console, or a
// write to a pty master. Runs the line discipline: echo, erase and
// kill in canonical mode, INTR when ISIG is set, and finally the queue.
//
// `mods` is the KEY_MOD_* word the console's ring has always carried
// alongside the byte (api/keyboard.h). A terminal is a BYTE stream and
// this is not part of one -- it is carried because the ring-0 GUI
// readers cannot tell Shift-Tab from Tab without it, and dropping it
// here would be a regression in something unrelated. A pty passes 0.
// docs/tty-design.md's open questions records this as the one place the
// abstraction leaks.
//
// **CALLED FROM THE KEYBOARD IRQ**, so it may only fill buffers, flip
// scheduler state and call the driver's output hook. Signal delivery
// keeps to that too: signal_send_group() sets a bit and the kernel acts
// on it on the way back to ring 3.
void tty_input(struct tty *t, uint8_t byte, uint8_t mods);

// Bytes the terminal should EMIT -- what a slave write does. Goes
// through the driver's output hook.
void tty_output(struct tty *t, const char *buf, unsigned len);

// --- the reader's side -----------------------------------------------

// Pops up to `len` bytes, NEVER BLOCKING -- the caller decides how to
// wait, because the two callers wait very differently (a ring-0 reader
// halts, a ring-3 one parks in its syscall). Returns the count, or 0
// when nothing is readable right now.
//
// In canonical mode a line that is still being typed is NOT readable:
// it becomes readable in one piece when the newline arrives. That is
// what ICANON means, and it is why this can return 0 with the terminal
// full of characters.
unsigned tty_read(struct tty *t, char *dst, unsigned len);

// The same, one byte at a time and with the modifier word, for the
// console's ring-0 readers. Returns -1 when nothing is readable.
int tty_read_key(struct tty *t, uint8_t *out_mods);

// Is anything readable right now? Distinct from `tty_read(..., 0)`
// because a caller that is deciding whether to park must not consume.
int tty_readable(const struct tty *t);

// --- termios ---------------------------------------------------------

void tty_get_termios(const struct tty *t, struct tty_termios *out);
// Setting it DISCARDS a partly-typed canonical line rather than
// carrying it into raw mode, where its bytes would suddenly become
// readable in a way the program that switched modes never asked for.
void tty_set_termios(struct tty *t, const struct tty_termios *tio);

// --- ownership and the foreground group ------------------------------
//
// PER TERMINAL NOW. These are the same two questions the console has
// always answered, asked of one terminal among several.

// The process that owns `t`, or 0. On the console this is the first
// process to read fd 0 (syscall_fd.c's rule, unchanged); on a pty it is
// set when the pty is created, because the process that opened it is
// unambiguously the one that has it.
int  tty_owner(const struct tty *t);
// Setting an owner also puts that owner's own group in front, so a
// terminal is never in the state "owned, with nothing to interrupt".
void tty_set_owner(struct tty *t, int pid);

int  tty_fg_pgid(const struct tty *t);
// tcsetpgrp(). 0, or a negative errno: -EPERM if the caller does not
// own this terminal, -ENODEV if nobody does, -ESRCH for a group with no
// live member.
//
// **ONLY THE OWNER MAY CALL IT.** Not a privilege check in the sense
// this kernel does not have -- an ownership one: a process that has
// never read a terminal has no business deciding what its Ctrl-C
// interrupts, and letting it would be a way to point somebody else's
// interrupt at a process of your choosing.
int  tty_set_fg_pgid(struct tty *t, int pgid);

// --- the console, as it was ------------------------------------------
//
// The pre-layer spelling, kept because the callers are right to ask
// about "the console" specifically -- syscall_fd.c claims it, and
// SYS_TCSETPGRP with no fd means it. One line each, over tty0.
int  tty_console_owner(void);
void tty_set_console_owner(int pid);
int  tty_foreground_pgid(void);
int  tty_set_foreground_pgid(int pgid);

// **THE CONSOLE'S DISCIPLINE IS BYPASSED WHILE A COMPOSITOR HOLDS THE
// KEYBOARD**, and that is not a special case bolted on -- it is what
// Linux does. A compositor puts its VT in KD_GRAPHICS and mutes the
// keyboard (KDSKBMODE, K_OFF) so the kernel stops making tty input out
// of scancodes at all, because the compositor reads evdev itself.
//
// Here the same thing: bytes go straight onto tty0's queue with no
// echo, no canonical buffering and no INTR, so win_input.c drains
// exactly what it always did and a desktop cannot lose a keystroke to a
// line the console was assembling.
void tty_set_bypass(struct tty *t, int on);
int  tty_bypassed(const struct tty *t);

#endif // KERNEL_TTY_H
