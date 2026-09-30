// reboot -- restart the machine. `reboot --poweroff` shuts it down;
// `reboot --entry <name|number>` restarts into that GRUB entry, once.
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
// fails the kernel says so loudly there. The one-shot entry is written
// through the same cache, so the same flush carries it to GRUB.
//
// --entry IS ONE BOOT, and GRUB is what ends it: see lib/ubootmenu.h.
#include "rt/sys.h"
#include "lib/cmd.h"
#include "lib/ubootmenu.h"
#include <stdio.h>
#include <string.h>

#define USAGE "reboot [--poweroff | --entries | --entry <name|number>]"

static struct ubootmenu g_menu;

static int list(void) {
    if (ubootmenu_read(&g_menu, 0) < 0) {
        printf("reboot: cannot read %s -- no boot menu on this boot\n", UBOOTMENU_CFG);
        return 1;
    }
    for (int i = 0; i < g_menu.count; i++)
        printf("%2d  %s%s%s\n", i, g_menu.title[i],
               i == g_menu.def ? "  (default)" : "",
               i == g_menu.next ? "  (next boot)" : "");
    return 0;
}

int main(int argc, char **argv) {
    int poweroff = 0;
    const char *entry = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--poweroff")) poweroff = 1;
        else if (!strcmp(argv[i], "--entries")) return list();
        else if (!strcmp(argv[i], "--entry") && i + 1 < argc) entry = argv[++i];
        else { cmd_usage(USAGE); return 1; }
    }
    if (poweroff && entry) { cmd_usage(USAGE); return 1; }

    if (entry) {
        if (ubootmenu_read(&g_menu, 0) < 0) {
            printf("reboot: cannot read %s -- no boot menu to choose from\n", UBOOTMENU_CFG);
            return 1;
        }
        if (!g_menu.oneshot) {
            printf("reboot: cannot choose an entry: %s\n", g_menu.why_not);
            return 1;
        }
        int n = ubootmenu_find(&g_menu, entry);
        if (n < 0) {
            printf("reboot: no boot entry \"%s\"; the entries are:\n", entry);
            list();
            return 1;
        }
        // The default needs no choice -- but one left pending by an
        // earlier call is cleared, or it would win instead.
        if (ubootmenu_set_next(n == g_menu.def ? 0 : g_menu.title[n]) < 0) {
            printf("reboot: cannot write %s -- not restarting\n", UBOOTMENU_ENV);
            return 1;
        }
        printf("reboot: next boot: %s\n", g_menu.title[n]);
    }

    sys_poweroff(!poweroff);
    // Only reached if the call failed -- it does not return on success.
    // A choice made above would outlive this failure and surprise the
    // next ordinary restart, so it is taken back.
    if (entry) ubootmenu_set_next(0);
    cmd_fail("reboot", 0);
    return 1;
}
