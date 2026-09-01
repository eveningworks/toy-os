// See kversion.h.
//
// THE ONLY FILE THAT INCLUDES build_stamp.h. That header is regenerated
// on every build, so every build rebuilds this object and relinks --
// and nothing else. It is Linux's init/version.c, for the same reason
// and at the same cost.
#include "kversion.h"
#include "query.h"
#include "string.h"
#include "kfmt.h"
#include "version.h"      // TOYOS_VERSION, TOYOS_BUILD_ID
#include "build_stamp.h"  // TOYOS_BUILD_STAMP -- and NOWHERE else
#include <stddef.h>

const char *kversion_banner(void) {
    return "toy-os " TOYOS_VERSION " (" TOYOS_BUILD_ID ") built " TOYOS_BUILD_STAMP;
}

static int version_count(void) { return 1; } // scalar

static int version_fill(int index, void *out) {
    if (index != 0) return 0;
    struct query_version *v = out;
    k_memset(v, 0, sizeof *v);
    k_strlcpy(v->version,  TOYOS_VERSION,      sizeof v->version);
    k_strlcpy(v->build_id, TOYOS_BUILD_ID,     sizeof v->build_id);
    k_strlcpy(v->stamp,    TOYOS_BUILD_STAMP,  sizeof v->stamp);
    return 1;
}

// No numeric fields: everything here is a string, and QUERY_OP_FIELD_GET
// only answers scalars. A caller reads the record.
static const struct query_provider version_provider = {
    .cls = QUERY_VERSION,
    .name = "version",
    .record_size = sizeof(struct query_version),
    .flags = 0, // scalar
    .count = version_count,
    .fill = version_fill,
    .fields = NULL,
    .field_count = 0,
};

void kversion_query_init(void) {
    query_register(&version_provider);
}
