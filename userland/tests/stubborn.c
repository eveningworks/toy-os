// A service that IGNORES SIGTERM -- the shutdown's fixture.
//
// init stops each service at shutdown with SIGTERM and, once its
// StopTimeout= runs out, SIGKILL (userland/bin/init.c). Every real
// service dies of the first, so without this the second is a branch no
// boot reaches: an init whose timeout never fired would hang every
// shutdown behind the first stuck service and look perfect until then.
// tools/shutdown_test.py adds it as a service and reboots.
//
// It sleeps, as notready does, so leaving it running costs nothing.
#include "rt/sys.h"

int main(void) {
    sys_signal(SIGTERM, (sighandler_t)SIG_IGN);
    sys_eprint("stubborn: started, ignoring SIGTERM\n");
    for (;;) sys_sleep_ms(1000);
}
