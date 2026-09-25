// The panic store -- see kernel/panic_store.h for what it is and why the
// range is a constant. This file is the record format, the boot-time
// probe, the panic-time save, and QUERY_PANIC, through which logd files
// the record into the dead boot's log.
#include "panic_store.h"
#include "multiboot.h"
#include "klog.h"
#include "kfmt.h"
#include "kcrc.h"
#include "string.h"
#include "reloc.h"
#include "query.h"
#include "initcall.h"
#include "clocksource.h"
#include "version.h"

extern char _kernel_end[];
extern char __kimage_start[];

#define PANIC_RECORD_MAGIC   0x434e5054u // "TPNC"
#define PANIC_RECORD_VERSION 1u

struct panic_record {
    uint32_t magic;
    uint32_t version;
    uint32_t len;       // text bytes after the header
    uint32_t crc;       // kcrc32 of the header with crc = 0, then the text
    uint64_t uptime_ns;
    char     build[48];
};

_Static_assert(sizeof(struct panic_record) == PANIC_RECORD_HDR,
               "PANIC_RECORD_HDR must match struct panic_record");

static uint32_t record_crc(const struct panic_record *r) {
    struct panic_record h = *r;
    h.crc = 0;
    uint32_t crc = kcrc32_update(KCRC32_INIT, &h, sizeof h);
    crc = kcrc32_update(crc, (const char *)r + sizeof *r, r->len);
    return KCRC32_FINAL(crc);
}

int panic_record_seal(void *buf, uint32_t cap, uint32_t len,
                      uint64_t uptime_ns, const char *build) {
    if (cap < sizeof(struct panic_record) || len > cap - sizeof(struct panic_record))
        return 0;
    struct panic_record *r = buf;
    r->magic = PANIC_RECORD_MAGIC;
    r->version = PANIC_RECORD_VERSION;
    r->len = len;
    r->uptime_ns = uptime_ns;
    k_memset(r->build, 0, sizeof r->build);
    if (build) k_strlcpy(r->build, build, sizeof r->build);
    r->crc = record_crc(r);
    return 1;
}

// Everything is checked before `len` is trusted, because the bytes may
// be anything at all: a cold boot's RAM, or firmware's scratch.
int panic_record_check(const void *buf, uint32_t cap) {
    const struct panic_record *r = buf;
    if (cap < sizeof *r) return -1;
    if (r->magic != PANIC_RECORD_MAGIC || r->version != PANIC_RECORD_VERSION) return -1;
    if (r->len > cap - sizeof *r) return -1;
    if (r->build[sizeof r->build - 1] != 0) return -1;
    if (record_crc(r) != r->crc) return -1;
    return (int)r->len;
}

// ---- the store ------------------------------------------------------

static int g_usable;
static int g_recovered;
static int g_ram_ok;

static int overlaps(uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1) {
    return a0 < b1 && b0 < a1;
}

static void ram_cb(const struct multiboot_mmap_region *region) {
    if (region->type != 1) return;
    if (region->base <= PANIC_STORE_BASE &&
        region->base + region->length >= PANIC_STORE_BASE + PANIC_STORE_SIZE)
        g_ram_ok = 1;
}

// Why the range cannot be used on this boot, or NULL.
static const char *refusal(void) {
    const uint64_t s = PANIC_STORE_BASE, e = PANIC_STORE_BASE + PANIC_STORE_SIZE;
    g_ram_ok = 0;
    multiboot_mmap_foreach(ram_cb);
    if (!g_ram_ok) return "not RAM in this machine's memory map";
    uint64_t delta = kernel_reloc_delta();
    uint64_t kend = (uint64_t)(uintptr_t)_kernel_end;
    if (overlaps(s, e, 0x100000, delta ? kend - delta : kend))
        return "the kernel image has grown into it -- move PANIC_STORE_BASE";
    if (delta && overlaps(s, e, (uint64_t)(uintptr_t)__kimage_start, kend))
        return "KASLR placed the kernel on it";
    uint64_t is, ie;
    if (multiboot_get_info_range(&is, &ie) && overlaps(s, e, is, ie))
        return "GRUB put its info block on it";
    for (int i = 0; ; i++) {
        struct multiboot_module_info mod;
        if (!multiboot_get_module(i, &mod)) break;
        if (overlaps(s, e, mod.start, mod.end)) return "GRUB put a module on it";
    }
    return 0;
}

