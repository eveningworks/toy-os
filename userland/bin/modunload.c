// modunload -- unload a kernel module by name (rmmod, sized for here).
//
// Refused with "in use" when one of the module's drivers holds a device
// it cannot release -- a driver without a remove() keeps its device
// until reboot. A driver that CAN let go (e1000) is removed first, and
// the device is free for a `modload` to claim again.
#include "rt/sys.h"
#include "lib/cmd.h"
#include <stdio.h>
#include <string.h>

#define USAGE "modunload <name>"

int main(int argc, char **argv) {
    if (argc != 2 || argv[1][0] == '-') { cmd_usage(USAGE); return 1; }
    if (sys_modunload(argv[1]) < 0) {
        int e = sys_errno();
        cmd_fail_err("modunload", argv[1], e);
        if (e == EBUSY)
            fprintf(stderr, "modunload: a driver in it holds a device and has no remove()\n");
        return 1;
    }
    return 0;
}
