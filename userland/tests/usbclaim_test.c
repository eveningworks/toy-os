// The USB claim, checked against whatever audio device is attached.
//
// IT NEEDS A REAL ONE and says so rather than passing vacuously: the
// whole point is that claiming takes a device away from a class driver
// that had it, and a machine with no USB audio has nothing to take.
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "rt/sys.h"
#include "query_abi.h"
#include "syscall_abi.h"
#include "lib/utest.h"

int main(void) {
    utest_begin("usbclaim_test",
                "claiming a USB device takes it off its class driver", 0);

    // The device, and whether a class driver has it -- both from
    // QUERY_USB, which ring 3 could already read.
    struct query_usb q;
    int slot = -1, was_bound = 0;
    QUERY_FOREACH(QUERY_USB, q, i) {
        if (q.if_class != 1) continue;          // 1 = Audio
        slot = (int)q.slot;
        was_bound = (int)q.bound;
        break;
    }
    if (slot < 0) {
        printf("usbclaim_test: SKIP -- no USB audio device attached\n");
        return utest_end();
    }
    utest_check(was_bound, "a class driver had the device before the claim");

    utest_check(sys_usb_claim(slot) == 0, "claimed it");
    utest_check(sys_usb_claim(slot) == 0, "...and claiming it again is idempotent");

    // THE UNBIND IS THE POINT, so it is asserted rather than assumed:
    // `bound` going false is the class driver having let go.
    int still_bound = 1;
    QUERY_FOREACH(QUERY_USB, q, i)
        if ((int)q.slot == slot) still_bound = (int)q.bound;
    utest_check(!still_bound, "...and the class driver let go of it");

    utest_check(sys_usb_release(slot, USB_RELEASE_REBIND) == 0,
                "released it with a rebind");
    int rebound = 0;
    QUERY_FOREACH(QUERY_USB, q, i)
        if ((int)q.slot == slot) rebound = (int)q.bound;
    utest_check(rebound, "...and the class driver took it back");

    utest_check(sys_usb_release(slot, 0) < 0 && sys_errno() == EACCES,
                "releasing one nobody holds is refused");
    utest_check(sys_usb_claim(200) < 0 && sys_errno() == EINVAL,
                "a slot that does not exist is refused");

    return utest_end();
}
