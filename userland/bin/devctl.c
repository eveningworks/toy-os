// devctl -- list the machine's devices, and disable or enable one.
//
// The command-line half of the Device Manager: both are lib/udevice.c,
// so what this prints and what the window shows cannot disagree.
// Disabling is an unbind (udevice.h says how, and what "disabled" means);
// `apply` is what the `devices` service runs at boot to re-disable what
// /etc/devices.conf lists.
//
// SPAWN it, never `run`: an unbind needs a scheduler slot.
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "rt/sys.h"
#include "lib/cmd.h"
#include "lib/udevice.h"

#define USAGE "devctl [list] | devctl disable [-p] ID | devctl enable ID | devctl apply"

static struct udevice g_dev[UDEV_MAX];

static const char *why(int err) {
    switch (err) {
    case EBUSY:   return "a process holds it -- close that first";
    case ENOTSUP: return "its driver cannot let go of it";
    case EPERM:   return "needs a scheduler slot -- spawn devctl, do not `run` it";
    case EINVAL:  return "the kernel does not know that device";
    default:      return strerror(err);
    }
}

static int list(void) {
    int n = udevice_list(g_dev, UDEV_MAX);
    printf("%-20s %-12s %-10s %-10s %s\n", "ID", "TYPE", "DRIVER", "STATE", "NAME");
    for (int t = 0; t < UDEV_T_COUNT; t++)
        for (int i = 0; i < n; i++) {
            struct udevice *d = &g_dev[i];
            if ((int)d->type != t) continue;
            const char *state = d->holder_pid ? "held" : d->disabled ? "disabled"
                              : d->problem ? "no-driver" : "ok";
            printf("%-20s %-12.12s %-10s %-10s %s\n", d->id, udevice_type_name(d->type),
                   d->driver[0] ? d->driver : "-", state, d->name);
        }
    return 0;
}

static struct udevice *find(const char *id) {
    int n = udevice_list(g_dev, UDEV_MAX);
    for (int i = 0; i < n; i++)
        if (!strcmp(g_dev[i].id, id)) return &g_dev[i];
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2 || !strcmp(argv[1], "list")) return list();

    // Boot (the `devices` service): every present device the persisted
    // file names, when its ids still match (udevice.c checks), goes off
    // again. Each outcome is logged -- a boot service's only witness.
    if (!strcmp(argv[1], "apply") && argc == 2) {
        int n = udevice_list(g_dev, UDEV_MAX), bad = 0;
        for (int i = 0; i < n; i++) {
            struct udevice *d = &g_dev[i];
            if (!d->persisted || !d->driver[0]) continue;   // not asked for, or already off
            int r = udevice_disable(d, 1);
            if (r < 0) { bad++; fprintf(stderr, "devctl: apply %s: %s\n", d->id, why(-r)); }
            else printf("devctl: disabled %s (%s)\n", d->id, d->name);
        }
        return bad ? 1 : 0;
    }

    int persist = 0, at = 2;
    int disable = !strcmp(argv[1], "disable"), enable = !strcmp(argv[1], "enable");
    if (disable && argc > at && !strcmp(argv[at], "-p")) { persist = 1; at++; }
    if ((!disable && !enable) || argc != at + 1) { cmd_usage(USAGE); return 1; }

    struct udevice *d = find(argv[at]);
    if (!d) {
        fprintf(stderr, "devctl: no device %s -- `devctl list` names them\n", argv[at]);
        return 1;
    }
    int r = disable ? udevice_disable(d, persist) : udevice_enable(d);
    if (r < 0) {
        fprintf(stderr, "devctl: %s %s: %s\n", argv[1], d->id, why(-r));
        return 1;
    }
    printf("devctl: %s %s (%s)%s\n", disable ? "disabled" : "enabled", d->id, d->name,
           persist ? ", and it stays disabled after a restart" : "");
    return 0;
}
