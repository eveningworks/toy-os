#ifndef ULIB_SYS_IOCTL_H
#define ULIB_SYS_IOCTL_H

// **ioctl() IS NOT THIS SYSTEM'S INTERFACE, AND THIS HEADER DOES NOT
// PRETEND OTHERWISE.** toy-os gives each terminal operation its own
// typed syscall -- SYS_TCGETATTR, SYS_TCGETWINSZ and the rest
// (docs/conventions/shell.md) -- because a single entry point taking an
// untyped pointer and an integer command is exactly the shape that
// cannot be checked at a ring boundary.
//
// What is here is the small part that CAN be honoured: the two window-
// size requests, forwarded to the typed calls. Every other request is
// refused with EINVAL rather than silently returning 0, so a caller
// asking for something this system does not do finds out.

#include <termios.h>

// The two requests that work. Values are Linux's, so a program that
// hardcodes the number rather than the name still reaches the right
// one.
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414

// Variadic to match every other ioctl() in existence; the third
// argument is a `struct winsize *` for both requests above. Returns 0,
// or -1 with errno -- EINVAL for an unrecognised request, ENOTTY for a
// descriptor that is not a terminal.
int ioctl(int fd, unsigned long request, ...);

#endif
