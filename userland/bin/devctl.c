// devctl -- list the machine's devices, show one, disable or enable one,
// and write the hardware report.
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
#include "lib/uargs.h"
#include "lib/udevice.h"

static struct udevice g_dev[UDEV_MAX];
static struct udev_prop g_props[UDEV_PROPS_MAX];
static int g_persist, g_addresses;

static const struct uargs_opt OPTS[] = {
    { "persist",   'p', 0, "with disable: keep it disabled after a restart", &g_persist, 0 },
    { "addresses", 'a', 0, "with report: include MAC and IP addresses", &g_addresses, 0 },
    { 0 }
};

static const struct uargs_cmd CMDS[] = {
    { "list",    0,    "every device, grouped by type (the default)" },
    { "show",    "ID", "everything known about one device" },
    { "events",  "[ID]", "what happened to the devices this boot, or to one" },
    { "report",  0,    "the hardware report: every device and its properties" },
    { "disable", "ID", "unbind its driver (-p: and keep it so after a restart)" },
    { "enable",  "ID", "bind its driver again, and forget any -p" },
    { "apply",   0,    "what the `devices` service runs at boot" },
    { 0 }
};

static const struct uargs_prog PROG = {
    .name = "devctl",
    .usage = "[list] | show ID | events [ID] | report [-a]\n"
             "disable [-p] ID | enable ID | apply",
    .summary = "The machine's devices and their drivers -- the Device Manager's\n"
               "command-line half.",
    .opts = OPTS,
    .cmds = CMDS,
    .notes = "An ID is stable across boots: pci:00:1b.0, usb:2:2357:0601, blk:ahci0.\n"
             "Spawn devctl; never `run` it -- an unbind needs a scheduler slot.",
};

static const char *why(int err) {
    switch (err) {
    case EBUSY:   return "a process holds it -- close that first";
    case ENOTSUP: return "its driver does not support disabling";
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

static int find(const char *id, int *n) {
    *n = udevice_list(g_dev, UDEV_MAX);
    for (int i = 0; i < *n; i++)
        if (!strcmp(g_dev[i].id, id)) return i;
    fprintf(stderr, "devctl: no device %s -- `devctl list` names them\n", id);
    return -1;
}

static int show(const char *id) {
    int n, i = find(id, &n);
    if (i < 0) return 1;
    static char text[8192];
    int np = udevice_props(g_dev, n, i, UDEV_PROPS_ALL, g_props, UDEV_PROPS_MAX);
    char title[160], st[64];
    snprintf(title, sizeof title, "%s [%s] -- %s", g_dev[i].name, g_dev[i].id,
             udevice_status(&g_dev[i], st, sizeof st));
    if (udevice_props_text(g_props, np, title, text, sizeof text) < 0) {
        fprintf(stderr, "devctl: too much to print for %s\n", id);
        return 1;
    }
    fputs(text, stdout);
    return 0;
}

static int events(const char *id) {
    struct query_devevent e;
    printf("%-10s %-20s %-11s %-12s %s\n", "TIME", "DEVICE", "WHAT", "DRIVER", "DETAIL");
    QUERY_FOREACH(QUERY_DEVEVENT, e, i) {
        if (id && strcmp(e.device_id, id) != 0) continue;
        char when[16];
        snprintf(when, sizeof when, "%llu.%02llu", (unsigned long long)e.uptime_ms / 1000,
                 (unsigned long long)(e.uptime_ms % 1000) / 10);
        printf("%-10s %-20s %-11s %-12s %s\n", when, e.device_id, udevice_event_name(e.kind),
               e.driver[0] ? e.driver : "-", e.text);
    }
    return 0;
}

static void put_line(void *ctx, const char *line) { (void)ctx; puts(line); }

static int report(void) {
    int n = udevice_list(g_dev, UDEV_MAX);
    udevice_report(g_dev, n, UDEV_PROPS_RESOURCES | UDEV_PROPS_EVENTS |
                   (g_addresses ? UDEV_PROPS_ADDRESSES : 0), put_line, 0);
    return 0;
}

// Boot (the `devices` service): every present device the persisted
// file names, when its ids still match (udevice.c checks), goes off
// again. Each outcome is logged -- a boot service's only witness.
static int apply(void) {
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

static int toggle(int disable, const char *id) {
    int n, i = find(id, &n);
    if (i < 0) return 1;
    struct udevice *d = &g_dev[i];
    int r = disable ? udevice_disable(d, g_persist) : udevice_enable(d);
    if (r < 0) {
        fprintf(stderr, "devctl: %s %s: %s\n", disable ? "disable" : "enable", d->id, why(-r));
        return 1;
    }
    printf("devctl: %s %s (%s)%s\n", disable ? "disabled" : "enabled", d->id, d->name,
           disable && g_persist ? ", and it stays disabled after a restart" : "");
    return 0;
}

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    const char *cmd = a.argc ? a.argv[0] : "list";
    int nargs = a.argc ? a.argc - 1 : 0;
    char **args = a.argv + 1;
    // uargs stops short options at the command, so `disable -p ID` -- the
    // form the docs and the service file have always used -- arrives here.
    if (!strcmp(cmd, "disable") && nargs && !strcmp(args[0], "-p")) {
        g_persist = 1;
        args++;
        nargs--;
    }
    const char *arg = nargs ? args[0] : 0;

    if (!strcmp(cmd, "list") && nargs == 0) return list();
    if (!strcmp(cmd, "show") && nargs == 1) return show(arg);
    if (!strcmp(cmd, "events") && nargs <= 1) return events(arg);
    if (!strcmp(cmd, "report") && nargs == 0) return report();
    if (!strcmp(cmd, "apply") && nargs == 0) return apply();
    if (!strcmp(cmd, "disable") && nargs == 1) return toggle(1, arg);
    if (!strcmp(cmd, "enable") && nargs == 1) return toggle(0, arg);
    return uargs_error(&PROG, "'%s' takes %s", cmd,
                       !strcmp(cmd, "events") ? "at most one ID" :
                       !strcmp(cmd, "show") || !strcmp(cmd, "disable") || !strcmp(cmd, "enable")
                           ? "one ID" : "no arguments");
}
