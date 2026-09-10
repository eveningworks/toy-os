#ifndef PCI_DRIVER_H
#define PCI_DRIVER_H

#include <stdint.h>
#include "pci.h"

// PCI BUS MATCHING: a driver declares what it drives and a probe(), and
// the bus calls it once per matching device -- Linux's `pci_driver` +
// id table, NT's PnP, sized for here. A driver no longer walks the
// device table looking for itself, a second controller is a second
// probe() call rather than a silent skip, and "which driver has that
// device" is data (`pci_device_driver()`).
//
// pci_bind() runs as an INITCALL at INIT_BUS (kernel/drivers/pci_bind.c):
// devices in enumeration order, and for each the FIRST driver in link
// order whose table matches. Two drivers matching one device is a
// mistake this cannot see; the KTEST in pci_bind.c looks for it.
#define PCI_ANY 0xFFFF

struct pci_match {
    uint16_t vendor, device;             // or PCI_ANY
    uint16_t class_code, subclass, prog_if; // or PCI_ANY
};

#define PCI_MATCH_ID(v, d)        { (v), (d), PCI_ANY, PCI_ANY, PCI_ANY }
#define PCI_MATCH_CLASS(c, s, p)  { PCI_ANY, PCI_ANY, (c), (s), (p) }

struct pci_driver {
    const char *name;                 // the DRIVER_DECLARE name
    const struct pci_match *matches;
    int nmatches;
    void (*probe)(const struct pci_device *dev);
    // Undoes probe() for one device: quiesce the hardware, unregister
    // from the class, free what probe allocated. Optional; a driver
    // without one holds every device it bound until reboot, and a
    // module carrying such a driver cannot be unloaded while bound.
    void (*remove)(const struct pci_device *dev);
} __attribute__((aligned(32)));

// File scope: `static const struct pci_match hda_matches[] = { ... };
// PCI_DRIVER("hda", hda_matches, hda_probe);`
#define PCI_DRIVER(name_str, table, probe_fn)                                  \
    static const struct pci_driver pci_driver_##probe_fn                      \
        __attribute__((used, section(".pci_drivers"))) = {                    \
            .name = name_str, .matches = table,                                \
            .nmatches = (int)(sizeof(table) / sizeof((table)[0])),             \
            .probe = probe_fn,                                                 \
        }

// The same, for a driver that can also let go of a device.
#define PCI_DRIVER_REMOVABLE(name_str, table, probe_fn, remove_fn)             \
    static const struct pci_driver pci_driver_##probe_fn                      \
        __attribute__((used, section(".pci_drivers"))) = {                    \
            .name = name_str, .matches = table,                                \
            .nmatches = (int)(sizeof(table) / sizeof((table)[0])),             \
            .probe = probe_fn, .remove = remove_fn,                            \
        }

int pci_match_device(const struct pci_match *m, const struct pci_device *d);

int pci_driver_count(void);
const struct pci_driver *pci_driver_at(int i);

// The driver pci_bind() handed this device to, or NULL.
const char *pci_device_driver(const struct pci_device *d);

// --- tables that are not in the image: loadable modules ----------------
//
// pci_bind() walks the image's `.pci_drivers` section and every table
// added here, in that order. A module's loader adds its table and asks
// for a re-bind, which probes every device no driver has claimed yet
// (bound devices are never re-probed). Removing a table first calls
// remove() for each device its drivers hold and REFUSES (-EBUSY) if
// one of them cannot let go; the table is then gone from every walk.
#define PCI_DRIVER_TABLES_MAX 8
int pci_driver_add_table(const struct pci_driver *drivers, int n);
int pci_driver_remove_table(const struct pci_driver *drivers);
int pci_rebind(void); // devices newly bound, or -1 before pci_bind() ran
// How many devices the drivers of `table` currently hold -- what stops
// its module unloading when a driver has no remove().
int pci_driver_table_bound(const struct pci_driver *drivers);

#endif
