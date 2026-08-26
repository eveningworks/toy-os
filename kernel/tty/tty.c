// The terminal object, its registry, and who owns one.
//
// See kernel/tty.h for what a terminal IS here, and docs/tty-design.md
// for the staging. This file holds the object and the two questions
// that are about a terminal rather than about the bytes crossing it:
// who owns it, and which process group is in front of it. What happens
// to a byte is ldisc.c.
//
// tty0 -- the physical console -- lives here too, because its driver is
// four lines and a separate file for it would be filing rather than
// splitting.
#include "tty_internal.h"
#include "scheduler.h"
#include "signal.h"
#include "signal_abi.h"
#include "syscall_abi.h" // SYS_RETRY -- the value a woken reader gets
#include "vga.h"
#include "errno.h"
#include "string.h"
#include <stddef.h>

static struct tty g_ttys[TTY_MAX];

// --- tty0's driver ---------------------------------------------------
//
// The physical console: bytes out go to the framebuffer console, which
// is a WRITE-THROUGH rather than a queue. That is the one structural
// difference from a pty, and it is right: there is no second process to
// read the console's output, so buffering it would only add a place for
// it to be stuck.

static void console_output(struct tty *t, const char *buf, unsigned len) {
    (void)t;
    for (unsigned i = 0; i < len; i++) vga_putc(buf[i]);
}

// DERIVED, never stored: the font size is a setting, so the console's
// grid changes under a running program. A copy here is the one that goes
// stale, and a full-screen program drawing to a stale size paints past
// the bottom of the screen.
static void console_winsize(struct tty *t, uint16_t *rows, uint16_t *cols) {
    (void)t;
    *rows = (uint16_t)vga_rows();
    *cols = (uint16_t)vga_cols();
}

static const struct tty_driver console_driver = {
    .name = "console",
    .output = console_output,
    .winsize = console_winsize,
};

// The control characters and lflag a fresh terminal starts with.
//
// ONE FUNCTION BECAUSE THERE ARE TWO TERMINALS TO CREATE -- tty0 at
// boot and a pty on demand -- and the list was written out in both.
// Adding TTY_VSUSP made that concrete: a fifth slot set in one place
// and not the other is a Ctrl-Z that works on the console and not in a
// window, which is exactly the kind of difference the tty layer exists
// to abolish.
static void tty_termios_defaults(struct tty_termios *tio) {
    tio->lflag = TTY_LFLAG_DEFAULT;
    tio->cc[TTY_VINTR]  = 0x03; // ^C
    tio->cc[TTY_VERASE] = '\b';
    tio->cc[TTY_VKILL]  = 0x15; // ^U
    tio->cc[TTY_VEOF]   = 0x04; // ^D
    tio->cc[TTY_VSUSP]  = 0x1A; // ^Z
}

void tty_init(void) {
    struct tty *t = &g_ttys[0];
    k_memset(t, 0, sizeof *t);
    t->used = 1;
    t->index = 0;
    t->drv = &console_driver;
    tty_termios_defaults(&t->tio);
}

struct tty *tty_console(void) { return &g_ttys[0]; }

struct tty *tty_at(int index) {
    if (index < 0 || index >= TTY_MAX) return NULL;
    return g_ttys[index].used ? &g_ttys[index] : NULL;
}

int tty_index(const struct tty *t) { return t ? t->index : -1; }
int tty_count(void) { return TTY_MAX; }

struct tty *tty_create(const struct tty_driver *drv, void *drv_data) {
    if (!drv || !drv->output) return NULL;
    // From 1: tty0 is the console and is never handed out.
    for (int i = 1; i < TTY_MAX; i++) {
        if (g_ttys[i].used) continue;
        struct tty *t = &g_ttys[i];
        k_memset(t, 0, sizeof *t);
        t->used = 1;
        t->index = i;
        t->drv = drv;
        t->drv_data = drv_data;
        tty_termios_defaults(&t->tio);
        return t;
    }
    return NULL;
}

void tty_destroy(struct tty *t) {
    if (!t || !t->used || t->index == 0) return; // tty0 is not destroyable
    // WAKE BEFORE FREEING. scheduler.h's second rule for wait channels:
    // a channel whose object is going away must have its waiters
    // released first, or they are parked on an address that no longer
    // means anything. They re-read, find the terminal gone, and get EOF.
    t->used = 0;
    scheduler_wake(tty_wait_chan(t), SYS_RETRY);
}

void *tty_driver_data(struct tty *t) { return t ? t->drv_data : NULL; }

const char *tty_driver_name(const struct tty *t) {
    return (t && t->drv) ? t->drv->name : "?";
}

const void *tty_wait_chan(const struct tty *t) { return t; }

// --- the queue -------------------------------------------------------

void tty_enqueue(struct tty *t, uint8_t byte, uint8_t mods) {
    unsigned next = (t->in_head + 1) % TTY_INQ_MAX;
    if (next == t->in_tail) return; // full: drop, as the keyboard ring did
    t->inq[t->in_head] = ((uint32_t)mods << 16) | byte;
    t->in_head = next;
    // Release anything parked in a read of this terminal. Runs in the
    // keyboard IRQ on tty0, so it may only flip scheduler state and
    // write an already-saved trapframe -- which is all scheduler_wake()
    // does. SYS_RETRY is the wake value, so a reader that finds the byte
    // already taken by somebody else parks again rather than reporting a
    // spurious end of input.
    scheduler_wake(tty_wait_chan(t), SYS_RETRY);
}

