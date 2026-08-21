// See dirsort.h for why this is shared, and why it moves entries rather
// than handing back a permutation.
#include "lib/dirsort.h"
#include <string.h>

static int cmp(const struct dirent *a, const struct dirent *b,
               enum dirsort_key key, int reverse) {
    int r = 0;
    if (key == DIRSORT_SIZE) {
        r = (a->size < b->size) - (a->size > b->size);
    } else if (key == DIRSORT_TIME) {
        // Field by field rather than through an epoch conversion:
        // struct rtc_time is what the ABI hands over, and tz.h's
        // converter is kernel-side.
        const struct rtc_time *x = &a->modified, *y = &b->modified;
        const uint32_t ax[6] = { x->year, x->month, x->day, x->hour, x->minute, x->second };
        const uint32_t by[6] = { y->year, y->month, y->day, y->hour, y->minute, y->second };
        for (int i = 0; i < 6 && r == 0; i++)
            r = (ax[i] < by[i]) - (ax[i] > by[i]);
    }
    // Name is the default AND the tie-break, so two files written in the
    // same second still land in a stable, reproducible order.
    if (r == 0) r = strcmp(a->name, b->name);
    return reverse ? -r : r;
}

void dirsort(struct dirent *e, int n, enum dirsort_key key, int reverse) {
    // Insertion sort. n is bounded by SYS_LISTDIR_MAX and the
    // comparisons are cheap, so a quicksort would be more code than
    // either caller for a list this size. ONE scratch entry, on a ring-3
    // stack with a 2 KiB frame budget (USERLAND_CFLAGS) -- a second full
    // array would not fit.
    for (int i = 1; i < n; i++) {
        struct dirent v = e[i];
        int j = i - 1;
        while (j >= 0 && cmp(&e[j], &v, key, reverse) > 0) {
            e[j + 1] = e[j];
            j--;
        }
        e[j + 1] = v;
    }
}
