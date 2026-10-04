// See umemcomp.h.
#include "lib/umemcomp.h"
#include "rt/sys.h"
#include <string.h>

void umem_comp_from(const struct query_meminfo *m, uint64_t apps, struct umem_comp *out) {
    memset(out, 0, sizeof *out);
    uint64_t total = m->frame_total * m->frame_bytes;
    uint64_t freeb = m->frame_free * m->frame_bytes;
    out->in_use = total > freeb ? total - freeb : 0;
    out->apps = apps;
    out->shared = m->shm_bytes;
    out->graphics = m->graphics_bytes;
    uint64_t known = out->apps + out->shared + out->graphics;
    out->kernel = out->in_use > known ? out->in_use - known : 0;
}

int umem_comp_read(struct umem_comp *out) {
    struct query_meminfo m;
    if (sys_query_record(QUERY_MEMINFO, 0, &m, sizeof m) < (int)sizeof m) return 0;
    uint64_t apps = 0;
    struct query_procmem pm;
    QUERY_FOREACH(QUERY_PROCMEM, pm, i) apps += pm.private_bytes;
    umem_comp_from(&m, apps, out);
    return 1;
}
