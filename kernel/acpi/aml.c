// The ACPI namespace: declarations parsed, Methods skipped, nothing
// executed. See kernel/include/kernel/aml.h for what this is and
// docs/aml-design.md for why it stops where it does.
//
// AML IS UNTRUSTED INPUT -- firmware this kernel did not write, parsed
// in ring 0. Every length, offset and name is bounds-checked against the
// table's own length, and a form this walk cannot measure makes it STOP
// rather than guess: the result is a PARTIAL namespace, which is a
// different answer from a wrong one. Same posture as ttf.c for fonts and
// usb_enum.c for descriptors.
//
// THE HARD PART IS NOT THE TREE, IT IS THE LENGTHS. A term list is a
// sequence of terms, and skipping one means knowing how long it is.
// Only some forms carry a PkgLength; `DefOpRegion` does not, and QEMU's
// own DSDT has seven of them in its root scope. So each form that may
// appear in a scope has its own rule below, and anything else is
// refused and counted.
#include "aml.h"
#include "aml_data.h"
#include "acpi.h"
#include "acpi_internal.h"
#include "klog.h"
#include "kfmt.h"
#include "string.h"

// --- opcodes ----------------------------------------------------------
#define OP_ZERO        0x00
#define OP_ONE         0x01
#define OP_ALIAS       0x06
#define OP_NAME        0x08
#define OP_BYTE        0x0A
#define OP_WORD        0x0B
#define OP_DWORD       0x0C
#define OP_STRING      0x0D
#define OP_QWORD       0x0E
#define OP_SCOPE       0x10
#define OP_BUFFER      0x11
#define OP_PACKAGE     0x12
#define OP_VAR_PACKAGE 0x13
#define OP_METHOD      0x14
#define OP_EXTERNAL    0x15
#define OP_EXT         0x5B   // the two-byte prefix
#define OP_ONES        0xFF

// Second byte of the 0x5B forms.
#define EXT_MUTEX      0x01
#define EXT_EVENT      0x02
#define EXT_OPREGION   0x80
#define EXT_FIELD      0x81
#define EXT_DEVICE     0x82
#define EXT_PROCESSOR  0x83
#define EXT_POWERRES   0x84
#define EXT_THERMAL    0x85
#define EXT_INDEXFIELD 0x86
#define EXT_BANKFIELD  0x87

// NameString structure.
#define NAME_ROOT      '\\'
#define NAME_PARENT    '^'
#define NAME_DUAL      0x2E
#define NAME_MULTI     0x2F
#define NAME_NULL      0x00

static struct aml_node g_nodes[AML_MAX_NODES];
static int g_count;
static int g_dropped;
static int g_refused;

int aml_node_count(void) { return g_count; }
int aml_dropped(void) { return g_dropped; }
int aml_refused(void) { return g_refused; }

const struct aml_node *aml_node_at(int index) {
    if (index < 0 || index >= g_count) return 0;
    return &g_nodes[index];
}

static int node_add(const char *seg, uint16_t parent, uint8_t kind,
                    const uint8_t *data, uint32_t len) {
    if (g_count >= AML_MAX_NODES) { g_dropped++; return -1; }
    struct aml_node *n = &g_nodes[g_count];
    k_memcpy(n->name, seg, 4);
    n->parent = parent;
    n->kind = kind;
    n->depth = (uint8_t)(parent < g_count ? g_nodes[parent].depth + 1 : 0);
    n->data = data;
    n->len = len;
    return g_count++;
}

int aml_child(int parent, const char *seg) {
    if (parent < 0 || parent >= g_count || !seg) return -1;
    for (int i = 0; i < g_count; i++)
        if (g_nodes[i].parent == parent && i != parent && k_memcmp(g_nodes[i].name, seg, 4) == 0)
            return i;
    return -1;
}

// --- the encodings ----------------------------------------------------

// PkgLength: the top two bits of the first byte say how many MORE bytes
// follow, and for the multi-byte forms the first byte contributes only
// its low NIBBLE. Returns 0 for a length that does not fit what remains,
// which every caller treats as "stop".
static uint32_t pkg_length(const uint8_t *p, uint32_t avail, uint32_t *used) {
    if (avail < 1) return 0;
    uint32_t extra = (uint32_t)(p[0] >> 6) & 3u;
    if (avail < extra + 1) return 0;
    uint32_t len;
    if (extra == 0) {
        len = p[0] & 0x3F;
    } else {
        len = p[0] & 0x0F;
        for (uint32_t i = 0; i < extra; i++)
            len |= (uint32_t)p[1 + i] << (4 + i * 8);
    }
    *used = extra + 1;
    return (len >= *used && len <= avail) ? len : 0;
}

