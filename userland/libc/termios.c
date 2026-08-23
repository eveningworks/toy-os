// <termios.h>: POSIX's struct converted to the TTY layer's.
//
// The conversion is the substance. Everything the kernel implements
// lives in `c_lflag` and `c_cc`; the other three flag words are the
// caller's own memory and never leave this file -- which means a
// tcgetattr() cannot report back what a tcsetattr() was given for them.
// Stated in the header rather than hidden: they are ignored, and a
// round trip through the terminal is where that becomes visible.
#include <termios.h>
#include <string.h>
#include "rt/sys.h"

int tcgetattr(int fd, struct termios *t) {
    struct tty_termios k;
    if (!t) return -1;
    if (sys_tcgetattr(fd, &k) < 0) return -1;

    memset(t, 0, sizeof *t);
    t->c_lflag = k.lflag;
    for (int i = 0; i < NCCS; i++) t->c_cc[i] = k.cc[i];
    return 0;
}

int tcsetattr(int fd, int when, const struct termios *t) {
    struct tty_termios k;
    if (!t) return -1;
    (void)when;   // see <termios.h>: nothing to drain, nothing to flush

    // MASKED TO WHAT THE TERMINAL IMPLEMENTS. Passing the caller's
    // whole c_lflag through would hand the kernel the inert bits this
    // header defines (ECHOE, IEXTEN, ...) as if they were real ones,
    // and tty_abi.h's flag space is not reserved above ISIG -- a future
    // TTY_* bit would land inside them and start being set by every
    // program that ever cleared IEXTEN.
    k.lflag = t->c_lflag & (ICANON | ECHO | ISIG);
    for (int i = 0; i < NCCS; i++) k.cc[i] = t->c_cc[i];
    return sys_tcsetattr(fd, &k);
}

void cfmakeraw(struct termios *t) {
    if (!t) return;
    t->c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG);
    // The input/output flags are cleared too, for a caller that later
    // inspects its own struct -- they are inert at the terminal, but a
    // cfmakeraw() that left ONLCR set in the caller's copy would be
    // lying about what it did.
    t->c_iflag = 0;
    t->c_oflag = 0;
}

int tcgetwinsize(int fd, struct winsize *ws) {
    struct tty_winsize k;
    if (!ws) return -1;
    if (sys_tcgetwinsz(fd, &k) < 0) return -1;
    ws->ws_row = k.rows;
    ws->ws_col = k.cols;
    return 0;
}

int tcsetwinsize(int fd, const struct winsize *ws) {
    struct tty_winsize k;
    if (!ws) return -1;
    k.rows = ws->ws_row;
    k.cols = ws->ws_col;
    return sys_tcsetwinsz(fd, &k);
}
