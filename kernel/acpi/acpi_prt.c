// The `_PRT` reader -- see kernel/include/kernel/acpi_prt.h.
#include "acpi_prt.h"
#include "aml.h"
#include "aml_data.h"
#include "pci.h"
#include "string.h"
#include "errno.h"
#include "klog.h"
#include "kfmt.h"
#include "ktest.h"

#define OP_RETURN 0xA4
#define OP_IF     0xA0
#define OP_ELSE   0xA1
#define OP_LEQUAL 0x93
#define OP_LNOT   0x92

static int is_int_name(const struct aml_node *n, uint64_t *v) {
    return n && n->kind == AML_KIND_NAME && aml_int(n->data, n->len, v) != 0;
}

// The PCI host bridge: the Device whose _HID is PNP0A03 or PNP0A08 (as an
// EISA id or a string). The first one found is the root bus.
static int pci_root(void) {
    for (int i = 0; i < aml_node_count(); i++) {
        const struct aml_node *n = aml_node_at(i);
        if (!n || n->kind != AML_KIND_DEVICE) continue;
        int hid = aml_child(i, "_HID");
        const struct aml_node *h = hid >= 0 ? aml_node_at(hid) : 0;
        if (!h || h->kind != AML_KIND_NAME) continue;
        uint64_t v;
        if (aml_int(h->data, h->len, &v)) {
            if (v == 0x030AD041 || v == 0x080AD041) return i;
        } else if (h->len >= 8 && h->data[0] == 0x0D &&
                   (k_memcmp(h->data + 1, "PNP0A03", 7) == 0 || k_memcmp(h->data + 1, "PNP0A08", 7) == 0)) {
            return i;
        }
    }
    return -1;
}

// THE ONE ASSUMPTION: a name beginning `PIC` is the flag \_PIC(1) sets,
// and reads as 1 -- this kernel routes through the I/O APIC. Any other
// name reads as its constant, or refuses.
static int pred_value(int scope, const uint8_t *p, uint32_t avail, uint64_t *out, uint32_t *used) {
    uint64_t v;
    uint32_t n = aml_int(p, avail, &v);
    if (n) { *out = v; *used = n; return 0; }
    if (!aml_is_name_lead(p[0])) return -ENOTSUP;
    struct aml_name nm;
    n = aml_name_parse(p, avail, &nm);
    if (!n) return -ENOTSUP;
    *used = n;
    if (k_memcmp(nm.seg[nm.nsegs - 1], "PIC", 3) == 0) { *out = 1; return 0; }
    const struct aml_node *node = aml_node_at(aml_resolve(scope, &nm));
    if (!is_int_name(node, out)) return -ENOTSUP;
    return 0;
}

static int eval_pred(int scope, const uint8_t *p, uint32_t avail, int *truth, uint32_t *used) {
    if (avail < 1) return -ENOTSUP;
    uint64_t a, b; uint32_t ua, ub; int rc;
    if (p[0] == OP_LEQUAL) {
        if ((rc = pred_value(scope, p + 1, avail - 1, &a, &ua))) return rc;
        if ((rc = pred_value(scope, p + 1 + ua, avail - 1 - ua, &b, &ub))) return rc;
        *truth = a == b; *used = 1 + ua + ub; return 0;
    }
    if (p[0] == OP_LNOT) {
        int inner; uint32_t ui;
        if ((rc = eval_pred(scope, p + 1, avail - 1, &inner, &ui))) return rc;
        *truth = !inner; *used = 1 + ui; return 0;
    }
    if ((rc = pred_value(scope, p, avail, &a, &ua))) return rc;
    *truth = a != 0; *used = ua; return 0;
}

static uint32_t pkg_length(const uint8_t *p, uint32_t avail, uint32_t *used) {
    if (avail < 1) return 0;
    uint32_t extra = (p[0] >> 6) & 3;
    if (avail < 1 + extra) return 0;
    uint32_t len = extra ? (p[0] & 0x0F) : (p[0] & 0x3F);
    for (uint32_t i = 0; i < extra; i++) len |= (uint32_t)p[1 + i] << (4 + 8 * i);
    *used = 1 + extra;
    return (len >= *used && len <= avail) ? len : 0;
}

