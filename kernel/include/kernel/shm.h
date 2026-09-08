#ifndef KERNEL_SHM_H
#define KERNEL_SHM_H

// Named shared memory: the frames behind SYS_MMAP's MAP_SHARED, and the
// only way two ring-3 processes share writable memory.
//
// The frames are the KERNEL's, not any process's, which is what lets a
// mapper come and go without them moving. Every mapping is therefore
// BORROWED (vmm.h) -- an address-space teardown walks past them, and
// this file is what actually frees them, at the last reference.

#include <stdint.h>

struct sched_mm;

// The object behind `idx`, or -1 if there is none. Callers hold an
// index rather than a pointer because the table's slots are reused and
// a stale pointer would not say so.
int shm_lookup(const char *name);

// A NAMELESS object, for a kernel subsystem that maps both sides itself.
// It is unreachable through SYS_SHM_OPEN, which is the point: this
// namespace has no permissions, so anything a process could name it
// could also map. Returns an index holding one reference, or -errno.
int shm_create_anon(uint64_t npages);

// A NAMED object owned by `owner_pid`, made on that process's behalf.
// For a caller standing in for a process that does not exist -- the
// window-server KTEST fixture, which builds windows for fake pids whose
// pixels a real client would have allocated. Unreachable from ring 3.
// Returns an index holding one reference, or -errno.
int shm_create_named(const char *name, uint64_t npages, int owner_pid);

// Which process created `idx`, or 0. What the window server checks
// before adopting an object a client named: a client must not be able
// to hand over somebody else's memory to be mapped to the compositor.
int shm_creator(int idx);

// +1 / -1 on the object's reference count. `shm_put` frees its frames
// at zero, so every `shm_get` must be paired.
void shm_get(int idx);
void shm_put(int idx);

// How many pages the object holds, and the frame behind one page --
// what mmap_fault_in() maps. 0 for an index or page that is not there.
uint64_t shm_npages(int idx);
uint64_t shm_frame(int idx, uint64_t page);

// The mapping table, so an address space's shares are dropped when it
// dies. `shm_map_add` takes its own reference; `shm_unmap_range` drops
// every mapping of `pml4` inside [base, base + npages*4096).
int  shm_map_add(uint64_t pml4, int idx, uint64_t base, uint64_t npages);
void shm_unmap_range(uint64_t pml4, uint64_t base, uint64_t npages);
void shm_process_gone(uint64_t pml4);

#endif