// The wake half of tty_enqueue(), on its own: end of input has no byte
// to queue and still has to release a parked reader.
void tty_enqueue_wake(struct tty *t) {
    scheduler_wake(tty_wait_chan(t), SYS_RETRY);
}

void tty_input(struct tty *t, uint8_t byte, uint8_t mods) {
    if (!t || !t->used) return;
    tty_ldisc_input(t, byte, mods);
}

void tty_output(struct tty *t, const char *buf, unsigned len) {
    if (!t || !t->used || !buf) return;
    t->drv->output(t, buf, len);
}

// --- termios ---------------------------------------------------------

void tty_get_winsize(struct tty *t, struct tty_winsize *out) {
    if (!t || !out) return;
    if (t->drv->winsize) {
        uint16_t r = 0, c = 0;
        t->drv->winsize(t, &r, &c);
        if (r && c) { out->rows = r; out->cols = c; return; }
    }
    *out = t->ws;
}

void tty_set_winsize(struct tty *t, const struct tty_winsize *ws) {
    if (t && ws) t->ws = *ws;
}

void tty_get_termios(const struct tty *t, struct tty_termios *out) {
    if (!t || !out) return;
    *out = t->tio;
}

void tty_set_termios(struct tty *t, const struct tty_termios *tio) {
    if (!t || !tio) return;
    // A HALF-TYPED LINE DOES NOT SURVIVE THE CHANGE. Carrying it into
    // raw mode would make its bytes readable the instant the mode
    // changed, which is not something the program asking for raw mode
    // requested -- it asked for what arrives NEXT. POSIX's TCSAFLUSH,
    // and the only one of its three flush modes worth having here.
    tty_ldisc_discard_line(t);
    t->tio = *tio;
}

// --- who may read ----------------------------------------------------

int tty_check_background_read(struct tty *t) {
    if (!t) return 0;

    // NO OWNER MEANS NO FOREGROUND, so there is nothing to be outside
    // of. This is what keeps the check out of the way of the FIRST
    // reader -- the process that becomes the owner by reading, which on
    // every boot here is a shell.
    if (!t->owner_pid || !t->fg_pgid) return 0;

    int pid = scheduler_current_tgid(); // the process owns a terminal
    if (!pid) return 0;  // kernel context: it has no group to be outside
    if (scheduler_pgid(pid) == t->fg_pgid) return 0;

    // IGNORING THE SIGNAL DOES NOT EARN THE KEYBOARD. A process that has
    // asked not to be stopped cannot be stopped, so the only two answers
    // left are "let it steal input" and "refuse the read"; POSIX picks
    // the second and so does this.
    if (scheduler_signal_ignored(pid, SIGTTIN)) return -EIO;

    // THE WHOLE GROUP, not the one process: a background PIPELINE whose
    // first stage reads would otherwise be half stopped and half
    // running, with the running half waiting on a pipe nothing will
    // ever fill.
    signal_send_group(scheduler_pgid(pid), SIGTTIN);
    return 1;
}

// --- ownership -------------------------------------------------------

int tty_owner(const struct tty *t) { return t ? t->owner_pid : 0; }

void tty_set_owner(struct tty *t, int pid) {
    if (!t) return;
    t->owner_pid = pid;
    // THE OWNER'S OWN GROUP GOES IN FRONT, so a terminal is never in the
    // state "somebody owns it and nothing is in front of it" -- in which
    // an interrupt would have nowhere to go and be silently dropped
    // rather than falling back to the line editor. A shell moves it from
    // here per job; one that never does simply keeps this.
    t->fg_pgid = pid ? scheduler_pgid(pid) : 0;
}

int tty_fg_pgid(const struct tty *t) { return t ? t->fg_pgid : 0; }

int tty_set_fg_pgid(struct tty *t, int pgid) {
    if (!t) return -ENODEV;
    if (!t->owner_pid) return -ENODEV;
    if (scheduler_current_tgid() != t->owner_pid) return -EPERM;
    if (pgid < 1 || !scheduler_pgid_live(pgid)) return -ESRCH;
    t->fg_pgid = pgid;
    return 0;
}

void tty_set_bypass(struct tty *t, int on) {
    if (!t) return;
    // Entering bypass drops a half-assembled line for the same reason a
    // termios change does: it would otherwise become readable, in one
    // lump, at whatever moment the compositor let go.
    if (on && !t->bypass) tty_ldisc_discard_line(t);
    t->bypass = on ? 1 : 0;
}

int tty_bypassed(const struct tty *t) { return t ? t->bypass : 0; }

// --- the console, as it was ------------------------------------------

int  tty_console_owner(void)            { return tty_owner(tty_console()); }
void tty_set_console_owner(int pid)     { tty_set_owner(tty_console(), pid); }
int  tty_foreground_pgid(void)          { return tty_fg_pgid(tty_console()); }
int  tty_set_foreground_pgid(int pgid)  { return tty_set_fg_pgid(tty_console(), pgid); }
