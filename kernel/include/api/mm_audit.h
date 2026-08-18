#ifndef MM_AUDIT_H
#define MM_AUDIT_H

#include <stdint.h>

// Walks every live process's page tables and compares them against the
// physical frame allocator, printing a per-process report. Returns the
// number of DANGLING mappings found -- a present mapping pointing at a
// frame pmm considers free, which is memory the allocator may hand to
// somebody else while the process is still using it. 0 is healthy.
//
// Read-only: it changes nothing, so it is safe to run at any time, from
// a shell command (`meminfo audit`) or a KTEST.
//
// It does NOT look for ordinary leaks (a used frame nothing references).
// That direction needs every kernel-side owner of a frame to declare it
// -- page tables, the heap, the kernel image, DMA buffers -- and is a
// separate job; see docs/roadmap.md.
uint64_t mm_audit_report(void);

#endif
