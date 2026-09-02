// The query registry: what FACTS the kernel can report about itself.
//
// The design and the reasoning are in docs/query-design.md; the header
// (api/query.h) carries the contract. What is here is the table, the
// name resolution, and the one read path everything else goes through.
//
// THE ONE READER. query_read() is called by the syscall, by the kernel
// shell's `meminfo`, and by SYS_SYSINFO -- so the ring-0 and ring-3
// answers to the same question are not two readers that agree, they are
// one function. That is what makes "both report the same numbers" true
// by construction instead of by a test that catches drift afterwards.
#include "query.h"
#include "errno.h"
#include "string.h"
#include "klog.h"
#include "kfmt.h" // klog_printf
#include <stddef.h>
#include "initcall.h"

static const struct query_provider *g_providers[QUERY_MAX];
static int g_count;

// ---- the registry describing itself ---------------------------------
//
// Class 0, and a LIST. Two things fall out of it that are worth having
// on the first commit rather than the fifth: "what facts exist?" is
// answered by the same syscall as every other question, needing no
// second mechanism; and the list path (count/index) has a real caller,
// rather than being a half nothing exercises -- which by this project's
// own rule is a half nobody has validated.
static int providers_count(void) { return g_count; }

static int providers_fill(int index, void *out) {
    if (index < 0 || index >= g_count) return 0;
    const struct query_provider *p = g_providers[index];
    struct query_provider_info *info = out;
    k_memset(info, 0, sizeof *info);
    info->cls = p->cls;
    info->record_size = p->record_size;
    // ASKED, not cached: a list's length is a fact in its own right and
    // changes between calls. A stored count would be stale the moment
    // anything registered a process or freed a frame.
    info->count = (uint32_t)(p->count ? p->count() : 1);
    info->flags = p->flags;
    k_strlcpy(info->name, p->name, sizeof info->name);
    return 1;
}

static const struct query_provider g_providers_provider = {
    .cls = QUERY_PROVIDERS,
    .name = "providers",
    .record_size = sizeof(struct query_provider_info),
    .flags = QUERY_F_LIST,
    .count = providers_count,
    .fill = providers_fill,
    // No named fields: a LIST class is deliberately not addressable as
    // one value. See api/query.h's `fields`.
    .fields = NULL,
    .field_count = 0,
};

// ---- registration ----------------------------------------------------

int query_register(const struct query_provider *p) {
    if (!p || !p->name || !p->name[0] || !p->fill || !p->record_size) {
        klog_write("query: refused a malformed provider\n");
        return 0;
    }
    if (g_count >= QUERY_MAX) {
        klog_write("query: registry full -- provider refused\n");
        return 0;
    }
    // A DUPLICATE CLASS IS REFUSED, not last-wins and not first-wins.
    // Two providers for one class would be resolved by registration
    // order, i.e. by boot sequence, which makes the answer depend on
    // link order -- the same refusal the settings registry makes for a
    // duplicate (namespace, name), and for the same reason.
    for (int i = 0; i < g_count; i++) {
        if (g_providers[i]->cls == p->cls) {
            klog_printf("query: class %u already registered -- '%s' refused\n",
                        p->cls, p->name);
            return 0;
        }
    }
    // Stored by POINTER, not copied -- see api/query.h's trap note.
    g_providers[g_count++] = p;
    return 1;
}

void query_unregister(const struct query_provider *p) {
    for (int i = 0; i < g_count; i++) {
        if (g_providers[i] != p) continue;
        // Shift down rather than leave a hole: query_at() walks
        // 0..count-1, and a NULL in the middle would end the walk early
        // for everything registered after it.
        for (int j = i; j < g_count - 1; j++) g_providers[j] = g_providers[j + 1];
        g_count--;
        return;
    }
}

int query_count(void) { return g_count; }

const struct query_provider *query_at(int index) {
    if (index < 0 || index >= g_count) return NULL;
    return g_providers[index];
}

const struct query_provider *query_find(uint32_t cls) {
    for (int i = 0; i < g_count; i++)
        if (g_providers[i]->cls == cls) return g_providers[i];
    return NULL;
}

const struct query_provider *query_find_by_name(const char *name) {
    if (!name || !name[0]) return NULL;
    for (int i = 0; i < g_count; i++)
        if (k_strcmp(g_providers[i]->name, name) == 0) return g_providers[i];
    return NULL;
}

// ---- reading ---------------------------------------------------------

int query_read(uint32_t cls, int index, void *out, uint32_t cap) {
    const struct query_provider *p = query_find(cls);
    if (!p) return -ENOENT;
    if (!out || cap < p->record_size) return -EINVAL;
    if (index < 0) return -ERANGE;
    if (!p->fill(index, out)) return -ERANGE;
    return (int)p->record_size;
}

// Splits "provider.field" at the FIRST dot. Returns 0 on a malformed
// name. The first dot rather than the last because a provider name never
// contains one and a field name might later (a nested record), so this
// stays right when that happens.
static int split_qualified(const char *q, char *prov, size_t prov_cap,
                           const char **field) {
    if (!q || !q[0]) return 0;
    size_t i = 0;
    while (q[i] && q[i] != '.') i++;
    if (q[i] != '.' || i == 0 || i >= prov_cap) return 0;
    if (!q[i + 1]) return 0;
    for (size_t j = 0; j < i; j++) prov[j] = q[j];
    prov[i] = '\0';
    *field = q + i + 1;
    return 1;
}

int query_field_get(const char *qualified, uint64_t *out_value, uint32_t *out_type) {
    char prov[QUERY_NAME_MAX];
    const char *field = NULL;
    if (!out_value || !out_type) return -EINVAL;
    if (!split_qualified(qualified, prov, sizeof prov, &field)) return -EINVAL;

    const struct query_provider *p = query_find_by_name(prov);
    if (!p) return -ENOENT;
    if (!p->fields || !p->field_count) {
        // The class exists and has no single value -- it is a table.
        // A DIFFERENT ANSWER from "no such fact": one sends the reader
        // to a tool that can show it, the other sends them hunting for
        // a typo they did not make.
        return -ENOTSUP;
    }
    for (uint32_t i = 0; i < p->field_count; i++) {
        if (k_strcmp(p->fields[i].name, field) != 0) continue;

        // Read the whole record into a local and take the field out of
        // it. Not a pointer into provider-owned memory: a provider
        // COMPUTES its record, so there is nothing stable to point at.
        // The record is small by construction (api/query.h) -- this is
        // the one place that bounds it.
        uint8_t rec[QUERY_RECORD_MAX];
        if (p->record_size > sizeof rec) return -EINVAL;
        if (!p->fill(0, rec)) return -ERANGE;

        uint64_t v = 0;
        k_memcpy(&v, rec + p->fields[i].offset, sizeof v);
        *out_value = v;
        *out_type = p->fields[i].type;
        return 0;
    }
    return -ENOENT;
}

void query_init(void) {
    query_register(&g_providers_provider);
}
INITCALL(query_init, INIT_QUERY);
