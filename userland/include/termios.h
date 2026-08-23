#ifndef ULIB_TERMIOS_H
#define ULIB_TERMIOS_H

// POSIX's <termios.h> over the TTY layer's own ABI (abi/tty_abi.h).
//
// WHY A SEPARATE STRUCT rather than a typedef of `struct tty_termios`:
// the kernel's carries the flags it implements, and POSIX's carries
// four flag words and a larger control array. Ported code says
// `t.c_lflag &= ~(ICANON | ECHO)` -- that idiom has to compile, and it
// cannot against a struct whose field is called `lflag`. So this is the
// POSIX shape, converted at the call, the same split <signal.h> makes.
//
// WHAT IS HONOURED, STATED PLAINLY: `c_lflag`'s ICANON, ECHO and ISIG,
// and `c_cc`'s five entries. `c_iflag`, `c_oflag` and `c_cflag` are
// STORED AND IGNORED -- they exist so that a program clearing IXON or
// setting CS8 compiles and runs, which it does, because this terminal
// never did those transformations in the first place. That is different
// from ignoring a request the terminal COULD honour, and the two should
// not be confused: there is no flow control here to disable and no
// character size to choose.
//
// THERE IS NO BAUD RATE, so cfsetispeed/cfsetospeed are absent rather
// than accepted-and-ignored: a program that sets one is talking to a
// serial line and needs to know this is not one.
#include <tty_abi.h>
#include <sys/types.h>

typedef unsigned tcflag_t;
typedef unsigned char cc_t;

// The control characters, at the KERNEL's indices under POSIX's names.
// Not Linux's numbering -- these are private to the pair of headers
// that agree on them, and every program indexes by name.
#define VINTR  TTY_VINTR
#define VERASE TTY_VERASE
#define VKILL  TTY_VKILL
#define VEOF   TTY_VEOF
#define VSUSP  TTY_VSUSP
#define NCCS   TTY_NCCS

// VMIN and VTIME are deliberately NOT defined. A non-canonical read
// here blocks until at least one byte arrives, which is exactly
// VMIN=1/VTIME=0, and there is no machinery for any other pair -- so a
// program setting VMIN=0 for a polling read would get a blocking one.
// Leaving them undefined turns that into a compile error naming the
// exact line, instead of a program that hangs. What it wants is
// set_nonblock() in <unistd.h>.

// c_lflag bits. The three the line discipline actually implements.
#define ICANON TTY_ICANON
#define ECHO   TTY_ECHO
#define ISIG   TTY_ISIG
// Accepted, stored, and inert -- see the header comment. Values chosen
// clear of the three above so that clearing one cannot clear a real one.
#define ECHOE   (1u << 8)
#define ECHOK   (1u << 9)
#define ECHONL  (1u << 10)
#define NOFLSH  (1u << 11)
#define IEXTEN  (1u << 12)
#define TOSTOP  (1u << 13)

// c_iflag / c_oflag / c_cflag bits: names only, so the idioms compile.
#define IXON    (1u << 0)
#define IXOFF   (1u << 1)
#define ICRNL   (1u << 2)
#define INLCR   (1u << 3)
#define ISTRIP  (1u << 4)
#define IGNBRK  (1u << 5)
#define BRKINT  (1u << 6)
#define INPCK   (1u << 7)
#define OPOST   (1u << 0)
#define ONLCR   (1u << 1)
#define CS8     (1u << 0)
#define CREAD   (1u << 1)
#define CLOCAL  (1u << 2)
#define PARENB  (1u << 3)
#define CSIZE   (1u << 4)

struct termios {
    tcflag_t c_iflag;    // stored, ignored
    tcflag_t c_oflag;    // stored, ignored
    tcflag_t c_cflag;    // stored, ignored
    tcflag_t c_lflag;    // ICANON | ECHO | ISIG are real
    cc_t     c_cc[NCCS];
};

// The terminal's size in CHARACTER CELLS -- POSIX's name for what
// abi/tty_abi.h calls tty_winsize. Field names are the ones every
// program uses (ws_row/ws_col), and ws_xpixel/ws_ypixel are absent
// because nothing here answers them: a terminal's size is a grid, and
// the pixel conversion belongs to whoever knows the font.
struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
};

// tcsetattr's `when` argument. ONE VALUE IS HONOURED: the change takes
// effect immediately, because there is no output queue to drain and no
// input queue that is discarded on a mode change. TCSADRAIN and
// TCSAFLUSH are accepted as synonyms rather than refused -- a program
// using them is asking for MORE care than TCSANOW, and getting less
// waiting than asked for cannot corrupt anything here.
#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

int tcgetattr(int fd, struct termios *t);
int tcsetattr(int fd, int when, const struct termios *t);

// Put `t` into the raw mode a full-screen program wants: no line
// assembly, no echo, no signal keys. BSD's cfmakeraw(), which glibc and
// musl both provide, and it is here because otherwise every caller
// writes the same three lines -- /bin/edit and the terminal emulator
// already did.
void cfmakeraw(struct termios *t);

// The window size. TIOCGWINSZ/TIOCSWINSZ on a real system, which is an
// ioctl() this kernel deliberately does not have -- a named call
// instead, since ioctl's whole shape is a dispatch chain over an
// untyped argument.
int tcgetwinsize(int fd, struct winsize *ws);
int tcsetwinsize(int fd, const struct winsize *ws);

#endif