// `Return (X)`: the object returned, as a pointer into the table, or
// the node a returned name resolves to (*ref). 0 on success.
static int returned(int scope, const uint8_t *p, uint32_t avail,
                    const uint8_t **obj, uint32_t *obj_len, int *ref) {
    if (avail < 2 || p[0] != OP_RETURN) return -ENOTSUP;
    *ref = -1;
    if (aml_is_name_lead(p[1])) {
        struct aml_name nm;
        if (!aml_name_parse(p + 1, avail - 1, &nm)) return -ENOTSUP;
        *ref = aml_resolve(scope, &nm);
        return *ref >= 0 ? 0 : -ENOENT;
    }
    uint32_t n = aml_object_len(p + 1, avail - 1);
    if (!n) return -ENOTSUP;
    *obj = p + 1; *obj_len = n;
    return 0;
}

// The constant package behind node `idx`, through the shapes the header
// lists. `depth` bounds reference chains.
static int package_of(int idx, const uint8_t **pkg, uint32_t *pkg_len, int depth) {
    const struct aml_node *n = aml_node_at(idx);
    if (!n || depth > 4) return -ENOTSUP;
    if (n->kind == AML_KIND_NAME) {
        if (n->len && n->data[0] == 0x12) { *pkg = n->data; *pkg_len = n->len; return 0; }
        return -ENOTSUP;
    }
    if (n->kind != AML_KIND_METHOD || n->len < 2) return -ENOTSUP;
    const uint8_t *body = n->data + 1;       // past the flags byte
    uint32_t avail = n->len - 1;
    const uint8_t *obj = 0; uint32_t obj_len = 0; int ref = -1;
    int rc;
    if (body[0] == OP_RETURN) {
        if ((rc = returned(idx, body, avail, &obj, &obj_len, &ref))) return rc;
        if (ref >= 0) return package_of(ref, pkg, pkg_len, depth + 1);
        if (obj[0] != 0x12) return -ENOTSUP;
        *pkg = obj; *pkg_len = obj_len;
        return 0;
    }
    if (body[0] == OP_IF) {
        uint32_t used = 0;
        uint32_t plen = pkg_length(body + 1, avail - 1, &used);
        if (!plen) return -ENOTSUP;
        const uint8_t *pred = body + 1 + used;
        int truth; uint32_t pu;
        if ((rc = eval_pred(idx, pred, plen - used, &truth, &pu))) return rc;
        const uint8_t *then = pred + pu;
        uint32_t then_len = plen - used - pu;
        const uint8_t *after = body + 1 + plen;
        uint32_t after_len = avail - 1 - plen;
        if (after_len && after[0] == OP_ELSE) {   // Else { Return (B) }
            uint32_t eu = 0;
            uint32_t elen = pkg_length(after + 1, after_len - 1, &eu);
            if (!elen) return -ENOTSUP;
            after = after + 1 + eu; after_len = elen - eu;
        }
        const uint8_t *take = truth ? then : after;
        uint32_t take_len = truth ? then_len : after_len;
        if ((rc = returned(idx, take, take_len, &obj, &obj_len, &ref))) return rc;
        if (ref >= 0) return package_of(ref, pkg, pkg_len, depth + 1);
        if (obj[0] != 0x12) return -ENOTSUP;
        *pkg = obj; *pkg_len = obj_len;
        return 0;
    }
    return -ENOTSUP;
}

// A link device's interrupt, from its `_CRS` if that is a constant.
static int link_irq(int link, uint32_t *gsi, int *level, int *low) {
    int crs = aml_child(link, "_CRS");
    const struct aml_node *n = aml_node_at(crs);
    if (!n) return -ENOENT;
    const uint8_t *obj = 0; uint32_t obj_len = 0;
    if (n->kind == AML_KIND_NAME) { obj = n->data; obj_len = n->len; }
    else if (n->kind == AML_KIND_METHOD && n->len >= 2) {
        int ref = -1;
        if (returned(crs, n->data + 1, n->len - 1, &obj, &obj_len, &ref) || ref >= 0) return -ENOTSUP;
    } else return -ENOTSUP;
    const uint8_t *bytes; uint32_t nbytes;
    if (!aml_buffer(obj, obj_len, &bytes, &nbytes)) return -ENOTSUP;
    return aml_crs_irq(bytes, nbytes, gsi, level, low) ? 0 : -ENOENT;
}

