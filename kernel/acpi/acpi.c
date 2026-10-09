// Finding the ACPI tables and validating them. What is done WITH them
// is split by table: acpi_power.c owns the FADT and the `_S5_` scan,
// acpi_madt.c owns the processor list. See kernel/include/kernel/acpi.h
// for the boundary this deliberately stops at.
#include "acpi.h"
#include "acpi_internal.h"
#include "aml.h"
#include "multiboot.h"
#include "bootstage.h"
#include "klog.h"
#include "kfmt.h" // klog_printf
#include "string.h"

// Tables are indexed, not copied -- the firmware's own memory is where
// they stay. 64 is well past what any machine here presents (QEMU q35
// lists 8, a laptop 20-30); the cap bounds the walk rather than
// rationing anything.
#define ACPI_TABLE_MAX 64

static const struct acpi_sdt_header *g_tables[ACPI_TABLE_MAX];
static struct acpi_state g_state;

struct acpi_state *acpi_state_mut(void) { return &g_state; }
const struct acpi_state *acpi_get_state(void) {
    BOOT_REQUIRE(BOOT_SUB_ACPI);
    return &g_state;
}

uint8_t acpi_checksum(const void *p, uint32_t len) {
    const uint8_t *b = p;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum = (uint8_t)(sum + b[i]);
    return sum;
}

// A physical address this kernel can read. boot.asm identity-maps the
// low 4 GiB and nothing else exists yet at acpi_init() time, so an
// address above that is refused rather than mapped -- a table there
// would be reported as absent, which is honest, instead of read from a
// mapping that is not there.
int acpi_phys_readable(uint64_t phys, uint32_t len) {
    if (phys == 0 || len == 0) return 0;
    if (phys + len < phys) return 0;          // wrapped
    return phys + len <= 0x100000000ULL;
}

const void *acpi_phys(uint64_t phys) {
    return (const void *)(uintptr_t)phys;
}

// The RSDP, in both revisions. v1 stops after `rsdt_phys`; v2 appends
// its own length and a second checksum covering the whole thing, which
// is why the two are summed separately.
struct acpi_rsdp {
    char     signature[8]; // "RSD PTR "
    uint8_t  checksum;     // over the first 20 bytes
    char     oem_id[6];
    uint8_t  revision;     // 0 = ACPI 1.0, 2 = 2.0+
    uint32_t rsdt_phys;
    // v2 only, from here
    uint32_t length;
    uint64_t xsdt_phys;
    uint8_t  ext_checksum;  // over `length` bytes
    uint8_t  reserved[3];
} __attribute__((packed));

static int rsdp_valid(const struct acpi_rsdp *r, uint32_t avail) {
    if (avail < 20) return 0;
    if (k_memcmp(r->signature, "RSD PTR ", 8) != 0) return 0;
    if (acpi_checksum(r, 20) != 0) return 0;
    if (r->revision >= 2 && avail >= 36) {
        // A v2 RSDP whose extended checksum fails is a v1 RSDP as far
        // as this is concerned: the first 20 bytes still summed to
        // zero, so the RSDT half of it is trustworthy on its own.
        if (r->length < 36 || r->length > avail) return 1;
        if (acpi_checksum(r, r->length) != 0) return 1;
    }
    return 1;
}

// The pre-multiboot way to find the RSDP: the first kilobyte of the
// EBDA, then the BIOS ROM area, both on 16-byte boundaries. Kept as a
// fallback because a bootloader that passes no tag is otherwise
// indistinguishable from a machine with no ACPI, and those want
// opposite responses.
static const struct acpi_rsdp *scan_bios_area(void) {
    // The address goes through a volatile local rather than being cast
    // inline: GCC reads a dereferenced constant address as a
    // one-element array and warns (-Warray-bounds), which is right
    // everywhere except the BIOS data area.
    volatile uintptr_t ebda_ptr_addr = 0x40E;
    uint32_t ebda = (uint32_t)*(const uint16_t *)ebda_ptr_addr << 4;
    if (ebda >= 0x80000 && ebda < 0xA0000) {
        for (uint32_t a = ebda; a < ebda + 1024; a += 16) {
            const struct acpi_rsdp *r = (const struct acpi_rsdp *)(uintptr_t)a;
            if (rsdp_valid(r, 36)) return r;
        }
    }
    for (uint32_t a = 0xE0000; a < 0x100000; a += 16) {
        const struct acpi_rsdp *r = (const struct acpi_rsdp *)(uintptr_t)a;
        if (rsdp_valid(r, 36)) return r;
    }
    return 0;
}

static void record_table(uint64_t phys) {
    if (g_state.table_count >= ACPI_TABLE_MAX) return;
    if (!acpi_phys_readable(phys, sizeof(struct acpi_sdt_header))) return;

    const struct acpi_sdt_header *h = acpi_phys(phys);
    if (h->length < sizeof *h) return;
    if (!acpi_phys_readable(phys, h->length)) return;
    // A table failing its own checksum is DROPPED, not repaired and not
    // trusted: everything downstream reads fixed offsets out of it, and
    // a corrupt FADT names I/O ports this kernel would then write to.
    if (acpi_checksum(h, h->length) != 0) {
        klog_printf(KLOG_ERR "acpi: %c%c%c%c at 0x%x FAILS its checksum -- ignored\n",
                    h->signature[0], h->signature[1], h->signature[2],
                    h->signature[3], (uint32_t)phys);
        return;
    }
    g_tables[g_state.table_count++] = h;
}