// A NameString, which is the part with the most shapes. Fills `out` with
// the LAST segment (the one being declared) and reports how many bytes
// it consumed; `up` counts ParentPrefixChars and `root` says whether it
// started at the root.
//
// Returns 0 when it is malformed, 1 for a name, and 2 for the NULL NAME
// -- `Scope(\)`, which names no new object and must NOT create a node,
// or every path in the table comes out prefixed with a placeholder.
//
// Only the last segment is kept because this walk names an object by its
// declaring scope: a multi-segment path in a Scope() re-opens an
// existing branch, and stage 1 records where the declaration WAS rather
// than resolving the reference.
static int name_string(const uint8_t *p, uint32_t avail, char out[4],
                       uint32_t *used, int *up, int *root) {
    uint32_t o = 0;
    *up = 0;
    *root = 0;
    if (avail == 0) return 0;
    if (p[o] == NAME_ROOT) { *root = 1; o++; }
    while (o < avail && p[o] == NAME_PARENT) { (*up)++; o++; }
    if (o >= avail) return 0;

    uint32_t segs;
    if (p[o] == NAME_NULL) { o++; *used = o; k_memcpy(out, "____", 4); return 2; }
    if (p[o] == NAME_DUAL) { segs = 2; o++; }
    else if (p[o] == NAME_MULTI) {
        o++;
        if (o >= avail) return 0;
        segs = p[o++];
        if (segs == 0) return 0;
    } else {
        segs = 1;
    }
    if (o + segs * 4 > avail) return 0;
    // A NameSeg is a lead char then three name chars; anything else is a
    // table this walk has lost its place in.
    for (uint32_t i = 0; i < segs * 4; i++) {
        uint8_t c = p[o + i];
        int lead = (i % 4) == 0;
        int ok = (c >= 'A' && c <= 'Z') || c == '_' ||
                 (!lead && c >= '0' && c <= '9');
        if (!ok) return 0;
    }
    k_memcpy(out, p + o + (segs - 1) * 4, 4);
    o += segs * 4;
    *used = o;
    return 1;
}

// A constant DataObject's length, for skipping past a Name's value.
// Returns 0 for a form this does not know -- including an expression,
// which is legal AML and needs the interpreter this deliberately is not.
static uint32_t data_object_len(const uint8_t *p, uint32_t avail) {
    if (avail < 1) return 0;
    switch (p[0]) {  // dispatch-ok: the constant DataObject forms, a closed set
        case OP_ZERO: case OP_ONE: case OP_ONES: return 1;
        case OP_BYTE:  return avail >= 2 ? 2 : 0;
        case OP_WORD:  return avail >= 3 ? 3 : 0;
        case OP_DWORD: return avail >= 5 ? 5 : 0;
        case OP_QWORD: return avail >= 9 ? 9 : 0;
        case OP_STRING: {
            for (uint32_t i = 1; i < avail; i++) if (p[i] == 0) return i + 1;
            return 0;
        }
        case OP_BUFFER: case OP_PACKAGE: case OP_VAR_PACKAGE: {
            uint32_t used = 0;
            uint32_t len = pkg_length(p + 1, avail - 1, &used);
            return len ? 1 + len : 0;
        }
        default: return 0;
    }
}

// --- the walk ---------------------------------------------------------

static int parse_terms(const uint8_t *p, uint32_t len, uint16_t parent,
                       int depth);

// A container: PkgLength, NameString, an optional fixed trailer, then a
// term list. Returns the bytes consumed from `p` (which points at the
// PkgLength), or 0 to stop.
static uint32_t container(const uint8_t *p, uint32_t avail, uint16_t parent,
                          uint8_t kind, uint32_t trailer, int depth) {
    uint32_t plen_used = 0;
    uint32_t plen = pkg_length(p, avail, &plen_used);
    if (!plen) return 0;

    char seg[4];
    uint32_t nlen = 0;
    int up = 0, root = 0;
    int named = name_string(p + plen_used, plen - plen_used, seg, &nlen, &up, &root);
    if (!named) return 0;

    uint32_t body = plen_used + nlen + trailer;
    if (body > plen) return 0;

    // A SCOPE RE-OPENS AN EXISTING OBJECT -- `Scope (\_SB.PCI0)` adds
    // declarations to the device of that name, it does not declare a
    // second PCI0. Resolved by the full name; a scope naming nothing
    // yet known becomes a node of its own, which is what firmware
    // opening a scope before its SSDT declares it looks like.
    // `Scope(\)` re-opens the root.
    int me = -1;
    if (named == 2) me = (int)parent;
    else if (kind == AML_KIND_SCOPE) {
        struct aml_name nm;
        if (aml_name_parse(p + plen_used, plen - plen_used, &nm))
            me = aml_resolve(parent, &nm);
    }
    if (me < 0) me = node_add(seg, parent, kind, p + body, plen - body);
    // A node that did not fit still has its body walked, under its
    // parent: dropping a subtree because one name overflowed would lose
    // far more than it saves.
    parse_terms(p + body, plen - body,
                me >= 0 ? (uint16_t)me : parent, depth + 1);
    return plen;
}