// Looks `dev`/`pin` up in the `_PRT` under `owner` (a Device node).
static int lookup_under(int owner, uint8_t dev, uint8_t pin, uint32_t *gsi, int *level, int *low) {
    int prt = aml_child(owner, "_PRT");
    if (prt < 0) return -ENOENT;
    const uint8_t *pkg; uint32_t pkg_len;
    int rc = package_of(prt, &pkg, &pkg_len, 0);
    if (rc) return rc;
    uint32_t count; const uint8_t *e; uint32_t e_avail;
    if (!aml_package(pkg, pkg_len, &count, &e, &e_avail)) return -ENOTSUP;
    for (uint32_t i = 0; i < count && e_avail; i++) {
        uint32_t elen = aml_object_len(e, e_avail);
        if (!elen) return -ENOTSUP;
        uint32_t fc; const uint8_t *f; uint32_t f_avail;
        if (aml_package(e, elen, &fc, &f, &f_avail) && fc == 4) {
            uint64_t addr, epin, idx;
            uint32_t u1 = aml_int(f, f_avail, &addr);
            uint32_t u2 = u1 ? aml_int(f + u1, f_avail - u1, &epin) : 0;
            if (u1 && u2 && (uint8_t)(addr >> 16) == dev && epin == pin) {
                const uint8_t *src = f + u1 + u2;
                uint32_t src_avail = f_avail - u1 - u2;
                uint64_t zero;
                uint32_t u3 = aml_int(src, src_avail, &zero);
                if (u3) {
                    if (!aml_int(src + u3, src_avail - u3, &idx)) return -ENOTSUP;
                    if (zero != 0) return -ENOTSUP;
                    *gsi = (uint32_t)idx; *level = 1; *low = 1;   // a bare GSI is PCI's level/low
                    return 0;
                }
                struct aml_name nm;
                if (!aml_name_parse(src, src_avail, &nm)) return -ENOTSUP;
                int link = aml_resolve(owner, &nm);
                if (link < 0) return -ENOENT;
                return link_irq(link, gsi, level, low);
            }
        }
        e += elen; e_avail -= elen;
    }
    return -ENOENT;
}

// The Device under `parent` whose _ADR names dev/fn.
static int child_by_adr(int parent, uint8_t dev, uint8_t fn) {
    for (int i = 0; i < aml_node_count(); i++) {
        const struct aml_node *n = aml_node_at(i);
        if (!n || n->parent != parent || n->kind != AML_KIND_DEVICE) continue;
        uint64_t adr;
        if (is_int_name(aml_node_at(aml_child(i, "_ADR")), &adr) &&
            (uint8_t)(adr >> 16) == dev && (uint8_t)adr == fn)
            return i;
    }
    return -1;
}

int acpi_prt_lookup(uint8_t bus, uint8_t dev, uint8_t pin, uint32_t *gsi, int *level, int *low) {
    if (pin > 3) return -ENOENT;
    int root = pci_root();
    if (root < 0) return -ENOENT;

    // Walk up through the bridges: a bridge with its own _PRT answers
    // for its bus; one without passes the pin up, SWIZZLED the way the
    // PCI-to-PCI bridge spec says (pin + device) mod 4.
    for (int hops = 0; hops < 4; hops++) {
        if (bus == 0) return lookup_under(root, dev, pin, gsi, level, low);
        const struct pci_device *br = pci_bridge_for_bus(bus);
        if (!br) return -ENOENT;
        int node = br->bus == 0 ? child_by_adr(root, br->device, br->function) : -1;
        if (node >= 0 && aml_child(node, "_PRT") >= 0)
            return lookup_under(node, dev, pin, gsi, level, low);
        pin = (uint8_t)((pin + dev) & 3);
        dev = br->device;
        bus = br->bus;
    }
    return -ENOENT;
}

