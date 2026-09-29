// SMBIOS / DMI -- see kernel/include/kernel/smbios.h.
//
// Linux's dmi_scan.c has the same shape: find the entry point, walk the
// structure table, keep a handful of strings from types 0, 1 and 2. What
// differs is that placeholders are dropped here (GNOME's hostnamed does
// the same) and serial numbers are never read.
#include "smbios.h"
#include "query.h"
#include "initcall.h"
#include "klog.h"
#include "kfmt.h"
#include "string.h"

_Static_assert(sizeof(struct query_smbios) <= QUERY_RECORD_MAX,
               "a record past QUERY_RECORD_MAX is refused at every read");

static struct query_smbios g_info;

const struct query_smbios *smbios_info(void) { return &g_info; }

// --- the structure table --------------------------------------------------

// Filler a firmware writes where a vendor left a field blank. Compared
// without case; a field that says only this is no name at all.
static int placeholder(const char *s) {
    static const char *const JUNK[] = {
        "To be filled by O.E.M.", "Default string", "System Product Name",
        "System manufacturer", "System Version", "Not Applicable",
        "Not Specified", "None", "OEM", "O.E.M.", "0123456789", "x.x",
    };
    for (unsigned i = 0; i < sizeof JUNK / sizeof JUNK[0]; i++) {
        const char *a = s, *b = JUNK[i];
        while (*a && *b && (*a | 0x20) == (*b | 0x20)) { a++; b++; }
        if (!*a && !*b) return 1;
    }
    return 0;
}

// String `idx` (1-based) of the structure whose strings start at `str`
// and end before `end`, copied to `dst`, or "" -- index 0, missing, not
// printable ASCII, or a placeholder.
static void take_string(const uint8_t *str, const uint8_t *end, uint8_t idx,
                        char *dst, uint32_t cap) {
    dst[0] = '\0';
    if (!idx) return;
    const uint8_t *p = str;
    for (uint8_t i = 1; i < idx; i++) {
        while (p < end && *p) p++;
        if (p >= end) return;
        p++;                                    // past its NUL
        if (p >= end || !*p) return;            // the table's double NUL
    }
    uint32_t n = 0;
    for (const uint8_t *q = p; q < end && *q; q++) {
        if (*q < 0x20 || *q > 0x7E) { dst[0] = '\0'; return; }  // refuse, do not repair
        if (n + 1 < cap) dst[n++] = (char)*q;
    }
    while (n && dst[n - 1] == ' ') n--;         // firmware pads with spaces
    dst[n] = '\0';
    if (placeholder(dst)) dst[0] = '\0';
}

int smbios_parse(const uint8_t *tbl, uint32_t len, struct query_smbios *out) {
    k_memset(out, 0, sizeof *out);
    uint32_t off = 0;
    int count = 0;
    while (off + 4 <= len) {
        const uint8_t *s = tbl + off;
        uint8_t type = s[0], hlen = s[1];
        if (hlen < 4 || hlen > len - off) break;
        // The strings follow the formatted area and end at a double NUL;
        // a structure whose double NUL is not in the table ends the walk.
        const uint8_t *str = s + hlen, *end = tbl + len, *q = str;
        while (q + 1 < end && !(q[0] == 0 && q[1] == 0)) q++;
        if (q + 1 >= end) break;
        const uint8_t *str_end = q + 1;
        count++;

        if (type == 0 && hlen > 8) {
            take_string(str, str_end, s[4], out->bios_vendor, sizeof out->bios_vendor);
            take_string(str, str_end, s[5], out->bios_version, sizeof out->bios_version);
            take_string(str, str_end, s[8], out->bios_date, sizeof out->bios_date);
        } else if (type == 1 && hlen > 6) {
            take_string(str, str_end, s[4], out->sys_vendor, sizeof out->sys_vendor);
            take_string(str, str_end, s[5], out->product, sizeof out->product);
            take_string(str, str_end, s[6], out->sys_version, sizeof out->sys_version);
            // s[7] is the serial number: deliberately not read.
            if (hlen > 0x1A)
                take_string(str, str_end, s[0x1A], out->family, sizeof out->family);
        } else if (type == 127) {
            break;                                   // end of table
        }
        off = (uint32_t)(str_end + 1 - tbl);
    }
    return count;
}

// --- the entry point ------------------------------------------------------

static uint8_t sum8(const uint8_t *p, uint32_t n) {
    uint8_t s = 0;
    for (uint32_t i = 0; i < n; i++) s = (uint8_t)(s + p[i]);
    return s;
}

// The low 4 GiB are identity-mapped (boot.asm), and nothing above is.
static int readable(uint64_t phys, uint64_t len) {
    return phys && len && phys + len > phys && phys + len <= 0x100000000ULL;
}

// SMBIOS 3's 64-bit entry point ("_SM3_", 24 bytes) is preferred when a
// firmware carries both, as Linux prefers it: its table may lie above
// the 2.x one's 16-bit length.
static void smbios_init(void) {
    const uint8_t *e2 = 0, *e3 = 0;
    for (uintptr_t a = 0xF0000; a < 0x100000; a += 16) {
        const uint8_t *p = (const uint8_t *)a;
        if (!e3 && !k_memcmp(p, "_SM3_", 5) && p[6] >= 24 && p[6] <= 32 &&
            a + p[6] <= 0x100000 && sum8(p, p[6]) == 0)
            e3 = p;
        if (!e2 && !k_memcmp(p, "_SM_", 4) && p[5] >= 31 && p[5] <= 32 &&
            a + p[5] <= 0x100000 && sum8(p, p[5]) == 0 &&
            !k_memcmp(p + 16, "_DMI_", 5) && sum8(p + 16, 15) == 0)
            e2 = p;
    }
    uint64_t addr = 0, len = 0;
    uint8_t major = 0, minor = 0;
    if (e3) {
        k_memcpy(&addr, e3 + 16, 8);
        uint32_t max;
        k_memcpy(&max, e3 + 12, 4);
        len = max;
        major = e3[7];
        minor = e3[8];
    } else if (e2) {
        uint32_t a32;
        uint16_t l16;
        k_memcpy(&a32, e2 + 24, 4);
        k_memcpy(&l16, e2 + 22, 2);
        addr = a32;
        len = l16;
        major = e2[6];
        minor = e2[7];
    }
    if (len > 0x100000) len = 0x100000;              // a sane bound on a firmware claim
    if (readable(addr, len)) {
        int n = smbios_parse((const uint8_t *)(uintptr_t)addr, (uint32_t)len, &g_info);
        g_info.found = 1;
        g_info.major = major;
        g_info.minor = minor;
        klog_printf("smbios: %u.%u, %d structures -- \"%s\" \"%s\"\n",
                    (unsigned)major, (unsigned)minor, n, g_info.sys_vendor, g_info.product);
    } else {
        klog_printf("smbios: no entry point in the BIOS area\n");
    }
}

// --- the query ------------------------------------------------------------

static int smbios_count(void) { return 1; }

static int smbios_fill(int index, void *out) {
    if (index != 0) return 0;
    k_memcpy(out, &g_info, sizeof g_info);
    return 1;
}

static const struct query_provider smbios_provider = {
    .cls = QUERY_SMBIOS,
    .name = "smbios",
    .record_size = sizeof(struct query_smbios),
    .flags = 0,
    .count = smbios_count,
    .fill = smbios_fill,
    .fields = 0,
    .field_count = 0,
};

static void smbios_query_init(void) {
    smbios_init();
    query_register(&smbios_provider);
}
INITCALL(smbios_query_init, INIT_QUERY);
