// The line discipline: what happens to a byte on its way into a
// terminal, and what a reader gets back out.
//
// This is the half that used to be spread across three places -- INTR
// recognition in the keyboard IRQ, line editing in ring 3, and "the
// console's owner" in a pair of globals. It is one implementation now,
// and it does not know whether the terminal underneath it is a
// framebuffer and a keyboard or a window and a pipe.
//
// **WHAT IT DELIBERATELY DOES NOT DO.** No iflag: no CR/NL translation
// beyond treating both as "the line ended", no XON/XOFF, no parity.
// Those describe wires this OS has never had. See abi/tty_abi.h on
// copying the shape and not the size.
//
// **AND THE HONEST COST OF CANONICAL MODE**, recorded where somebody
// editing this will see it: kernel/lib/klineedit.c already does line
// editing, in ring 3, shared by all three shells -- so ICANON here is a
// SECOND implementation of "assemble a line", and every shell turns it
// off. That is Linux's arrangement too (readline sets raw mode), and it
// was a deliberate choice rather than an oversight: a program that
// knows nothing about terminals and just wants a line should get one
// without linking an editor. docs/tty-design.md's "What this is NOT"
// states it at length.
#include "tty_internal.h"
#include "scheduler.h"
#include "signal.h"
#include "signal_abi.h"
#include <stddef.h>

static void echo(struct tty *t, char c) {
    if (!(t->tio.lflag & TTY_ECHO)) return;
    t->drv->output(t, &c, 1);
}

// Rub out the character to the left: back over it, paint a space, back
// again. The oldest terminal idiom there is, and the reason it is here
// rather than in the caller is that ECHO is the discipline's business
// -- a program in canonical mode has not been told a key was pressed at
// all, so it cannot possibly do this itself.
static void echo_erase(struct tty *t) {
    if (!(t->tio.lflag & TTY_ECHO)) return;
    t->drv->output(t, "\b \b", 3);
}

// A SIGNAL-GENERATING CHARACTER arrived on a terminal with ISIG set --
// INTR (^C) or SUSP (^Z). Returns 1 if it was CONSUMED, 0 if the caller
// should deliver it as an ordinary byte.
//
// THE TWO OUTCOMES ARE THE TWO STATES A SHELL IS IN, and both are
// right:
//
//   - A JOB IS RUNNING (the foreground group is not the owner's own):
//     the group gets the signal and the byte is DISCARDED, which is
//     what a line discipline does with these. Delivering it too would
//     leave a stray 0x03 for whoever reads next -- and the shell, one
//     instruction from putting its own group back in front, is exactly
//     who that is.
//   - NO JOB IS RUNNING (the shell's own group is in front, or nobody
//     owns this terminal): nothing is signalled and the byte goes
//     through, so Ctrl-C at a prompt still abandons the line exactly as
//     it does today -- KLINE_CANCEL, in the one line editor the shells
//     share. Ctrl-Z there is an ordinary byte the editor ignores, which
//     is right: there is no job to suspend, and suspending the shell
//     itself would leave nobody to resume it.
//
// Compared against the OWNER'S group rather than tracking a "is a job
// running" flag, because the flag would be a second record of the same
// fact -- and the one that goes stale when a shell exits without
// restoring the foreground group.
//
// Called from the keyboard IRQ on tty0, so it may only set bits and
// flip scheduler state; signal_send_group() keeps to that for
// everything except SIGKILL, which is why INTR sends SIGINT and SUSP
// sends SIGTSTP. A stop is a scheduler state flip and nothing else, so
// it is safe from here for the same reason (abi/signal_abi.h).
static int signal_char(struct tty *t, int sig) {
    if (!t->owner_pid || !t->fg_pgid) return 0;
    if (t->fg_pgid == scheduler_pgid(t->owner_pid)) return 0;

    signal_send_group(t->fg_pgid, sig);
    // POSIX flushes the input queue on a signal-generating character.
    // Only the PENDING LINE here, not the whole queue: bytes already
    // readable may belong to a reader this interrupt has nothing to do
    // with, and throwing them away would be a second, invisible effect
    // of pressing Ctrl-C.
    tty_ldisc_discard_line(t);
    return 1;
}

void tty_ldisc_discard_line(struct tty *t) {
    if (t) t->canon_len = 0;
}

// A finished line becomes readable IN ONE PIECE. Pushed byte by byte
// because the queue is a byte queue -- a reader asking for fewer bytes
// than the line holds gets the rest on its next read, exactly as a pipe
// behaves and as POSIX requires.
static void deliver_line(struct tty *t) {
    for (unsigned i = 0; i < t->canon_len; i++)
        tty_enqueue(t, (uint8_t)t->canon[i], 0);
    tty_enqueue(t, '\n', 0);
    t->canon_len = 0;
}