// --- KTESTs ------------------------------------------------------------
//
// One fixture per shape the header names, assembled by the same script
// that wrote this file (the PkgLengths are the part a person gets
// wrong). Each is parsed into a fresh namespace and rebuilt from this
// machine's tables afterwards, so nothing here changes what the next
// caller sees.

static const uint8_t FIX_STATIC[] = {
    0x5b, 0x82, 0x34, 0x50, 0x43, 0x49, 0x30, 0x08, 0x5f, 0x48, 0x49, 0x44,
    0x0c, 0x41, 0xd0, 0x0a, 0x08, 0x08, 0x5f, 0x50, 0x52, 0x54, 0x12, 0x1f,
    0x02, 0x12, 0x0c, 0x04, 0x0c, 0xff, 0xff, 0x03, 0x00, 0x0a, 0x00, 0x00,
    0x0a, 0x16, 0x12, 0x0f, 0x04, 0x0c, 0xff, 0xff, 0x03, 0x00, 0x0a, 0x01,
    0x4c, 0x4e, 0x4b, 0x41, 0x0a, 0x00, 0x5b, 0x82, 0x19, 0x4c, 0x4e, 0x4b,
    0x41, 0x08, 0x5f, 0x43, 0x52, 0x53, 0x11, 0x0e, 0x0a, 0x0b, 0x89, 0x06,
    0x00, 0x01, 0x01, 0x14, 0x00, 0x00, 0x00, 0x79, 0x00,
};
static const uint8_t FIX_IF_PICM[] = {
    0x08, 0x50, 0x49, 0x43, 0x4d, 0x00, 0x5b, 0x82, 0x44, 0x05, 0x50, 0x43,
    0x49, 0x30, 0x08, 0x5f, 0x48, 0x49, 0x44, 0x0c, 0x41, 0xd0, 0x0a, 0x03,
    0x14, 0x16, 0x5f, 0x50, 0x52, 0x54, 0x00, 0xa0, 0x0a, 0x50, 0x49, 0x43,
    0x4d, 0xa4, 0x41, 0x52, 0x30, 0x30, 0xa4, 0x50, 0x52, 0x30, 0x30, 0x08,
    0x41, 0x52, 0x30, 0x30, 0x12, 0x0f, 0x01, 0x12, 0x0c, 0x04, 0x0c, 0xff,
    0xff, 0x1f, 0x00, 0x0a, 0x02, 0x00, 0x0a, 0x12, 0x08, 0x50, 0x52, 0x30,
    0x30, 0x12, 0x12, 0x01, 0x12, 0x0f, 0x04, 0x0c, 0xff, 0xff, 0x1f, 0x00,
    0x0a, 0x02, 0x4c, 0x4e, 0x4b, 0x43, 0x0a, 0x00, 0x5b, 0x82, 0x14, 0x4c,
    0x4e, 0x4b, 0x43, 0x14, 0x0e, 0x5f, 0x43, 0x52, 0x53, 0x00, 0x70, 0x50,
    0x49, 0x52, 0x51, 0x60, 0xa4, 0x60,
};
static const uint8_t FIX_REFERENCE[] = {
    0x08, 0x50, 0x49, 0x43, 0x4d, 0x00, 0x08, 0x41, 0x52, 0x30, 0x30, 0x12,
    0x0f, 0x01, 0x12, 0x0c, 0x04, 0x0c, 0xff, 0xff, 0x02, 0x00, 0x0a, 0x00,
    0x00, 0x0a, 0x10, 0x5b, 0x82, 0x49, 0x06, 0x50, 0x43, 0x49, 0x30, 0x08,
    0x5f, 0x48, 0x49, 0x44, 0x0c, 0x41, 0xd0, 0x0a, 0x08, 0x14, 0x16, 0x5f,
    0x50, 0x52, 0x54, 0x00, 0xa0, 0x0a, 0x50, 0x49, 0x43, 0x4d, 0xa4, 0x41,
    0x52, 0x30, 0x30, 0xa4, 0x50, 0x52, 0x30, 0x30, 0x14, 0x0d, 0x41, 0x52,
    0x30, 0x30, 0x00, 0xa4, 0x5e, 0x5e, 0x41, 0x52, 0x30, 0x30, 0x14, 0x0d,
    0x50, 0x52, 0x30, 0x30, 0x00, 0xa4, 0x5e, 0x5e, 0x41, 0x52, 0x30, 0x30,
    0x5b, 0x82, 0x24, 0x52, 0x50, 0x30, 0x31, 0x08, 0x5f, 0x41, 0x44, 0x52,
    0x0c, 0x00, 0x00, 0x1c, 0x00, 0x08, 0x5f, 0x50, 0x52, 0x54, 0x12, 0x0f,
    0x01, 0x12, 0x0c, 0x04, 0x0c, 0xff, 0xff, 0x00, 0x00, 0x0a, 0x00, 0x00,
    0x0a, 0x13,
};
static const uint8_t FIX_Q35[] = {
    0x08, 0x50, 0x49, 0x43, 0x46, 0x00, 0x5b, 0x82, 0x4b, 0x05, 0x50, 0x43,
    0x49, 0x30, 0x08, 0x5f, 0x48, 0x49, 0x44, 0x0c, 0x41, 0xd0, 0x0a, 0x08,
    0x08, 0x50, 0x52, 0x54, 0x50, 0x12, 0x12, 0x01, 0x12, 0x0f, 0x04, 0x0c,
    0xff, 0xff, 0x03, 0x00, 0x0a, 0x00, 0x4c, 0x4e, 0x4b, 0x48, 0x0a, 0x00,
    0x08, 0x50, 0x52, 0x54, 0x41, 0x12, 0x12, 0x01, 0x12, 0x0f, 0x04, 0x0c,
    0xff, 0xff, 0x03, 0x00, 0x0a, 0x00, 0x47, 0x53, 0x49, 0x48, 0x0a, 0x00,
    0x14, 0x1a, 0x5f, 0x50, 0x52, 0x54, 0x00, 0xa0, 0x0c, 0x93, 0x50, 0x49,
    0x43, 0x46, 0x00, 0xa4, 0x50, 0x52, 0x54, 0x50, 0xa1, 0x06, 0xa4, 0x50,
    0x52, 0x54, 0x41, 0x5b, 0x82, 0x19, 0x47, 0x53, 0x49, 0x48, 0x08, 0x5f,
    0x43, 0x52, 0x53, 0x11, 0x0e, 0x0a, 0x0b, 0x89, 0x06, 0x00, 0x01, 0x01,
    0x17, 0x00, 0x00, 0x00, 0x79, 0x00, 0x5b, 0x82, 0x14, 0x4c, 0x4e, 0x4b,
    0x48, 0x14, 0x0e, 0x5f, 0x43, 0x52, 0x53, 0x00, 0x70, 0x50, 0x49, 0x52,
    0x51, 0x60, 0xa4, 0x60,
};

