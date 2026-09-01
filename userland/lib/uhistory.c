// See uhistory.h. A fixed ring of recent command lines.
//
// FIXED, not allocated, even though ring 3 has malloc() now. Two
// reasons and both are about what this is: it is bounded by what a
// person will scroll back through, so a ring that drops the oldest
// entry is the correct behaviour rather than a limitation -- and
// free() never returns memory to the kernel here (sbrk cannot move
// down), so a growing history would be a process footprint that only
// climbs. `struct uhistory` is ~4 KiB, which is why both front ends
// hold it at file scope: USERLAND_CFLAGS carries -Wframe-larger-than
// and a big local array in ring 3 steps over the single guard page.
//
// It does NOT persist. The kernel shell writes /etc/history; doing the
// same here would mean two writers of one file with no locking, and
// the file is not the interesting half of this change. It is a named
// roadmap item rather than a silent omission.
#include "lib/uhistory.h"
#include <string.h>
#include <stdlib.h>

void uhist_init(struct uhistory *h) {
    h->count = 0;
    h->head = 0;
    h->browse = 0;
    // CLEARED, because uhist_add() free()s the slot it is about to
    // overwrite: they held inert bytes before and hold pointers now, so
    // a struct that is not zero-initialised would free a garbage one on
    // its first add. Today's only caller is a file-scope static, which
    // is exactly why this would fail silently and at a distance.
    for (int i = 0; i < UHIST_MAX; i++) h->entries[i] = 0;
    h->pending = 0;
}

// Ring index of the entry `back` steps behind the newest (back = 1 is
// the newest). Callers guarantee 1 <= back <= count.
static int slot(const struct uhistory *h, int back) {
    int i = h->head - back;
    while (i < 0) i += UHIST_MAX;
    return i;
}

void uhist_add(struct uhistory *h, const char *line) {
    if (!line || !line[0]) return;
    if (h->count > 0 && h->entries[slot(h, 1)] &&
        strcmp(h->entries[slot(h, 1)], line) == 0) return;

    // Allocated to the line's own length, and the slot's previous
    // occupant freed. A failure leaves the slot empty rather than
    // holding a truncated command.
    size_t n = strlen(line) + 1;
    char *copy = malloc(n);
    if (!copy) return;
    memcpy(copy, line, n);
    free(h->entries[h->head]);
    h->entries[h->head] = copy;
    h->head = (h->head + 1) % UHIST_MAX;
    if (h->count < UHIST_MAX) h->count++;
}

const char *uhist_prev(struct uhistory *h, const char *current) {
    if (h->browse >= h->count) return 0; // nothing older
    // Save what was being typed, once, on the way out of the live line.
    if (h->browse == 0) {
        free(h->pending);
        const char *cur = current ? current : "";
        size_t n = strlen(cur) + 1;
        h->pending = malloc(n);
        if (h->pending) memcpy(h->pending, cur, n);
    }
    h->browse++;
    return h->entries[slot(h, h->browse)];
}

const char *uhist_next(struct uhistory *h) {
    if (h->browse == 0) return 0; // not browsing
    h->browse--;
    if (h->browse == 0) return h->pending ? h->pending : ""; // the live line
    return h->entries[slot(h, h->browse)];
}

void uhist_reset(struct uhistory *h) {
    h->browse = 0;
    free(h->pending);   // the line the first Up staged, if the user browsed
    h->pending = 0;
}

const char *uhist_last(const struct uhistory *h) {
    if (h->count == 0) return 0;
    return h->entries[slot(h, 1)];
}