void tty_ldisc_input(struct tty *t, uint8_t byte, uint8_t mods) {
    // A COMPOSITOR HOLDS THE KEYBOARD: no discipline at all, which is
    // KDSKBMODE/K_OFF on a Linux VT. See tty.h's tty_set_bypass().
    if (t->bypass) { tty_enqueue(t, byte, mods); return; }

    const struct tty_termios *tio = &t->tio;

    if (tio->lflag & TTY_ISIG) {
        // ORDER MATTERS ONLY IF SOMEBODY CONFIGURES THE SAME BYTE
        // TWICE, which termios permits and nothing here does; INTR
        // first is the arbitrary-but-stated choice.
        if (byte == tio->cc[TTY_VINTR] && signal_char(t, SIGINT))  return;
        if (byte == tio->cc[TTY_VSUSP] && signal_char(t, SIGTSTP)) return;
        // Not consumed -- fall through and deliver it as a byte.
    }

    if (!(tio->lflag & TTY_ICANON)) {
        echo(t, (char)byte);
        tty_enqueue(t, byte, mods);
        return;
    }

    // --- canonical mode ---------------------------------------------
    //
    // The MODIFIER WORD IS DROPPED HERE, and that is correct rather
    // than lossy: a line assembled over several keystrokes has no one
    // set of modifiers to carry, and a program in canonical mode has
    // asked for text rather than for keys. Raw mode above keeps them.

    if (byte == tio->cc[TTY_VERASE]) {
        if (t->canon_len) { t->canon_len--; echo_erase(t); }
        return;
    }
    if (byte == tio->cc[TTY_VKILL]) {
        while (t->canon_len) { t->canon_len--; echo_erase(t); }
        return;
    }
    if (byte == tio->cc[TTY_VEOF]) {
        // END OF INPUT. With text pending it delivers that text WITHOUT
        // a newline -- so a program reading lines gets a final partial
        // one rather than losing it -- and on an empty line it sets the
        // flag that makes the next read report zero bytes.
        //
        // NOT ECHOED, on purpose: there is no character to show, and a
        // terminal that painted a ^D would put it in the transcript of
        // every program that ended this way.
        if (t->canon_len) {
            for (unsigned i = 0; i < t->canon_len; i++)
                tty_enqueue(t, (uint8_t)t->canon[i], 0);
            t->canon_len = 0;
        } else {
            t->eof_pending = 1;
            // Wake anyway: a reader parked on an empty terminal has to
            // be told, and end of input is exactly the thing it cannot
            // discover by looking.
            tty_enqueue_wake(t);
        }
        return;
    }
    if (byte == '\n' || byte == '\r') {
        // BOTH END THE LINE, and what is stored is always '\n'. POSIX
        // spells this ICRNL and makes it optional; there is no iflag
        // here and no terminal on this machine sends a bare CR, so the
        // translation is unconditional and the flag it would need is
        // not invented for one caller.
        echo(t, '\n');
        deliver_line(t);
        return;
    }

    if (t->canon_len >= TTY_CANON_MAX) {
        // FULL. Discard rather than truncate: Linux's N_TTY does the
        // same, and the alternative -- delivering a partial line as if
        // it had ended -- would hand a program half a command with no
        // way to know it was half. Nothing is echoed either, so the
        // line visibly stops growing.
        return;
    }
    t->canon[t->canon_len++] = (char)byte;
    echo(t, (char)byte);
}

// --- the reader's side -----------------------------------------------

int tty_eof_pending(struct tty *t) {
    if (!t || !t->used || !t->eof_pending) return 0;
    // CONSUMED BY ONE READ. A second Ctrl-D is needed to end input
    // twice, exactly as on a real terminal -- a sticky flag would make
    // every later read on this terminal report end of input forever.
    t->eof_pending = 0;
    return 1;
}

int tty_readable(const struct tty *t) {
    if (!t || !t->used) return 0;
    return t->in_head != t->in_tail;
}

static int dequeue(struct tty *t, uint32_t *out) {
    if (t->in_tail == t->in_head) return 0;
    *out = t->inq[t->in_tail];
    t->in_tail = (t->in_tail + 1) % TTY_INQ_MAX;
    return 1;
}

unsigned tty_read(struct tty *t, char *dst, unsigned len) {
    if (!t || !t->used || !dst) return 0;
    unsigned got = 0;
    uint32_t ev;
    while (got < len && dequeue(t, &ev)) dst[got++] = (char)(ev & 0xFF);
    return got;
}

int tty_read_key(struct tty *t, uint8_t *out_mods) {
    if (!t || !t->used) return -1;
    uint32_t ev;
    if (!dequeue(t, &ev)) return -1;
    if (out_mods) *out_mods = (uint8_t)(ev >> 16);
    // EVERY CODE THIS TERMINAL CARRIES FITS IN A BYTE, specials
    // included (KEY_ARROW_* and friends are 0x91-0xA6) -- which is the
    // same fact SYS_READ's fd-0 contract already states, and is what
    // lets a terminal be a byte stream at all. What this adds over
    // tty_read() is the MODIFIER word, which a byte cannot express and
    // which the ring-0 GUI readers need to tell Shift-Tab from Tab.
    return (int)(ev & 0xFF);
}