static void with_fixture(const uint8_t *fix, uint32_t len) {
    aml_reset();
    aml_parse_table(fix, len, AML_ROOT);
}

KTEST("acpi_prt", "a static _PRT names a GSI outright and a link with a constant _CRS") {
    with_fixture(FIX_STATIC, sizeof FIX_STATIC);
    uint32_t gsi = 0; int level = 0, low = 0;
    KTEST_ASSERT_EQ(acpi_prt_lookup(0, 3, 0, &gsi, &level, &low), 0);
    KTEST_ASSERT_EQ(gsi, 22);
    KTEST_ASSERT(level && low);
    KTEST_ASSERT_EQ(acpi_prt_lookup(0, 3, 1, &gsi, &level, &low), 0);
    KTEST_ASSERT_EQ(gsi, 20);
    KTEST_ASSERT(level && !low);
    KTEST_ASSERT_EQ(acpi_prt_lookup(0, 3, 2, &gsi, &level, &low), -ENOENT);
    KTEST_ASSERT_EQ(acpi_prt_lookup(0, 9, 0, &gsi, &level, &low), -ENOENT);
    aml_build();
}

KTEST("acpi_prt", "If (PICM) takes the APIC branch even though the Name says 0") {
    with_fixture(FIX_IF_PICM, sizeof FIX_IF_PICM);
    uint32_t gsi = 0; int level = 0, low = 0;
    KTEST_ASSERT_EQ(acpi_prt_lookup(0, 0x1F, 2, &gsi, &level, &low), 0);
    KTEST_ASSERT_EQ(gsi, 18);
    aml_build();
}

