// The PCI bus binder -- see kernel/include/kernel/pci_driver.h.
// driver-none: binds drivers to devices; drives nothing itself
#include "pci_driver.h"
#include "initcall.h"
#include "klog.h"
#include "kfmt.h"
#include "ktest.h"
#include "errno.h"
#include "pci_internal.h" // pci_command_update -- stop a released card mastering
#include "scheduler.h"    // preempt guard around a probe -- see probe_one()
#include "devevent.h"
#include "string.h"
#include <stdarg.h>

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

// The last driver that declined each device, and why -- pci_probe_decline().
static char g_declined_by[PCI_MAX_DEVICES][16];
static char g_why[PCI_MAX_DEVICES][QUERY_DEVEVENT_TEXT];

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

// **A DRIVER CALLBACK RUNS WITH INTERRUPTS ON AND PREEMPTION OFF, and
// the bus is what guarantees it** -- Linux's process context, sized for
// here. probe() and remove() were written for INITCALL and module
// teardown; three routes now reach one from a SYSCALL, where
// `context_switch.asm` leaves IF clear. `hda_probe()` waits 30 ms for
// the link, which on a PIT clocksource is a `coarse_ticks()` loop the
// timer can never advance: the machine stops dead at one instruction,
// no panic, no log. Preemption stays off, so nothing a callback walks
// is re-entered; only the timer is let in.
static uint64_t driver_ctx_enter(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) :: "memory");
    scheduler_preempt_disable();
    __asm__ volatile ("sti" ::: "memory");
    return flags;
}

static void driver_ctx_leave(uint64_t flags) {
    if (!(flags & 0x200)) __asm__ volatile ("cli" ::: "memory");
    scheduler_preempt_enable();
}

static int probe_one(const struct pci_driver *drv, const struct pci_device *d) {
    uint64_t f = driver_ctx_enter();
    int r = drv->probe(d);
    driver_ctx_leave(f);
    return r;
}