// Everything that legally opens a term list, and nothing else. The
// default is REFUSE, which is what makes an unfamiliar table a partial
// namespace rather than a walk that has lost its place.
static int parse_terms(const uint8_t *p, uint32_t len, uint16_t parent,
                       int depth) {
    if (depth > 16) { g_refused++; return 0; }   // firmware nests shallowly
    uint32_t o = 0;
    while (o < len) {
        uint32_t start = o;
        uint8_t op = p[o];
        uint32_t step = 0;

        if (op == OP_SCOPE) {
            step = container(p + o + 1, len - o - 1, parent, AML_KIND_SCOPE, 0, depth);
            if (!step) { g_refused++; return 0; }
            o += 1 + step;
        } else if (op == OP_METHOD) {
            // RECORDED, NOT ENTERED. Its PkgLength is the whole reason
            // this can be a walk rather than an interpreter.
            uint32_t used = 0;
            uint32_t plen = pkg_length(p + o + 1, len - o - 1, &used);
            if (!plen) { g_refused++; return 0; }
            char seg[4];
            uint32_t nlen = 0;
            int up = 0, root = 0;
            if (name_string(p + o + 1 + used, plen - used, seg, &nlen, &up, &root))
                node_add(seg, parent, AML_KIND_METHOD, p + o + 1 + used + nlen,
                         plen - used - nlen);
            o += 1 + plen;
        } else if (op == OP_NAME) {
            char seg[4];
            uint32_t nlen = 0;
            int up = 0, root = 0;
            if (!name_string(p + o + 1, len - o - 1, seg, &nlen, &up, &root)) {
                g_refused++;
                return 0;
            }
            const uint8_t *val = p + o + 1 + nlen;
            uint32_t dlen = data_object_len(val, len - (uint32_t)(val - p));
            if (!dlen) { g_refused++; return 0; }   // an expression: not ours
            node_add(seg, parent, AML_KIND_NAME, val, dlen);
            o += 1 + nlen + dlen;
        } else if (op == OP_ALIAS || op == OP_EXTERNAL) {
            // NameString(s) then a fixed trailer. Alias is a second
            // NameString; External is two bytes.
            char seg[4];
            uint32_t nlen = 0;
            int up = 0, root = 0;
            if (!name_string(p + o + 1, len - o - 1, seg, &nlen, &up, &root)) {
                g_refused++;
                return 0;
            }
            uint32_t extra = 0;
            if (op == OP_ALIAS) {
                char seg2[4];
                uint32_t n2 = 0;
                if (!name_string(p + o + 1 + nlen, len - o - 1 - nlen, seg2,
                                 &n2, &up, &root)) { g_refused++; return 0; }
                extra = n2;
            } else {
                extra = 2;
            }
            o += 1 + nlen + extra;
        } else if (op == OP_EXT) {
            if (o + 1 >= len) { g_refused++; return 0; }
            uint8_t ext = p[o + 1];
            const uint8_t *q = p + o + 2;
            uint32_t qav = len - o - 2;
            if (ext == EXT_DEVICE || ext == EXT_THERMAL) {
                step = container(q, qav, parent,
                                 ext == EXT_DEVICE ? AML_KIND_DEVICE
                                                   : AML_KIND_THERMAL, 0, depth);
            } else if (ext == EXT_POWERRES) {
                step = container(q, qav, parent, AML_KIND_POWER, 3, depth);
            } else if (ext == EXT_PROCESSOR) {
                step = container(q, qav, parent, AML_KIND_PROCESSOR, 6, depth);
            } else if (ext == EXT_FIELD || ext == EXT_INDEXFIELD ||
                       ext == EXT_BANKFIELD) {
                // A PkgLength and a field list this walk does not model.
                uint32_t used = 0;
                step = pkg_length(q, qav, &used);
            } else if (ext == EXT_OPREGION) {
                // NO PkgLength: NameString, a space byte, then two
                // TermArgs -- constant integers in practice, and an
                // expression here is a table this stops at.
                char seg[4];
                uint32_t nlen = 0;
                int up = 0, root = 0;
                if (!name_string(q, qav, seg, &nlen, &up, &root)) {
                    g_refused++;
                    return 0;
                }
                uint32_t at = nlen + 1;              // + RegionSpace
                uint32_t a = data_object_len(q + at, qav > at ? qav - at : 0);
                if (!a) { g_refused++; return 0; }
                uint32_t b = data_object_len(q + at + a,
                                             qav > at + a ? qav - at - a : 0);
                if (!b) { g_refused++; return 0; }
                node_add(seg, parent, AML_KIND_OTHER, q, 0);
                step = at + a + b;
            } else if (ext == EXT_MUTEX || ext == EXT_EVENT) {
                char seg[4];
                uint32_t nlen = 0;
                int up = 0, root = 0;
                if (!name_string(q, qav, seg, &nlen, &up, &root)) {
                    g_refused++;
                    return 0;
                }
                node_add(seg, parent, AML_KIND_OTHER, q, 0);
                step = nlen + (ext == EXT_MUTEX ? 1u : 0u);
            } else {
                g_refused++;
                return 0;
            }
            if (!step) { g_refused++; return 0; }
            o += 2 + step;
        } else {
            g_refused++;
            return 0;
        }

        if (o <= start || o > len) { g_refused++; return 0; }  // never stall
    }
    return 1;
}