KTEST("acpi_prt", "a method returning ^^AR00 is followed, and a bridge's own _PRT answers for its bus") {
    with_fixture(FIX_REFERENCE, sizeof FIX_REFERENCE);
    uint32_t gsi = 0; int level = 0, low = 0;
    KTEST_ASSERT_EQ(acpi_prt_lookup(0, 2, 0, &gsi, &level, &low), 0);
    KTEST_ASSERT_EQ(gsi, 16);
    // A bus behind bridge 00:1c.0 -- only if this machine has one there,
    // since the bridge table is the real PCI's. The fixture's half is
    // still exercised above.
    const struct pci_device *br = pci_bridge_for_bus(1);
    if (br && br->bus == 0 && br->device == 0x1C && br->function == 0) {
        KTEST_ASSERT_EQ(acpi_prt_lookup(1, 0, 0, &gsi, &level, &low), 0);
        KTEST_ASSERT_EQ(gsi, 19);
    }
    aml_build();
}

KTEST("acpi_prt", "LEqual (PICF, 0) with an Else takes the APIC side, through a GSI link") {
    with_fixture(FIX_Q35, sizeof FIX_Q35);
    uint32_t gsi = 0; int level = 0, low = 0;
    KTEST_ASSERT_EQ(acpi_prt_lookup(0, 3, 0, &gsi, &level, &low), 0);
    KTEST_ASSERT_EQ(gsi, 23);
    KTEST_ASSERT(level && !low);
    aml_build();
}

KTEST("acpi_prt", "a link whose _CRS reads hardware is refused, not guessed") {
    // FIX_IF_PICM's PIC branch: PR00 through LNKC, whose _CRS is a method
    // body. Reached by making the predicate false -- a fixture with the
    // flag named PICM cannot do that, so this one spells it PR_M.
    static uint8_t fix[sizeof FIX_IF_PICM];
    k_memcpy(fix, FIX_IF_PICM, sizeof fix);
    for (uint32_t i = 0; i + 4 <= sizeof fix; i++)
        if (k_memcmp(fix + i, "PICM", 4) == 0) k_memcpy(fix + i, "PR_M", 4);
    with_fixture(fix, sizeof fix);
    uint32_t gsi = 0; int level = 0, low = 0;
    KTEST_ASSERT_EQ(acpi_prt_lookup(0, 0x1F, 2, &gsi, &level, &low), -ENOTSUP);
    aml_build();
}

KTEST("acpi_prt", "this machine's own _PRT answers for a present device, or refuses by name") {
    // Every device on bus 0 with a pin: the answer is a GSI the I/O APIC
    // has, or one of the two refusals -- never a crash and never a
    // number out of range. QEMU i440fx refuses (its links are methods),
    // q35 and both laptops answer.
    aml_build();   // this machine's tables, whatever a fixture test before this left
    int answered = 0, refused = 0;
    for (int i = 0; i < pci_device_count(); i++) {
        const struct pci_device *d = pci_device_at(i);
        if (!d->interrupt_pin) continue;
        uint32_t gsi = 0; int level = 0, low = 0;
        int rc = acpi_prt_lookup(d->bus, d->device, (uint8_t)(d->interrupt_pin - 1), &gsi, &level, &low);
        if (rc == 0) { KTEST_ASSERT(gsi < 24); answered++; }
        else { KTEST_ASSERT(rc == -ENOENT || rc == -ENOTSUP); refused++; }
    }
    KTEST_ASSERT(answered + refused > 0);
    klog_printf(KLOG_ERR "acpi_prt: %d device(s) routed by _PRT, %d refused\n", answered, refused);
}
