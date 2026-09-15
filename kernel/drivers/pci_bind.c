// The PCI bus binder -- see kernel/include/kernel/pci_driver.h.
// driver-none: binds drivers to devices; drives nothing itself
#include "pci_driver.h"
#include "initcall.h"
#include "klog.h"
#include "kfmt.h"
#include "ktest.h"
#include "errno.h"

extern const struct pci_driver __pci_drivers_start[];
extern const struct pci_driver __pci_drivers_end[];

// The image's section first, then the tables modules add. Walked in
// that order everywhere, so a module never outranks a built-in driver
// for a device both match.
struct table { const struct pci_driver *drivers; int n; };
static struct table g_tables[PCI_DRIVER_TABLES_MAX];
static int g_table_count;

// The driver each device was handed to -- the POINTER, since a module's
// driver has to be found again when the module goes.
static const struct pci_driver *g_bound[PCI_MAX_DEVICES];
static int g_bind_ran;

static int image_count(void) {
    long n = __pci_drivers_end - __pci_drivers_start;
    return n < 0 ? 0 : (int)n;
}

int pci_driver_count(void) {
    int n = image_count();
    for (int t = 0; t < g_table_count; t++) n += g_tables[t].n;
    return n;
}

const struct pci_driver *pci_driver_at(int i) {
    if (i < 0) return 0;
    if (i < image_count()) return &__pci_drivers_start[i];
    i -= image_count();
    for (int t = 0; t < g_table_count; t++) {
        if (i < g_tables[t].n) return &g_tables[t].drivers[i];
        i -= g_tables[t].n;
    }
    return 0;
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
        const struct pci_driver *drv = pci_driver_at(i);
        for (int k = 0; k < drv->nmatches; k++)
            if (pci_match_device(&drv->matches[k], d)) return drv;
    }
    return 0;
}

const char *pci_device_driver(const struct pci_device *d) {
    for (int i = 0; i < pci_device_count() && i < PCI_MAX_DEVICES; i++)
        if (pci_device_at(i) == d) return g_bound[i] ? g_bound[i]->name : 0;
    return 0;
}

// Unclaimed devices in enumeration order, the first matching driver
// each. A probe that finds the device unusable says so itself; the
// binding is still recorded, because "a driver looked at it" is the
// fact `lspci` wants either way.
static int bind_unbound(void) {
    int bound = 0;
    for (int i = 0; i < pci_device_count() && i < PCI_MAX_DEVICES; i++) {
        if (g_bound[i]) continue;
        const struct pci_device *d = pci_device_at(i);
        const struct pci_driver *drv = driver_for(d);
        if (!drv || !drv->probe) continue;
        g_bound[i] = drv;
        drv->probe(d);
        bound++;
    }
    return bound;
}

static void pci_bind(void) {
    int bound = bind_unbound();
    g_bind_ran = 1;
    klog_printf("pci: %d device(s) bound to %d driver(s)\n", bound, pci_driver_count());
}
INITCALL(pci_bind, INIT_BUS);

int pci_rebind(void) {
    if (!g_bind_ran) return -1;
    return bind_unbound();
}

int pci_driver_add_table(const struct pci_driver *drivers, int n) {
    if (!drivers || n <= 0) return -EINVAL;
    for (int t = 0; t < g_table_count; t++)
        if (g_tables[t].drivers == drivers) return -EEXIST;
    if (g_table_count >= PCI_DRIVER_TABLES_MAX) return -ENOSPC;
    g_tables[g_table_count].drivers = drivers;
    g_tables[g_table_count].n = n;
    g_table_count++;
    return 0;
}

static int table_index(const struct pci_driver *drivers) {
    for (int t = 0; t < g_table_count; t++)
        if (g_tables[t].drivers == drivers) return t;
    return -1;
}

static int in_table(const struct table *tb, const struct pci_driver *drv) {
    return drv >= tb->drivers && drv < tb->drivers + tb->n;
}

int pci_driver_table_bound(const struct pci_driver *drivers) {
    int t = table_index(drivers);
    if (t < 0) return 0;
    int held = 0;
    for (int i = 0; i < pci_device_count() && i < PCI_MAX_DEVICES; i++)
        if (g_bound[i] && in_table(&g_tables[t], g_bound[i])) held++;
    return held;
}

int pci_driver_remove_table(const struct pci_driver *drivers) {
    int t = table_index(drivers);
    if (t < 0) return -ENOENT;

    // Every held device must be releasable BEFORE any is released, or
    // a table with one removable and one stuck driver would be left
    // half gone.
    for (int i = 0; i < pci_device_count() && i < PCI_MAX_DEVICES; i++) {
        if (!g_bound[i] || !in_table(&g_tables[t], g_bound[i])) continue;
        if (!g_bound[i]->remove) {
            klog_printf(KLOG_ERR "pci: %s holds a device and cannot let go\n", g_bound[i]->name);
            return -EBUSY;
        }
    }
    for (int i = 0; i < pci_device_count() && i < PCI_MAX_DEVICES; i++) {
        if (!g_bound[i] || !in_table(&g_tables[t], g_bound[i])) continue;
        g_bound[i]->remove(pci_device_at(i));
        g_bound[i] = 0;
    }
    for (int k = t; k + 1 < g_table_count; k++) g_tables[k] = g_tables[k + 1];
    g_table_count--;
    return 0;
}

// --- KTESTs ------------------------------------------------------------

KTEST("pci_bind", "every PCI driver has a probe and at least one match") {
    int n = pci_driver_count();
    KTEST_ASSERT(n >= 5);
    for (int i = 0; i < n; i++) {
        const struct pci_driver *drv = pci_driver_at(i);
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
            const struct pci_driver *drv = pci_driver_at(j);
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

// A table that matches nothing present: added, counted, walked, removed.
static void ktest_probe_nothing(const struct pci_device *d) { (void)d; }
static const struct pci_match ktest_matches[] = { PCI_MATCH_ID(0xDEAD, 0xBEEF) };
static const struct pci_driver ktest_table[] = {
    { .name = "ktest-pci", .matches = ktest_matches, .nmatches = 1,
      .probe = ktest_probe_nothing },
};

KTEST("pci_bind", "an added table is walked after the image's and removed cleanly") {
    int before = pci_driver_count();
    KTEST_ASSERT(pci_driver_add_table(ktest_table, 1) == 0);
    KTEST_ASSERT(pci_driver_add_table(ktest_table, 1) == -EEXIST);
    KTEST_ASSERT(pci_driver_count() == before + 1);
    KTEST_ASSERT(pci_driver_at(before) == &ktest_table[0]);
    KTEST_ASSERT(pci_rebind() == 0);            // nothing present to take
    KTEST_ASSERT(pci_driver_table_bound(ktest_table) == 0);
    KTEST_ASSERT(pci_driver_remove_table(ktest_table) == 0);
    KTEST_ASSERT(pci_driver_remove_table(ktest_table) == -ENOENT);
    KTEST_ASSERT(pci_driver_count() == before);
}
