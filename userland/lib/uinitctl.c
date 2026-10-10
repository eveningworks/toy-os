// Asking init to restart or power off the machine -- see uinitctl.h.
#include <string.h>
#include "lib/uinitctl.h"
#include "lib/uchan.h"
#include "rt/sys.h"

// init answers before it starts stopping anything, so this is a round
// trip, not the shutdown; a slow answer means a wedged init, and the
// fallback below is for exactly that.
#define REPLY_MS 2000

int uinitctl_shutdown(int reboot) {
    struct uchan_client c;
    if (uchan_client_open(&c, INITCTL_SERVICE) == 0) {
        struct initctl_msg m, reply;
        memset(&m, 0, sizeof m);
        memset(&reply, 0, sizeof reply);
        m.verb = reboot ? INITCTL_REBOOT : INITCTL_POWEROFF;
        int ok = uchan_call(&c, &m, sizeof m, &reply, sizeof reply, REPLY_MS) == 0 &&
                 reply.result == INITCTL_OK;
        uchan_client_close(&c);
        if (ok) return 0;
    }
    sys_eprint("uinitctl: init did not take the shutdown -- stopping the machine directly\n");
    sys_poweroff(reboot);
    return -1;
}