int acpi_table_count(void) { return (int)g_state.table_count; }

const struct acpi_sdt_header *acpi_table_at(int index) {
    if (index < 0 || index >= (int)g_state.table_count) return 0;
    return g_tables[index];
}

// The DSDT, if the FADT named one this walk could read. Its own entry
// point because it is not in the RSDT/XSDT -- see acpi.h.
static const struct acpi_sdt_header *dsdt_header(void) {
    if (!g_state.dsdt_phys ||
        !acpi_phys_readable(g_state.dsdt_phys, sizeof(struct acpi_sdt_header)))
        return 0;
    const struct acpi_sdt_header *h = acpi_phys(g_state.dsdt_phys);
    if (h->length <= sizeof *h ||
        !acpi_phys_readable(g_state.dsdt_phys, h->length))
        return 0;
    return h;
}

int acpi_dumpable_count(void) {
    return (int)g_state.table_count + (dsdt_header() ? 1 : 0);
}

const struct acpi_sdt_header *acpi_dumpable_at(int index) {
    if (index >= 0 && index < (int)g_state.table_count) return g_tables[index];
    if (index == (int)g_state.table_count) return dsdt_header();
    return 0;
}

const struct acpi_sdt_header *acpi_find_table(const char *sig) {
    BOOT_REQUIRE(BOOT_SUB_ACPI);
    for (uint32_t i = 0; i < g_state.table_count; i++)
        if (k_memcmp(g_tables[i]->signature, sig, 4) == 0) return g_tables[i];
    return 0;
}

static void walk_root(const struct acpi_rsdp *r) {
    // The XSDT when the RSDP is revision 2 AND names one. Firmware that
    // sets revision 2 with a zero XSDT address exists; falling through
    // to the RSDT there is what every real OS does.
    if (r->revision >= 2 && r->xsdt_phys &&
        acpi_phys_readable(r->xsdt_phys, sizeof(struct acpi_sdt_header))) {
        const struct acpi_sdt_header *x = acpi_phys(r->xsdt_phys);
        if (k_memcmp(x->signature, "XSDT", 4) == 0 &&
            x->length >= sizeof *x &&
            acpi_phys_readable(r->xsdt_phys, x->length) &&
            acpi_checksum(x, x->length) == 0) {
            g_state.flags |= ACPI_F_XSDT;
            g_state.rsdt_phys = r->xsdt_phys;
            uint32_t n = (x->length - (uint32_t)sizeof *x) / 8;
            // Read a byte at a time: the XSDT's 64-bit entries are only
            // 4-byte aligned inside a table whose header is 36 bytes,
            // so a u64 load off that array is misaligned.
            const uint8_t *e = (const uint8_t *)x + sizeof *x;
            for (uint32_t i = 0; i < n; i++) {
                uint64_t phys = 0;
                for (int b = 7; b >= 0; b--) phys = (phys << 8) | e[i * 8 + b];
                record_table(phys);
            }
            return;
        }
    }

    if (!acpi_phys_readable(r->rsdt_phys, sizeof(struct acpi_sdt_header))) return;
    const struct acpi_sdt_header *s = acpi_phys(r->rsdt_phys);
    if (k_memcmp(s->signature, "RSDT", 4) != 0) return;
    if (s->length < sizeof *s || !acpi_phys_readable(r->rsdt_phys, s->length)) return;
    if (acpi_checksum(s, s->length) != 0) return;

    g_state.rsdt_phys = r->rsdt_phys;
    uint32_t n = (s->length - (uint32_t)sizeof *s) / 4;
    const uint32_t *e = (const uint32_t *)(const void *)((const uint8_t *)s + sizeof *s);
    for (uint32_t i = 0; i < n; i++) record_table(e[i]);
}

void acpi_init(void) {
    const struct acpi_rsdp *rsdp = 0;

    uint32_t tag_bytes = 0;
    const void *tag = multiboot_acpi_rsdp(&tag_bytes);
    if (tag && rsdp_valid(tag, tag_bytes)) {
        rsdp = tag;
        g_state.rsdp_source = ACPI_RSDP_MULTIBOOT;
    } else {
        rsdp = scan_bios_area();
        if (rsdp) g_state.rsdp_source = ACPI_RSDP_SCAN;
    }

    if (!rsdp) {
        klog_write("acpi: no RSDP -- this machine reports no ACPI tables\n");
        boot_subsystem_up(BOOT_SUB_ACPI);
        acpi_query_init();
        return;
    }

    g_state.rsdp_revision = rsdp->revision;
    walk_root(rsdp);
    if (g_state.table_count) g_state.flags |= ACPI_F_TABLES;

    klog_printf("acpi: RSDP rev %d from %s, %d table(s) via %s\n",
                (int)rsdp->revision,
                g_state.rsdp_source == ACPI_RSDP_SCAN ? "a BIOS-area scan" : "multiboot2",
                (int)g_state.table_count,
                (g_state.flags & ACPI_F_XSDT) ? "XSDT" : "RSDT");

    acpi_fadt_init();
    acpi_madt_init();

    // The namespace, walked once here: pci_bind() asks it for `_PRT`
    // routing before any driver unmasks a line (kernel/acpi/acpi_prt.c).
    aml_build();
    acpi_power_boot();   // `acpimode=boot` only (kernel/acpi/acpi_power.c)

    boot_subsystem_up(BOOT_SUB_ACPI);
    acpi_query_init();
}
