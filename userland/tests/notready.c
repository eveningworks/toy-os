// A service that stays alive and NEVER announces itself.
//
// WHY THIS EXISTS
// ---------------
// init's readiness barrier (`Ready=notify`, userland/bin/init.c) has two
// exits: the service calls SYS_NOTIFY_READY, or the wait runs out. The
// desktop covers the first one on every graphical boot. Nothing covered
// the second, and the second is the one that matters -- it is the reason
// a descriptor cannot leave the machine with nothing started, so a
// version of init whose timeout silently did not fire would look
// perfectly healthy right up until a service hung.
//
// Every other long-lived binary here is unusable as that fixture:
// hangclient needs the compositor, catin and pipedrain wait on a console
// that never reaches EOF, spin_test burns a core, and everything else
// exits (which releases the barrier through a DIFFERENT path -- the
// "it can never answer now" rule -- and so proves nothing about the
// timeout).
//
// HOW IT WAITS IS THE WHOLE POINT. It sleeps rather than spinning, so a
// test that leaves it running for the rest of a boot costs nothing and
// cannot skew the timings of whatever it is being measured against.
#include "rt/sys.h"

int main(void) {
    sys_eprint("notready: started, and saying nothing else\n");
    // Not a single long sleep: SYS_SLEEP is capped at SYS_SLEEP_MAX_MS,
    // and a refused sleep would turn this into the busy loop it exists
    // not to be.
    for (;;) sys_sleep_ms(1000);
}
