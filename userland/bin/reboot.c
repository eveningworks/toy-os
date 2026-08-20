// reboot -- restart the machine. `reboot --poweroff` shuts it down.
//
// SYS_POWEROFF takes the choice as an argument, so one program serves
// both rather than two programs differing by a constant -- and the
// destructive one is not the default: a bare `reboot` reboots, and
// powering off has to be asked for by name.
//
// IT DOES NOT SYNC, AND THAT IS NOT AN OVERSIGHT. The kernel flushes
// the disk cache on its way down (see ata_cache.c and the `sync`
// command's note), so a sync here would be a second place that has to
// remember, and the one that gets forgotten is the kernel's -- which is
// also the one that matters for a crash or a power cut. If the flush
// fails the kernel says so loudly there.
#include "rt/sys.h"
#include "lib/cmd.h"
#include "lib/string.h"

int main(int argc, char **argv) {
    int poweroff = 0;
    if (argc > 1) {
        if (strcmp(argv[1], "--poweroff") == 0) poweroff = 1;
        else {
            cmd_usage("reboot [--poweroff]");
            return 1;
        }
    }
    sys_poweroff(!poweroff);
    // Only reached if the call failed -- it does not return on success.
    cmd_fail("reboot", 0);
    return 1;
}
