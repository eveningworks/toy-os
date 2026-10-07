// A grow-on-demand table of kernel objects -- kernel/include/kernel/kslots.h.
#include "kslots.h"
#include "heap.h"
#include "string.h"
#include "scheduler.h"

#define KSLOTS_FIRST 8

// Doubles both index arrays. The new ones are fully written before they
// are published, and the capacity is published LAST, so a reader seeing
// the new capacity sees the new arrays; one seeing the old capacity is
// safe with either. The old arrays are kept, never freed: a reader
// preempted between loading the array and indexing it still holds one.
static int grow(struct kslots *t) {
    int cap = t->cap ? t->cap * 2 : KSLOTS_FIRST;
    void **obj = kmalloc((uint32_t)cap * sizeof *obj);
    uint8_t *live = kmalloc((uint32_t)cap);
    if (!obj || !live) {
        if (obj) kfree(obj);
        if (live) kfree(live);
        return 0;
    }
    k_memset(obj, 0, (uint32_t)cap * sizeof *obj);
    k_memset(live, 0, (uint32_t)cap);
    if (t->cap) {
        k_memcpy(obj, t->obj, (uint32_t)t->cap * sizeof *obj);
        k_memcpy(live, t->live, (uint32_t)t->cap);
    }
    __atomic_store_n(&t->obj, obj, __ATOMIC_RELEASE);
    __atomic_store_n(&t->live, live, __ATOMIC_RELEASE);
    __atomic_store_n(&t->cap, cap, __ATOMIC_RELEASE);
    return 1;
}

int kslots_alloc(struct kslots *t) {
    scheduler_preempt_disable();
    int i = 0;
    while (i < t->cap && t->live[i]) i++;
    if (i == t->cap && !grow(t)) { scheduler_preempt_enable(); return -1; }
    if (!t->obj[i] && !(t->obj[i] = kmalloc(t->size))) {
        scheduler_preempt_enable();
        return -1;
    }
    k_memset(t->obj[i], 0, t->size);
    t->live[i] = 1;
    scheduler_preempt_enable();
    return i;
}

void *kslots_at(const struct kslots *t, int i) {
    int cap = __atomic_load_n(&t->cap, __ATOMIC_ACQUIRE);
    if (i < 0 || i >= cap) return 0;
    const uint8_t *live = __atomic_load_n(&t->live, __ATOMIC_ACQUIRE);
    void *const *obj = __atomic_load_n(&t->obj, __ATOMIC_ACQUIRE);
    return live[i] ? obj[i] : 0;
}

void kslots_free(struct kslots *t, int i) {
    scheduler_preempt_disable();
    if (i >= 0 && i < t->cap) t->live[i] = 0;
    scheduler_preempt_enable();
}

int kslots_cap(const struct kslots *t) { return __atomic_load_n(&t->cap, __ATOMIC_ACQUIRE); }

int kslots_live(const struct kslots *t) {
    int n = 0;
    for (int i = 0; i < kslots_cap(t); i++) n += kslots_at(t, i) != 0;
    return n;
}
