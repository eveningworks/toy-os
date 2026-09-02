// The PCI bus binder -- see kernel/include/kernel/pci_driver.h.
// driver-none: binds drivers to devices; drives nothing itself
#include "pci_driver.h"
#include "initcall.h"
#include "klog.h"
#include "kfmt.h"
#include "ktest.h"

extern const struct pci_driver __pci_drivers_start[];
extern const struct pci_driver __pci_drivers_end[];

static const char *g_bound[PCI_MAX_DEVICES];

int pci_driver_count(void) {
    long n = __pci_drivers_end - __pci_drivers_start;
    return n < 0 ? 0 : (int)n;
}

const struct pci_driver *pci_driver_at(int i) {
    return (i >= 0 && i < pci_driver_count()) ? &__pci_drivers_start[i] : 0;
}

int pci_match_device(const struct pci_match *m, const struct pci_device *d) {
    if (!m || !d) return 0;
    if (m->vendor != PCI_ANY && m->vendor != d->vendor_id) return 0;
    if (m->device != PCI_ANY && m->device != d->device_id) return 0;
    if (m->class_code != PCI_ANY && m->class_code != d->class_code) return 0;
    if (m->subclass != PCI_ANY && m->subclass != d->subclass) return 0;
    if (m->prog_if != PCI_ANY && m->prog_if != d->prog_if) return 0;
    return 1;
}

static const struct pci_driver *driver_for(const struct pci_device *d) {
    int n = pci_driver_count();
    for (int i = 0; i < n; i++) {
        const struct pci_driver *drv = &__pci_drivers_start[i];
        for (int k = 0; k < drv->nmatches; k++)
            if (pci_match_device(&drv->matches[k], d)) return drv;
    }
    return 0;
}

const char *pci_device_driver(const struct pci_device *d) {
    for (int i = 0; i < pci_device_count() && i < PCI_MAX_DEVICES; i++)
        if (pci_device_at(i) == d) return g_bound[i];
    return 0;
}

// Devices in enumeration order, the first matching driver each. A
// probe that finds the device unusable says so itself; the binding is
// still recorded, because "a driver looked at it" is the fact `lspci`
// wants either way.
static void pci_bind(void) {
    int bound = 0;
    for (int i = 0; i < pci_device_count() && i < PCI_MAX_DEVICES; i++) {
        const struct pci_device *d = pci_device_at(i);
        const struct pci_driver *drv = driver_for(d);
        if (!drv || !drv->probe) continue;
        g_bound[i] = drv->name;
        drv->probe(d);
        bound++;
    }
    klog_printf("pci: %d device(s) bound to %d driver(s)\n", bound, pci_driver_count());
}
INITCALL(pci_bind, INIT_BUS);

// --- KTESTs ------------------------------------------------------------

KTEST("pci_bind", "every PCI driver has a probe and at least one match") {
    int n = pci_driver_count();
    KTEST_ASSERT(n >= 5);
    for (int i = 0; i < n; i++) {
        const struct pci_driver *drv = &__pci_drivers_start[i];
        KTEST_ASSERT(drv->name && drv->probe && drv->matches && drv->nmatches > 0);
    }
}

KTEST("pci_bind", "no two drivers claim the same present device") {
    // Two matches on one device would bind the first in link order and
    // silently starve the second -- a filename deciding a driver.
    for (int i = 0; i < pci_device_count(); i++) {
        const struct pci_device *d = pci_device_at(i);
        int claims = 0;
        for (int j = 0; j < pci_driver_count(); j++) {
            const struct pci_driver *drv = &__pci_drivers_start[j];
            for (int k = 0; k < drv->nmatches; k++)
                if (pci_match_device(&drv->matches[k], d)) { claims++; break; }
        }
        KTEST_ASSERT(claims <= 1);
    }
}

KTEST("pci_bind", "a wildcard matches and a mismatch does not") {
    struct pci_device d = { .vendor_id = 0x8086, .device_id = 0x9ca0,
                            .class_code = 0x04, .subclass = 0x03, .prog_if = 0 };
    struct pci_match by_class = PCI_MATCH_CLASS(0x04, 0x03, PCI_ANY);
    struct pci_match by_id    = PCI_MATCH_ID(0x8086, 0x9ca0);
    struct pci_match other    = PCI_MATCH_CLASS(0x04, 0x01, PCI_ANY);
    KTEST_ASSERT(pci_match_device(&by_class, &d));
    KTEST_ASSERT(pci_match_device(&by_id, &d));
    KTEST_ASSERT(!pci_match_device(&other, &d));
}