static void remove_one(const struct pci_driver *drv, const struct pci_device *d) {
    uint64_t f = driver_ctx_enter();
    drv->remove(d);
    driver_ctx_leave(f);
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

static int index_of(const struct pci_device *d) {
    for (int i = 0; i < pci_device_count() && i < PCI_MAX_DEVICES; i++)
        if (pci_device_at(i) == d) return i;
    return -1;
}

const char *pci_device_driver(const struct pci_device *d) {
    int i = index_of(d);
    return i >= 0 && g_bound[i] ? g_bound[i]->name : 0;
}

// The binding is recorded BEFORE probe() runs, so the decline below
// knows whose it is; a probe that declines is unbound again at once.
int pci_probe_decline(const struct pci_device *d, const char *fmt, ...) {
    int i = index_of(d);
    if (i < 0) return -ENODEV;
    const char *drv = g_bound[i] ? g_bound[i]->name : "?";
    k_strlcpy(g_declined_by[i], drv, sizeof g_declined_by[i]);
    va_list ap;
    va_start(ap, fmt);
    k_vsnprintf(g_why[i], sizeof g_why[i], fmt, ap);
    va_end(ap);
    klog_printf("pci: %s declined %02x:%02x.%u: %s\n",
                drv, d->bus, d->device, d->function, g_why[i]);
    devevent_pci(QUERY_DEVEV_DECLINED, d, drv, "%s", g_why[i]);
    return -ENODEV;
}

const char *pci_device_declined(int index, const char **why) {
    if (index < 0 || index >= pci_device_count() || index >= PCI_MAX_DEVICES
        || !g_declined_by[index][0])
        return 0;
    if (why) *why = g_why[index];
    return g_declined_by[index];
}

// Hands device `i` to `drv`: 1 when it drives it, 0 when it declined.
static int bind_one(int i, const struct pci_driver *drv) {
    const struct pci_device *d = pci_device_at(i);
    g_bound[i] = drv;
    g_declined_by[i][0] = 0;
    g_why[i][0] = 0;
    int r = probe_one(drv, d);
    if (r >= 0) {
        devevent_pci(QUERY_DEVEV_BOUND, d, drv->name, "Driven by %s", drv->name);
        return 1;
    }
    g_bound[i] = 0;
    // Whatever the probe had pointed the card at, it no longer masters.
    pci_command_update(d, 0, PCI_CMD_BUS_MASTER);
    if (!g_declined_by[i][0]) {   // an errno without pci_probe_decline()
        k_strlcpy(g_declined_by[i], drv->name, sizeof g_declined_by[i]);
        k_snprintf(g_why[i], sizeof g_why[i], "its probe returned %d", r);
        klog_printf("pci: %s declined %02x:%02x.%u: %s\n",
                    drv->name, d->bus, d->device, d->function, g_why[i]);
        devevent_pci(QUERY_DEVEV_DECLINED, d, drv->name, "%s", g_why[i]);
    }
    return 0;
}

// --- handing a device back (pci_driver.h) ------------------------------

int pci_device_removable(int index) {
    if (index < 0 || index >= pci_device_count() || index >= PCI_MAX_DEVICES)
        return 0;
    return g_bound[index] && g_bound[index]->remove ? 1 : 0;
}

int pci_device_release(int index) {
    if (index < 0 || index >= pci_device_count() || index >= PCI_MAX_DEVICES)
        return -EINVAL;
    const struct pci_driver *drv = g_bound[index];
    if (!drv) return -ENOENT;
    if (!drv->remove) return -ENOTSUP;

    const struct pci_device *d = pci_device_at(index);
    remove_one(drv, d);
    g_bound[index] = 0;
    // The bus's own statement that nothing in ring 0 drives this any
    // more. A driver's remove() quiesces its engine; this stops the
    // CARD mastering whatever it was last pointed at, which is the
    // closest thing to vfio-pci's device reset that this kernel has.
    // Memory decode stays on -- the next holder needs the BAR.
    pci_command_update(d, 0, PCI_CMD_BUS_MASTER);
    klog_printf("pci: %s released %02x:%02x.%u\n",
                drv->name, d->bus, d->device, d->function);
    devevent_pci(QUERY_DEVEV_RELEASED, d, drv->name, "%s let go", drv->name);
    return 0;
}

int pci_device_rebind(int index) {
    if (index < 0 || index >= pci_device_count() || index >= PCI_MAX_DEVICES)
        return -EINVAL;
    if (g_bound[index]) return -EBUSY;
    const struct pci_device *d = pci_device_at(index);
    const struct pci_driver *drv = driver_for(d);
    if (!drv || !drv->probe) return 0;
    if (!bind_one(index, drv)) return 0;
    klog_printf("pci: %s took %02x:%02x.%u back\n",
                drv->name, d->bus, d->device, d->function);
    return 1;
}

int pci_device_claim(int index, const struct pci_driver *owner) {
    if (g_bind_ran || index < 0 || index >= pci_device_count() || index >= PCI_MAX_DEVICES)
        return 0;
    if (g_bound[index]) return 0;
    g_bound[index] = owner;
    devevent_pci(QUERY_DEVEV_BOUND, pci_device_at(index), owner->name,
                 "Reserved for %s before drivers were bound", owner->name);
    return 1;
}

// Unbound devices in enumeration order, the first matching driver each.
// A driver that already declined a device is not asked again on a pass
// -- a module load re-runs this, and nothing it changed is that
// driver's reason; pci_device_rebind() asks again on purpose.
static int bind_unbound(void) {
    int bound = 0;
    for (int i = 0; i < pci_device_count() && i < PCI_MAX_DEVICES; i++) {
        if (g_bound[i]) continue;
        const struct pci_driver *drv = driver_for(pci_device_at(i));
        if (!drv || !drv->probe) continue;
        if (g_declined_by[i][0] && k_strcmp(g_declined_by[i], drv->name) == 0) continue;
        bound += bind_one(i, drv);
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
        const char *name = g_bound[i]->name;
        remove_one(g_bound[i], pci_device_at(i));
        g_bound[i] = 0;
        devevent_pci(QUERY_DEVEV_RELEASED, pci_device_at(i), name,
                     "%s let go: its module is unloading", name);
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
static int ktest_probe_nothing(const struct pci_device *d) { (void)d; return 0; }
static const struct pci_match ktest_matches[] = { PCI_MATCH_ID(0xDEAD, 0xBEEF) };
static const struct pci_driver ktest_table[] = {
    { .name = "ktest-pci", .matches = ktest_matches, .nmatches = 1,
      .probe = ktest_probe_nothing },
};

// A driver that declines: the device must come out UNBOUND, with the
// reason kept beside it and in the event ring. Run against a real
// present device no driver matches, so the bus path is the real one.
static int ktest_probe_decline(const struct pci_device *d) {
    return pci_probe_decline(d, "ktest says no (%d)", 42);
}
static struct pci_match ktest_decline_match[1];
static const struct pci_driver ktest_decline_table[] = {
    { .name = "ktest-decline", .matches = ktest_decline_match, .nmatches = 1,
      .probe = ktest_probe_decline },
};

KTEST("pci_bind", "a probe that declines leaves the device unbound and says why") {
    int idx = -1;
    for (int i = 0; i < pci_device_count() && i < PCI_MAX_DEVICES; i++)
        if (!g_bound[i] && !driver_for(pci_device_at(i))) { idx = i; break; }
    if (idx < 0) { KTEST_SKIP("every device has a driver"); return; }
    const struct pci_device *d = pci_device_at(idx);
    ktest_decline_match[0] = (struct pci_match)PCI_MATCH_ID(d->vendor_id, d->device_id);
    char saved_by[sizeof g_declined_by[0]], saved_why[sizeof g_why[0]];
    k_strlcpy(saved_by, g_declined_by[idx], sizeof saved_by);
    k_strlcpy(saved_why, g_why[idx], sizeof saved_why);

    KTEST_ASSERT(pci_driver_add_table(ktest_decline_table, 1) == 0);
    KTEST_ASSERT_EQ(pci_device_rebind(idx), 0);          // matched, and declined
    KTEST_ASSERT(pci_device_driver(d) == 0);
    const char *why = 0;
    const char *by = pci_device_declined(idx, &why);
    KTEST_ASSERT(by && k_strcmp(by, "ktest-decline") == 0);
    KTEST_ASSERT(why && k_strcmp(why, "ktest says no (42)") == 0);
    struct query_devevent e;
    char id[24];
    devevent_pci_id(d, id, sizeof id);
    KTEST_ASSERT(devevent_latest(&e));
    KTEST_ASSERT_EQ(e.kind, QUERY_DEVEV_DECLINED);
    KTEST_ASSERT(k_strcmp(e.device_id, id) == 0 && k_strcmp(e.driver, "ktest-decline") == 0);
    KTEST_ASSERT(k_strcmp(e.text, "ktest says no (42)") == 0);
    KTEST_ASSERT_EQ(pci_rebind(), 0);                    // not asked again on a pass
    KTEST_ASSERT(pci_driver_remove_table(ktest_decline_table) == 0);

    k_strlcpy(g_declined_by[idx], saved_by, sizeof g_declined_by[idx]);
    k_strlcpy(g_why[idx], saved_why, sizeof g_why[idx]);
}

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
