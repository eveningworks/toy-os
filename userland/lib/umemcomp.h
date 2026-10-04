#ifndef ULIB_UMEMCOMP_H
#define ULIB_UMEMCOMP_H

// Where the memory in use is, as four rows that SUM to it: Apps (every
// process's private bytes, QUERY_PROCMEM), Shared (shm objects, each once), Graphics
// (RAM held for the screen) and Kernel (the rest -- heap, page tables,
// drivers, caches). Windows' "Memory composition"; Task Manager and
// `meminfo` both read it from here.
//
// KERNEL IS THE REMAINDER, so it absorbs whatever the other three miss,
// and is clamped at 0 because the reads behind it are not atomic.
#include <stdint.h>
#include "query_abi.h"

struct umem_comp {
    uint64_t in_use;
    uint64_t apps, shared, graphics, kernel;
};

// From a QUERY_MEMINFO record and the caller's own sum of every
// QUERY_PROCMEM record's private_bytes -- for a caller that already read
// that list, as Task Manager has.
void umem_comp_from(const struct query_meminfo *m, uint64_t apps, struct umem_comp *out);

// Both reads done here. 1 on success, 0 when the kernel answered short.
int umem_comp_read(struct umem_comp *out);

#endif
