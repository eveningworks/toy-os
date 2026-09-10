// AML stage 2 -- see kernel/include/kernel/aml_data.h. Every read is
// bounded by `avail`: this parses firmware, in ring 0.
#include "aml_data.h"
#include "aml.h"
#include "string.h"

#define OP_ZERO 0x00
#define OP_ONE 0x01
#define OP_ONES 0xFF
#define OP_BYTE 0x0A
#define OP_WORD 0x0B
#define OP_DWORD 0x0C
#define OP_QWORD 0x0E
#define OP_STRING 0x0D
#define OP_BUFFER 0x11
#define OP_PACKAGE 0x12
#define OP_VAR_PACKAGE 0x13

int aml_is_name_lead(uint8_t c) {
    return c == 0x5C || c == 0x5E || c == 0x2E || c == 0x2F || c == 0x5F ||
           (c >= 'A' && c <= 'Z');
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

uint32_t aml_name_parse(const uint8_t *p, uint32_t avail, struct aml_name *out) {
    uint32_t o = 0;
    k_memset(out, 0, sizeof *out);
    if (avail == 0) return 0;
    if (p[o] == 0x5C) { out->root = 1; o++; }
    while (o < avail && p[o] == 0x5E) { out->ups++; o++; }
    if (o >= avail) return 0;
    uint32_t segs;
    if (p[o] == 0x00) { o++; return o; }              // the null name
    if (p[o] == 0x2E) { segs = 2; o++; }
    else if (p[o] == 0x2F) { o++; if (o >= avail) return 0; segs = p[o++]; }
    else segs = 1;
    if (segs == 0 || segs > 8 || o + segs * 4 > avail) return 0;
    for (uint32_t i = 0; i < segs * 4; i++) {
        uint8_t c = p[o + i];
        int lead = (i % 4) == 0;
        if (!((c >= 'A' && c <= 'Z') || c == '_' || (!lead && c >= '0' && c <= '9'))) return 0;
    }
    for (uint32_t s = 0; s < segs; s++) k_memcpy(out->seg[s], p + o + s * 4, 4);
    out->nsegs = (int)segs;
    return o + segs * 4;
}

uint32_t aml_int(const uint8_t *p, uint32_t avail, uint64_t *out) {
    if (avail < 1) return 0;
    switch (p[0]) {  // dispatch-ok: the integer constant forms, a closed set
        case OP_ZERO: *out = 0; return 1;
        case OP_ONE:  *out = 1; return 1;
        case OP_ONES: *out = ~0ull; return 1;
        case OP_BYTE:  if (avail < 2) return 0; *out = p[1]; return 2;
        case OP_WORD:  if (avail < 3) return 0; *out = (uint64_t)p[1] | ((uint64_t)p[2] << 8); return 3;
        case OP_DWORD: if (avail < 5) return 0;
            *out = (uint64_t)p[1] | ((uint64_t)p[2] << 8) | ((uint64_t)p[3] << 16) | ((uint64_t)p[4] << 24);
            return 5;
        case OP_QWORD: if (avail < 9) return 0;
            *out = 0;
            for (int i = 7; i >= 0; i--) *out = (*out << 8) | p[1 + i];
            return 9;
        default: return 0;
    }
}

uint32_t aml_object_len(const uint8_t *p, uint32_t avail) {
    if (avail < 1) return 0;
    uint64_t v;
    uint32_t n = aml_int(p, avail, &v);
    if (n) return n;
    switch (p[0]) {  // dispatch-ok: the constant DataObject forms, a closed set
        case OP_STRING:
            for (uint32_t i = 1; i < avail; i++) if (p[i] == 0) return i + 1;
            return 0;
        case OP_BUFFER: case OP_PACKAGE: case OP_VAR_PACKAGE: {
            uint32_t used = 0;
            uint32_t len = pkg_length(p + 1, avail - 1, &used);
            return len ? 1 + len : 0;
        }
        default: {
            if (!aml_is_name_lead(p[0])) return 0;
            struct aml_name nm;
            return aml_name_parse(p, avail, &nm);
        }
    }
}

int aml_package(const uint8_t *p, uint32_t avail, uint32_t *count,
                const uint8_t **elems, uint32_t *elems_avail) {
    if (avail < 3 || p[0] != OP_PACKAGE) return 0;
    uint32_t used = 0;
    uint32_t len = pkg_length(p + 1, avail - 1, &used);
    if (!len || len < used + 1) return 0;
    *count = p[1 + used];
    *elems = p + 1 + used + 1;
    *elems_avail = len - used - 1;
    return 1;
}

int aml_buffer(const uint8_t *p, uint32_t avail, const uint8_t **bytes, uint32_t *n) {
    if (avail < 3 || p[0] != OP_BUFFER) return 0;
    uint32_t used = 0;
    uint32_t len = pkg_length(p + 1, avail - 1, &used);
    if (!len) return 0;
    uint64_t size;
    uint32_t su = aml_int(p + 1 + used, len - used, &size);
    if (!su) return 0;                                   // a computed size
    uint32_t have = len - used - su;
    if (size > have) return 0;
    *bytes = p + 1 + used + su;
    *n = (uint32_t)size;
    return 1;
}

int aml_resolve(int scope, const struct aml_name *name) {
    if (scope < 0 || !name || name->nsegs == 0) return -1;
    int at = scope;
    if (name->root) at = AML_ROOT;
    for (int i = 0; i < name->ups; i++) {
        const struct aml_node *n = aml_node_at(at);
        if (!n || at == AML_ROOT) return -1;
        at = n->parent;
    }
    // A bare single segment searches upward; anything qualified does not.
    if (name->nsegs == 1 && !name->root && !name->ups) {
        for (;;) {
            int c = aml_child(at, name->seg[0]);
            if (c >= 0) return c;
            if (at == AML_ROOT) return -1;
            const struct aml_node *n = aml_node_at(at);
            if (!n) return -1;
            at = n->parent;
        }
    }
    for (int s = 0; s < name->nsegs; s++) {
        at = aml_child(at, name->seg[s]);
        if (at < 0) return -1;
    }
    return at;
}

int aml_crs_irq(const uint8_t *buf, uint32_t n, uint32_t *irq, int *level, int *low) {
    uint32_t i = 0;
    while (i < n) {
        uint8_t t = buf[i];
        if (t & 0x80) {                                  // a large descriptor
            if (i + 3 > n) return 0;
            uint32_t ln = (uint32_t)buf[i + 1] | ((uint32_t)buf[i + 2] << 8);
            if (i + 3 + ln > n) return 0;
            if (t == 0x89 && ln >= 6) {                  // Extended Interrupt
                uint8_t flags = buf[i + 3];
                *irq = (uint32_t)buf[i + 5] | ((uint32_t)buf[i + 6] << 8) |
                       ((uint32_t)buf[i + 7] << 16) | ((uint32_t)buf[i + 8] << 24);
                *level = !(flags & 2);
                *low = !!(flags & 4);
                return 1;
            }
            i += 3 + ln;
        } else {
            uint32_t ln = t & 7;
            if (i + 1 + ln > n) return 0;
            if ((t >> 3) == 0x04 && ln >= 2) {           // IRQ descriptor
                uint16_t mask = (uint16_t)(buf[i + 1] | (buf[i + 2] << 8));
                for (uint32_t k = 0; k < 16; k++)
                    if (mask & (1u << k)) {
                        *irq = k;
                        // Byte 3, when present: bit 0 level? no -- bit 0 is
                        // EDGE (1) / LEVEL (0), bit 3 active-low.
                        *level = ln >= 3 ? !(buf[i + 3] & 1) : 0;
                        *low = ln >= 3 ? !!(buf[i + 3] & 8) : 0;
                        return 1;
                    }
            }
            if ((t >> 3) == 0x0F) return 0;              // End Tag
            i += 1 + ln;
        }
    }
    return 0;
}
