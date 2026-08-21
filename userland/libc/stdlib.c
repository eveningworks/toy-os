// exit(), atexit() and abort() -- the part of <stdlib.h> that has to
// exist the moment stdio does.
//
// WHY THIS IS NOT STAGE 3's PROBLEM (docs/libc-design.md lists atexit
// there): buffering means output written is not output emitted, and the
// one thing that guarantees a program's last printf() reaches the
// screen is exit() flushing on the way out. A stdio that shipped
// without it would lose the final line of every program that does not
// end in a newline on a line-buffered stream -- which is a bug report
// about printf, not about atexit.
//
// crt0.asm calls exit() rather than sys_exit() for exactly this reason,
// and that ONE line is the whole hook. A program that calls sys_exit()
// itself still bypasses everything here; <stdio.h> says so.
#include <stdlib.h>
#include <stdio.h>
#include "rt/sys.h"

// C requires at least 32. A fixed array rather than a malloc'd list
// because the handlers must be runnable when the heap is in whatever
// state made the program exit.
#define ATEXIT_MAX 32

static void (*g_handlers[ATEXIT_MAX])(void);
static int g_count;
static int g_exiting; // exit() called from inside a handler: run no more

int atexit(void (*fn)(void)) {
    if (!fn || g_count >= ATEXIT_MAX) return -1;
    g_handlers[g_count++] = fn;
    return 0;
}

void exit(int code) {
    // LIFO, which is what C specifies and what makes handlers compose:
    // a handler registered later can rely on what an earlier one set up
    // still being there.
    //
    // The guard is not paranoia. A handler that calls exit() re-enters
    // this function, and without it the loop would run every handler
    // again from the top -- including the one that is mid-call.
    if (!g_exiting) {
        g_exiting = 1;
        while (g_count > 0) g_handlers[--g_count]();
    }
    // AFTER the handlers, because a handler is entitled to print.
    fflush(0);
    sys_exit(code);
}

void abort(void) {
    // stderr is unbuffered, so anything already reported is already
    // out; the buffered streams are deliberately NOT flushed, because
    // abort() means the program's state is not to be trusted and
    // committing a half-written file is a worse outcome than losing it.
    // C leaves this implementation-defined and both mainstream libcs
    // choose the same way.
    //
    // 134 is 128 + SIGABRT, the shell convention for "died on signal 6"
    // -- borrowed as a recognisable number even though this OS has no
    // signals yet (docs/signals-design.md).
    sys_exit(134);
}