int aml_parse_table(const uint8_t *aml, uint32_t len, uint16_t parent) {
    if (!aml || !len) return 0;
    return parse_terms(aml, len, parent, 0);
}

void aml_reset(void) {
    g_count = 0;
    g_dropped = 0;
    g_refused = 0;
    k_memset(g_nodes, 0, sizeof g_nodes);
    node_add("\\___", AML_ROOT, AML_KIND_ROOT, 0, 0);
}

int aml_build(void) {
    aml_reset();

    int n = acpi_dumpable_count();
    for (int i = 0; i < n; i++) {
        const struct acpi_sdt_header *h = acpi_dumpable_at(i);
        if (!h || h->length <= sizeof *h) continue;
        if (k_memcmp(h->signature, "DSDT", 4) != 0 &&
            k_memcmp(h->signature, "SSDT", 4) != 0)
            continue;
        aml_parse_table((const uint8_t *)h + sizeof *h,
                        h->length - (uint32_t)sizeof *h, AML_ROOT);
    }
    return g_count;
}

int aml_path(int index, char *buf, uint32_t cap) {
    const struct aml_node *n = aml_node_at(index);
    if (!n || cap < 6) return 0;
    // Walk up first, then emit forwards -- the depth is bounded by the
    // recursion cap, so the stack cost is a few dozen bytes.
    int chain[20];
    int depth = 0;
    for (int i = index; i > 0 && depth < 20; i = g_nodes[i].parent)
        chain[depth++] = i;

    uint32_t o = 0;
    buf[o++] = '\\';
    for (int i = depth - 1; i >= 0; i--) {
        if (o + 5 >= cap) { buf[o] = 0; return (int)o; }
        if (i != depth - 1) buf[o++] = '.';
        for (int c = 0; c < 4; c++) buf[o++] = g_nodes[chain[i]].name[c];
    }
    buf[o] = 0;
    return (int)o;
}

static const char *kind_name(uint8_t k) {
    switch (k) {  // dispatch-ok: enum aml_kind, a closed set
        case AML_KIND_ROOT:      return "root";
        case AML_KIND_SCOPE:     return "scope";
        case AML_KIND_DEVICE:    return "device";
        case AML_KIND_NAME:      return "name";
        case AML_KIND_METHOD:    return "method";
        case AML_KIND_POWER:     return "power";
        case AML_KIND_THERMAL:   return "thermal";
        case AML_KIND_PROCESSOR: return "cpu";
        default:                 return "other";
    }
}

void aml_dump(void) {
    if (!g_count) aml_build();
    klog_printf(KLOG_ERR "aml: %d node(s), %d dropped, %d refused\n",
                g_count, g_dropped, g_refused);
    int devices = 0;
    for (int i = 0; i < g_count; i++) {
        if (g_nodes[i].kind == AML_KIND_DEVICE) devices++;
        char path[96];
        if (aml_path(i, path, sizeof path))
            klog_printf("aml:   %-7s %s\n", kind_name(g_nodes[i].kind), path);
    }
    klog_printf("aml: %d device(s)\n", devices);
}
