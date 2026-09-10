// modload -- load a kernel module (insmod, sized for here).
//
// A bare name is /lib/modules/<name>.ko; a path with a slash is used as
// given. The kernel says WHY a load was refused in its log, since a
// load fails a dozen ways and "unknown symbol scheduler_kill" is worth
// more than an errno -- so a failure here points at `dmesg`.
#include "rt/sys.h"
#include "lib/cmd.h"
#include <stdio.h>
#include <string.h>

#define USAGE "modload <name | path>"

int main(int argc, char **argv) {
    if (argc != 2 || argv[1][0] == '-') { cmd_usage(USAGE); return 1; }

    char path[64];
    if (strchr(argv[1], '/')) {
        snprintf(path, sizeof path, "%s", argv[1]);
    } else if (snprintf(path, sizeof path, "/lib/modules/%s.ko", argv[1]) >= (int)sizeof path) {
        cmd_fail_err("modload", argv[1], ENAMETOOLONG);
        return 1;
    }

    if (sys_modload(path) < 0) {
        int e = sys_errno();
        cmd_fail_err("modload", path, e);
        if (e != ENOENT) fprintf(stderr, "modload: the kernel log says why -- `dmesg`\n");
        return 1;
    }
    return 0;
}
