#ifndef ULIB_GETOPT_H
#define ULIB_GETOPT_H

// getopt() IS DECLARED IN <unistd.h>, WHICH IS WHERE POSIX PUTS IT.
// This header exists because glibc also ships one, and ported code
// includes whichever it saw first -- dash's histedit.c takes this one.
//
// Deliberately NOT a second declaration: it includes the real header,
// so the two can never drift. There is no getopt_long here, because
// there is no getopt_long in this library.

#include <unistd.h>

#endif
