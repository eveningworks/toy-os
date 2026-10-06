// The device-event ring -- see kernel/include/kernel/devevent.h.
#include "devevent.h"
#include "pci.h"
#include "query.h"
#include "initcall.h"
#include "irqflags.h"
#include "kfmt.h"
#include "string.h"
#include "timer.h"
#include "ktest.h"
#include <stdarg.h>

static struct query_devevent g_ring[QUERY_DEVEVENT_KEEP];
static uint32_t g_seq;   // events ever recorded; the newest is g_seq

static void add_v(int kind, const char *id, const char *driver,
                  const char *fmt, va_list ap) {
    struct query_devevent e;
    k_memset(&e, 0, sizeof e);
    e.kind = (uint32_t)kind;
    e.uptime_ms = coarse_ticks() * 10;   // 100 Hz, the kernel log's own clock
    if (id) k_strlcpy(e.device_id, id, sizeof e.device_id);
    if (driver) k_strlcpy(e.driver, driver, sizeof e.driver);
    if (fmt) k_vsnprintf(e.text, sizeof e.text, fmt, ap);

    uint64_t f = irq_save();
    e.seq = ++g_seq;
    g_ring[(e.seq - 1) % QUERY_DEVEVENT_KEEP] = e;
    irq_restore(f);
}

void devevent_add(int kind, const char *device_id, const char *driver, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    add_v(kind, device_id, driver, fmt, ap);
    va_end(ap);
}

void devevent_pci_id(const struct pci_device *d, char *out, unsigned cap) {
    k_snprintf(out, cap, "pci:%02x:%02x.%x", d->bus, d->device, d->function);
}

void devevent_pci(int kind, const struct pci_device *d, const char *driver, const char *fmt, ...) {
    char id[24];
    devevent_pci_id(d, id, sizeof id);
    va_list ap;
    va_start(ap, fmt);
    add_v(kind, id, driver, fmt, ap);
    va_end(ap);
}

// --- QUERY_DEVEVENT ------------------------------------------------------

static int held(void) {
    return g_seq < QUERY_DEVEVENT_KEEP ? (int)g_seq : QUERY_DEVEVENT_KEEP;
}

static int devevent_count(void) { return held(); }

int devevent_latest(struct query_devevent *out) {
    uint64_t f = irq_save();
    int ok = g_seq != 0;
    if (ok) *out = g_ring[(g_seq - 1) % QUERY_DEVEVENT_KEEP];
    irq_restore(f);
    return ok;
}

// Record `index` counts from the OLDEST event still held.
static int devevent_fill(int index, void *out) {
    uint64_t f = irq_save();
    int n = held();
    int ok = index >= 0 && index < n;
    if (ok) {
        uint32_t seq = g_seq - (uint32_t)n + 1 + (uint32_t)index;
        *(struct query_devevent *)out = g_ring[(seq - 1) % QUERY_DEVEVENT_KEEP];
    }
    irq_restore(f);
    return ok;
}

static const struct query_provider devevent_provider = {
    .cls = QUERY_DEVEVENT,
    .name = "devevent",
    .record_size = sizeof(struct query_devevent),
    .flags = QUERY_F_LIST,
    .count = devevent_count,
    .fill = devevent_fill,
};

static void devevent_query_init(void) { query_register(&devevent_provider); }
INITCALL(devevent_query_init, INIT_QUERY);

// --- KTESTs ------------------------------------------------------------

KTEST("devevent", "an event lands with its device, driver and text, newest last") {
    devevent_add(QUERY_DEVEV_ADDED, "ktest:devevent", "ktest", "seq probe %d", 7);
    int n = devevent_count();
    KTEST_ASSERT(n >= 1);
    struct query_devevent e;
    KTEST_ASSERT(devevent_fill(n - 1, &e));
    KTEST_ASSERT_EQ(e.kind, QUERY_DEVEV_ADDED);
    KTEST_ASSERT(k_strcmp(e.device_id, "ktest:devevent") == 0);
    KTEST_ASSERT(k_strcmp(e.driver, "ktest") == 0);
    KTEST_ASSERT(k_strcmp(e.text, "seq probe 7") == 0);
    KTEST_ASSERT_EQ(e.seq, g_seq);
    KTEST_ASSERT(!devevent_fill(n, &e));
}

KTEST("devevent", "the ring keeps the latest and its sequence shows the gap") {
    for (int i = 0; i < QUERY_DEVEVENT_KEEP + 3; i++)
        devevent_add(QUERY_DEVEV_REMOVED, "ktest:wrap", 0, "%d", i);
    KTEST_ASSERT_EQ(devevent_count(), QUERY_DEVEVENT_KEEP);
    struct query_devevent first, last;
    KTEST_ASSERT(devevent_fill(0, &first));
    KTEST_ASSERT(devevent_fill(QUERY_DEVEVENT_KEEP - 1, &last));
    KTEST_ASSERT_EQ(last.seq - first.seq, (uint32_t)(QUERY_DEVEVENT_KEEP - 1));
    KTEST_ASSERT(k_strcmp(first.text, "3") == 0);   // 0..2 were overwritten
    char want[8];
    k_snprintf(want, sizeof want, "%d", QUERY_DEVEVENT_KEEP + 2);
    KTEST_ASSERT(k_strcmp(last.text, want) == 0);
}
