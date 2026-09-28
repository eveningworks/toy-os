// The device list, and disable/enable over claim and release
// (lib/udevice.c -- what /bin/devctl and the Device Manager share).
//
// The storage check is the SAFETY property: the controller the root
// filesystem is on must refuse to be disabled, whatever asks. The
// network card is the round trip: disabled with persistence, seen as
// disabled and unbound, recorded in /etc/devices.conf -- then enabled,
// seen bound again, and forgotten. A card whose ids do not match its
// slot is left alone by `apply`; that half is checked by hand (a
// reordered guest), since a test cannot move a card.
//
// SPAWNED: an unbind needs a scheduler slot.
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "rt/sys.h"
#include "lib/uconf.h"
#include "lib/udevice.h"
#include "lib/uhwids.h"
#include "lib/utest.h"

static struct udevice g_dev[UDEV_MAX];

static struct udevice *by_id(const char *id) {
    int n = udevice_list(g_dev, UDEV_MAX);
    for (int i = 0; i < n; i++)
        if (!strcmp(g_dev[i].id, id)) return &g_dev[i];
    return 0;
}

int main(void) {
    utest_begin("udevice_test", "the device list, and disable/enable", UTEST_VERDICT_FILE);

    int n = udevice_list(g_dev, UDEV_MAX), pci = 0, named = 0;
    struct udevice *store = 0, *nic = 0;
    for (int i = 0; i < n; i++) {
        struct udevice *d = &g_dev[i];
        if (d->bus == UDEV_PCI) pci++;
        if (d->vendor_name[0]) named++;
        if (d->type == UDEV_T_STORAGE && d->driver[0] && !store) store = d;
        if (d->type == UDEV_T_NETWORK && d->driver[0] && d->can_disable && !nic) nic = d;
    }
    utest_checkf(pci > 0, "PCI devices are listed (%d devices in all)", n);
    utest_checkf(named > 0, "a device is named from the hwdata databases");

    // Each USB device named from usb.ids by ITS OWN ids -- resolved here
    // one at a time, so a list that mixed up its lookups cannot agree.
    int usb = 0, usb_ok = 0;
    for (int i = 0; i < n; i++) {
        if (g_dev[i].bus != UDEV_USB) continue;
        struct uhwids_entry e = { .vendor = g_dev[i].vendor, .device = g_dev[i].device,
                                  .cls = -1, .subclass = -1 };
        uhwids_resolve(UHWIDS_USB, &e, 1);
        usb++;
        if (!e.vendor_name[0] || !strcmp(e.vendor_name, g_dev[i].vendor_name)) usb_ok++;
        else printf("udevice_test: %s is \"%s\", usb.ids says \"%s\"\n",
                    g_dev[i].id, g_dev[i].vendor_name, e.vendor_name);
    }
    if (usb) utest_checkf(usb_ok == usb, "%d of %d USB devices carry their own usb.ids vendor", usb_ok, usb);

    utest_checkf(store != 0, "a storage controller with a driver is listed");
    if (store) {
        utest_checkf(!store->can_disable, "%s (%s) is not offered for disable", store->id, store->driver);
        int r = udevice_disable(store, 0);
        utest_checkf(r == -ENOTSUP, "disabling %s is refused with -ENOTSUP (gave %d)", store->id, r);
    }

    if (!nic) {
        printf("udevice_test: no network card that can be disabled -- round trip skipped\n");
        return utest_end();
    }
    char id[24], drv[16], val[16];
    strlcpy(id, nic->id, sizeof id);
    strlcpy(drv, nic->driver, sizeof drv);

    utest_checkf(udevice_disable(nic, 1) == 0, "disable -p %s", id);
    struct udevice *d = by_id(id);
    utest_checkf(d && !d->driver[0] && d->disabled && d->persisted,
                 "after disable: unbound, disabled and persisted (driver \"%s\" disabled %d persisted %d)",
                 d ? d->driver : "?", d ? d->disabled : -1, d ? d->persisted : -1);
    utest_checkf(uconf_get_in(UDEV_PERSIST_CONF, "pci", id + 4, val, sizeof val),
                 "%s is recorded in %s", id, UDEV_PERSIST_CONF);

    utest_checkf(d && udevice_enable(d) == 0, "enable %s", id);
    d = by_id(id);
    utest_checkf(d && !strcmp(d->driver, drv) && !d->disabled && !d->persisted,
                 "after enable: bound again and forgotten (driver \"%s\", wanted %s; disabled %d persisted %d)",
                 d ? d->driver : "?", drv, d ? d->disabled : -1, d ? d->persisted : -1);
    utest_checkf(!uconf_get_in(UDEV_PERSIST_CONF, "pci", id + 4, val, sizeof val),
                 "%s is gone from %s", id, UDEV_PERSIST_CONF);
    return utest_end();
}
