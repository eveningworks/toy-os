// modload -- load a kernel module (insmod, sized for here).
//
// A bare name is /lib/modules/<name>.ko; a path with a slash is used as
// given. The kernel says WHY a load was refused in its log, since a
// load fails a dozen ways and "unknown symbol scheduler_kill" is worth
// more than an errno -- so a failure here points at `dmesg`.
//
// `-r` RELOADS in one process: unload if loaded, then load. It exists
// because the natural `modunload x; modload x` cannot be typed over the
// network into a machine whose only NIC is the module -- the session
// dies with the unload and the second half never arrives. One process
// keeps going.
#include "rt/sys.h"
#include "lib/cmd.h"
#include <stdio.h>
#include <string.h>

#define USAGE "modload [-r] <name | path>"

// The module's name as `lsmod` shows it: the basename without .ko.
static void name_of(const char *arg, char *out, unsigned cap) {
    const char *b = strrchr(arg, '/');
    b = b ? b + 1 : arg;
    unsigned n = (unsigned)strlen(b);
    if (n > 3 && !strcmp(b + n - 3, ".ko")) n -= 3;
    if (n >= cap) n = cap - 1;
    memcpy(out, b, n);
    out[n] = '\0';
}

int main(int argc, char **argv) {
    int reload = 0, ai = 1;
    if (argc >= 2 && !strcmp(argv[1], "-r")) { reload = 1; ai = 2; }
    if (argc != ai + 1 || argv[ai][0] == '-') { cmd_usage(USAGE); return 1; }

    char path[64];
    if (strchr(argv[ai], '/')) {
        snprintf(path, sizeof path, "%s", argv[ai]);
    } else if (snprintf(path, sizeof path, "/lib/modules/%s.ko", argv[ai]) >= (int)sizeof path) {
        cmd_fail_err("modload", argv[ai], ENAMETOOLONG);
        return 1;
    }

    if (reload) {
        char name[16];
        name_of(argv[ai], name, sizeof name);
        if (sys_modunload(name) < 0 && sys_errno() != ENOENT) {
            cmd_fail_err("modload", name, sys_errno());
            return 1;
        }
    }

    if (sys_modload(path) < 0) {
        int e = sys_errno();
        cmd_fail_err("modload", path, e);
        if (e != ENOENT) fprintf(stderr, "modload: the kernel log says why -- `dmesg`\n");
        return 1;
    }
    return 0;
}