void panic_store_probe(void) {
    const char *why = refusal();
    if (why) {
        klog_printf("panic_store: NOT in use -- %s; a panic will not be kept\n", why);
        return;
    }
    g_usable = 1;
    void *base = (void *)(uintptr_t)PANIC_STORE_BASE;
    int len = panic_record_check(base, PANIC_STORE_SIZE);
    if (len < 0) {
        // WHAT IS THERE INSTEAD tells a firmware that clears RAM (zeroes)
        // from a memory scrambler re-keyed by the reset (noise) from a
        // bootloader that wrote over part of it (the magic survives).
        const uint32_t *w = base;
        const char *what = "no record";
        if (w[0] == PANIC_RECORD_MAGIC) what = "a DAMAGED record: magic intact, length or CRC wrong";
        else if (!w[0] && !w[1] && !w[2] && !w[3]) what = "zeroes";
        else if (w[0] == 0xFFFFFFFFu && w[1] == 0xFFFFFFFFu) what = "all ones";
        else what = "other bytes -- overwritten, or scrambled by the reset";
        klog_printf("panic_store: %lu KiB at 0x%lx, nothing recovered (%s; first word 0x%x)\n",
                    (unsigned long)(PANIC_STORE_SIZE / 1024),
                    (unsigned long)PANIC_STORE_BASE, what, w[0]);
        return;
    }
    g_recovered = 1;
    const struct panic_record *r = base;
    klog_printf(KLOG_WARN "panic_store: the PREVIOUS BOOT PANICKED -- recovered %d bytes "
                "of its log (build %s, up %lus); logd files it with that boot\n",
                len, r->build, (unsigned long)(r->uptime_ns / 1000000000ull));
}

int panic_store_usable(void) { return g_usable; }

// The newest bytes of the ring, straight into the store: no staging
// buffer, since a panic may be an out-of-memory one.
void panic_store_save(void) {
    if (!g_usable) return;
    char *text = PANIC_RECORD_TEXT((uintptr_t)PANIC_STORE_BASE);
    uint32_t cap = (uint32_t)(PANIC_STORE_SIZE - PANIC_RECORD_HDR);
    uint32_t retained = klog_retained_bytes();
    uint64_t from = klog_total_bytes() - (retained < cap ? retained : cap);
    uint64_t first;
    uint32_t len = klog_read(from, text, cap, &first);
    panic_record_seal((void *)(uintptr_t)PANIC_STORE_BASE, (uint32_t)PANIC_STORE_SIZE,
                      len, clocksource_now_ns(), TOYOS_VERSION_FULL);
    g_recovered = 0; // this boot's own record is the NEXT boot's to report
}

const char *panic_store_text(uint32_t *len, uint64_t *uptime_ns, const char **build) {
    if (!g_recovered) return 0;
    const struct panic_record *r = (const void *)(uintptr_t)PANIC_STORE_BASE;
    if (len) *len = r->len;
    if (uptime_ns) *uptime_ns = r->uptime_ns;
    if (build) *build = r->build;
    return PANIC_RECORD_TEXT((uintptr_t)PANIC_STORE_BASE);
}

void panic_store_clear(void) {
    if (!g_recovered) return;
    ((struct panic_record *)(uintptr_t)PANIC_STORE_BASE)->magic = 0;
    g_recovered = 0;
    klog_write("panic_store: record cleared\n");
}

// ---- QUERY_PANIC ----------------------------------------------------

static int panic_count(void) {
    uint32_t len;
    if (!panic_store_text(&len, 0, 0)) return 0;
    return (int)((len + QUERY_PANIC_DATA - 1) / QUERY_PANIC_DATA);
}

static int panic_fill(int index, void *out) {
    uint32_t len;
    uint64_t up;
    const char *build;
    const char *text = panic_store_text(&len, &up, &build);
    if (!text || index < 0) return 0;
    uint32_t off = (uint32_t)index * QUERY_PANIC_DATA;
    if (off >= len) return 0;
    struct query_panic *q = out;
    q->total = len;
    q->len = len - off < QUERY_PANIC_DATA ? len - off : QUERY_PANIC_DATA;
    q->uptime_ns = up;
    k_strlcpy(q->build, build, sizeof q->build);
    k_memcpy(q->data, text + off, q->len);
    return 1;
}

static const struct query_provider panic_provider = {
    .cls = QUERY_PANIC,
    .name = "panic",
    .record_size = sizeof(struct query_panic),
    .flags = QUERY_F_LIST,
    .count = panic_count,
    .fill = panic_fill,
    .fields = NULL,
    .field_count = 0,
};

static void panic_query_init(void) { query_register(&panic_provider); }
INITCALL(panic_query_init, INIT_QUERY);
