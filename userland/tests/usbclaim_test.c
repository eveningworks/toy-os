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
    unsigned want_vid = 0, want_pid = 0;
    QUERY_FOREACH(QUERY_USB, q, i) {
        if (q.if_class != 1) continue;          // 1 = Audio
        slot = (int)q.slot;
        was_bound = (int)q.bound;
        want_vid = (unsigned)q.vendor_id;
        want_pid = (unsigned)q.product_id;
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

    // A CONTROL TRANSFER FROM RING 3, and the device descriptor is the
    // one request whose answer this test can CHECK rather than merely
    // receive: QUERY_USB already reported the vendor and product, so
    // the two have to agree.
    {
        uint8_t desc[18];
        memset(desc, 0, sizeof desc);
        // GET_DESCRIPTOR(DEVICE): 0x80 in, request 6, wValue 0x0100.
        uint8_t setup[8] = { 0x80, 6, 0x00, 0x01, 0, 0, 18, 0 };
        int n = sys_usb_control(slot, setup, desc, sizeof desc, 1);
        utest_check(n == (int)sizeof desc, "a control transfer from ring 3");
        unsigned vid = (unsigned)desc[8] | ((unsigned)desc[9] << 8);
        unsigned pid = (unsigned)desc[10] | ((unsigned)desc[11] << 8);
        utest_check(desc[0] == 18 && desc[1] == 1,
                    "...and it IS a device descriptor, not a zeroed buffer");
        utest_check(vid == want_vid && pid == want_pid,
                    "...and its vendor:product match what QUERY_USB said");
    }

    // The two that would desync the kernel from the controller.
    {
        uint8_t set_addr[8] = { 0x00, 5, 1, 0, 0, 0, 0, 0 };
        utest_check(sys_usb_control(slot, set_addr, 0, 0, 0) < 0 &&
                    sys_errno() == EINVAL, "SET_ADDRESS is refused");
        uint8_t set_cfg[8] = { 0x00, 9, 1, 0, 0, 0, 0, 0 };
        utest_check(sys_usb_control(slot, set_cfg, 0, 0, 0) < 0 &&
                    sys_errno() == EINVAL, "SET_CONFIGURATION is refused");
    }

    utest_check(sys_usb_release(slot, USB_RELEASE_REBIND) == 0,
                "released it with a rebind");
    int rebound = 0;
    QUERY_FOREACH(QUERY_USB, q, i)
        if ((int)q.slot == slot) rebound = (int)q.bound;
    utest_check(rebound, "...and the class driver took it back");

    utest_check(sys_usb_release(slot, 0) < 0 && sys_errno() == EACCES,
                "releasing one nobody holds is refused");
    // NOT THE HOLDER ANY MORE, so the transfer must go too -- a claim
    // that stops gating the syscalls it exists to gate is no claim.
    {
        uint8_t setup[8] = { 0x80, 6, 0x00, 0x01, 0, 0, 18, 0 };
        uint8_t desc[18];
        utest_check(sys_usb_control(slot, setup, desc, sizeof desc, 1) < 0 &&
                    sys_errno() == EACCES,
                    "...and a control transfer without the claim is refused");
    }
    utest_check(sys_usb_claim(200) < 0 && sys_errno() == EINVAL,
                "a slot that does not exist is refused");

    return utest_end();
}
