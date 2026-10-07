#ifndef KSLOTS_H
#define KSLOTS_H

// A table of same-sized kernel objects addressed by a small, stable
// INDEX, grown on demand -- what pipes, sockets and TCP connections are
// kept in. An fd stores the index, never a pointer, so a stale or forged
// fd cannot dereference anything.
//
// THREE PROMISES, and the callers lean on each:
//   - An object's ADDRESS never moves. Each is its own allocation, so a
//     pointer to one (a pipe's address is its wait channel) survives the
//     table growing.
//   - A freed slot's MEMORY stays valid and is reused, never returned to
//     the heap: code preempted while holding a pointer to a socket that
//     is then closed reads a dead object, not freed memory. The table
//     therefore sits at its high-water mark, as a slab cache that never
//     shrinks does.
//   - kslots_at() is safe against a concurrent grow: the new index
//     arrays are published before the new capacity, and the old arrays
//     are never freed (together under twice the final size).
//
// Allocation and freeing take the preemption guard -- the heap's own
// lock -- so they may be called from any context that may kmalloc.
#include <stdint.h>

struct kslots {
    void **obj;        // obj[i]: slot i's memory, NULL until first used
    uint8_t *live;     // live[i]: in use
    int cap;           // entries in both arrays
    uint32_t size;     // bytes per object
};

#define KSLOTS_INIT(bytes) { 0, 0, 0, (bytes) }

// A ZEROED object in a free slot, growing the table if none is free.
// Returns its index, or -1 when the heap is out.
int kslots_alloc(struct kslots *t);

// The live object at `i`, or NULL when `i` is free or out of range.
void *kslots_at(const struct kslots *t, int i);

// Marks `i` free. Its memory is kept for the next kslots_alloc().
void kslots_free(struct kslots *t, int i);

// How many indices exist -- walk 0..kslots_cap()-1 with kslots_at().
int kslots_cap(const struct kslots *t);

// How many are live.
int kslots_live(const struct kslots *t);

#endif
