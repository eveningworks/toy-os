// POSIX threads over SYS_THREAD_*. See <pthread.h> for what this does
// not do; this file is the mechanism.
//
// THE DIVISION OF LABOUR IS THE POINT: the kernel starts a thread on a
// stack it is handed and tells you when one has died, and everything
// with a malloc in it lives here -- the stack, the TLS block, the
// return value, the descriptor. That is why the kernel's half is four
// small syscalls rather than a threading library in ring 0.
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "rt/sys.h"

struct pthread {
    int    tid;
    void *(*start)(void *);
    void  *arg;
    void  *retval;
    void  *stack;    // the malloc'd block, NULL for the initial thread
    void  *tls;      // ditto
    int    detached;
};

// WHICH THREAD AM I -- the first real use of `__thread` in this system
// beyond errno, and the reason pthread_self() and pthread_exit() need no
// table lookup. NULL in the initial thread, which never went through
// thread_entry().
static __thread struct pthread *g_self;

// The initial thread's descriptor. Static because it must exist before
// any allocation can, and because there is exactly one.
static struct pthread g_main;

static void thread_entry(void *p) {
    struct pthread *t = (struct pthread *)p;
    g_self = t;
    t->retval = t->start(t->arg);
    // The exit CODE is not the return value: a void* does not fit an
    // int, so the value goes in the descriptor and pthread_join() reads
    // it from there. The kernel only ever carries "it finished".
    sys_thread_exit(0);
}

// A block of `size` bytes aligned to 16, plus the pointer to free.
// malloc's own alignment is not part of its contract here, and a TLS
// block placed at an odd address puts every `__thread` variable at one.
static void *aligned_block(size_t size, void **out_raw) {
    char *raw = (char *)malloc(size + 16);
    if (!raw) return NULL;
    *out_raw = raw;
    uintptr_t a = ((uintptr_t)raw + 15) & ~(uintptr_t)15;
    return (void *)a;
}

int pthread_create(pthread_t *out, const pthread_attr_t *attr,
                   void *(*start)(void *), void *arg) {
    if (!out || !start) return EINVAL;

    size_t stacksize = PTHREAD_STACK_DEFAULT;
    int detached = PTHREAD_CREATE_JOINABLE;
    if (attr) {
        if (attr->stacksize) stacksize = attr->stacksize;
        detached = attr->detachstate;
    }
    if (stacksize < PTHREAD_STACK_MIN) stacksize = PTHREAD_STACK_MIN;

    struct pthread *t = (struct pthread *)malloc(sizeof *t);
    if (!t) return EAGAIN;
    memset(t, 0, sizeof *t);
    t->start = start;
    t->arg = arg;
    t->detached = detached == PTHREAD_CREATE_DETACHED;

    void *stack_raw = NULL;
    char *stack = (char *)aligned_block(stacksize, &stack_raw);
    if (!stack) { free(t); return EAGAIN; }
    t->stack = stack_raw;

    void *tls_raw = NULL;
    void *tls_mem = aligned_block((size_t)rt_tls_size(), &tls_raw);
    if (!tls_mem) { free(stack_raw); free(t); return EAGAIN; }
    t->tls = tls_raw;
    // EVERY THREAD GETS ITS OWN COPY OF EVERY `__thread` VARIABLE,
    // initialised from the program's template -- which is what makes
    // errno per thread and g_self above possible at all.
    void *tp = rt_tls_install(tls_mem);

    // The stack grows DOWN from the top of the block.
    int tid = sys_thread_create(thread_entry, stack + stacksize, t, tp,
                                t->detached);
    if (tid < 0) {
        free(tls_raw); free(stack_raw); free(t);
        return EAGAIN;
    }
    t->tid = tid;
    *out = t;
    return 0;
}

int pthread_join(pthread_t t, void **retval) {
    if (!t || t == &g_main) return EINVAL;
    if (t->detached) return EINVAL;
    if (sys_thread_join(t->tid) < 0) return ESRCH;
    if (retval) *retval = t->retval;
    // Safe HERE and nowhere earlier: the kernel has reported the thread
    // reaped, so nothing is standing on this stack any more.
    free(t->tls);
    free(t->stack);
    free(t);
    return 0;
}

int pthread_detach(pthread_t t) {
    if (!t || t == &g_main || t->detached) return EINVAL;
    if (sys_thread_detach(t->tid) < 0) return ESRCH;
    t->detached = 1;
    // The stack, the TLS and this descriptor are deliberately LEAKED --
    // see <pthread.h>. Nothing here can know when the thread has
    // stopped using its own stack.
    return 0;
}

void pthread_exit(void *retval) {
    if (g_self) g_self->retval = retval;
    sys_thread_exit(0); // from the initial thread this exits the process
    for (;;) { }        // unreachable; keeps the noreturn promise visible
}

pthread_t pthread_self(void) {
    return g_self ? g_self : &g_main;
}

int pthread_equal(pthread_t a, pthread_t b) { return a == b; }

int pthread_attr_init(pthread_attr_t *a) {
    if (!a) return EINVAL;
    a->stacksize = 0;
    a->detachstate = PTHREAD_CREATE_JOINABLE;
    return 0;
}
int pthread_attr_destroy(pthread_attr_t *a) { return a ? 0 : EINVAL; }

int pthread_attr_setstacksize(pthread_attr_t *a, size_t size) {
    if (!a || size < PTHREAD_STACK_MIN) return EINVAL;
    a->stacksize = size;
    return 0;
}
int pthread_attr_getstacksize(const pthread_attr_t *a, size_t *out) {
    if (!a || !out) return EINVAL;
    *out = a->stacksize ? a->stacksize : PTHREAD_STACK_DEFAULT;
    return 0;
}
int pthread_attr_setdetachstate(pthread_attr_t *a, int state) {
    if (!a || (state != PTHREAD_CREATE_JOINABLE &&
               state != PTHREAD_CREATE_DETACHED)) return EINVAL;
    a->detachstate = state;
    return 0;
}
int pthread_attr_getdetachstate(const pthread_attr_t *a, int *out) {
    if (!a || !out) return EINVAL;
    *out = a->detachstate;
    return 0;
}

// --- mutexes ---------------------------------------------------------
//
// A TEST-AND-SET THAT YIELDS, not a futex: `SYS_FUTEX_WAIT` exists
// (kernel/proc/futex.c) and this has not been moved onto it yet
// (docs/roadmap.md). Correct under a preemptive
// round-robin scheduler -- the holder is always eventually run, so a
// waiter always eventually gets in -- and the cost is that a waiter
// spends its slice asking. The acquire/release ordering is real work
// even so: x86-64 is TSO, but the COMPILER reorders freely, and the
// builtins are what stop it hoisting the critical section out.

int pthread_mutex_init(pthread_mutex_t *m, const void *attr) {
    (void)attr;
    if (!m) return EINVAL;
    m->locked = 0;
    m->owner = 0;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *m) {
    if (!m) return EINVAL;
    if (m->locked) return EBUSY;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *m) {
    if (!m) return EINVAL;
    int me = sys_gettid();
    // NOT RECURSIVE, and it says so rather than deadlocking: a thread
    // that locks what it already holds gets EDEADLK, which is what
    // POSIX specifies for a default mutex that can detect it.
    if (m->locked && m->owner == me) return EDEADLK;
    while (__atomic_exchange_n(&m->locked, 1, __ATOMIC_ACQUIRE))
        sys_yield();
    m->owner = me;
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *m) {
    if (!m) return EINVAL;
    if (__atomic_exchange_n(&m->locked, 1, __ATOMIC_ACQUIRE)) return EBUSY;
    m->owner = sys_gettid();
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *m) {
    if (!m) return EINVAL;
    if (!m->locked) return EPERM;
    m->owner = 0;
    __atomic_store_n(&m->locked, 0, __ATOMIC_RELEASE);
    return 0;
}

// --- condition variables ---------------------------------------------
//
// A GENERATION COUNTER, and reading it BEFORE the unlock is what makes
// the wait race-free: a signal that lands in the window between
// unlocking and looping has already bumped the counter the waiter
// sampled, so the waiter sees it and does not sleep through it. That is
// the same lost-wakeup argument the kernel's own wait channels make.

int pthread_cond_init(pthread_cond_t *c, const void *attr) {
    (void)attr;
    if (!c) return EINVAL;
    c->generation = 0;
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *c) { return c ? 0 : EINVAL; }

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    if (!c || !m) return EINVAL;
    unsigned seen = __atomic_load_n(&c->generation, __ATOMIC_ACQUIRE);
    int rc = pthread_mutex_unlock(m);
    if (rc) return rc;
    while (__atomic_load_n(&c->generation, __ATOMIC_ACQUIRE) == seen)
        sys_yield();
    return pthread_mutex_lock(m);
}

// SIGNAL AND BROADCAST ARE THE SAME CALL, and that is allowed: POSIX
// lets an implementation wake more waiters than asked, which is why
// every correct use of a condition variable re-tests its predicate in a
// loop. Waking exactly one needs per-waiter state, which this would
// have to grow before a futex could wake a chosen waiter.
int pthread_cond_signal(pthread_cond_t *c) {
    if (!c) return EINVAL;
    __atomic_add_fetch(&c->generation, 1, __ATOMIC_RELEASE);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *c) { return pthread_cond_signal(c); }

int pthread_once(pthread_once_t *once, void (*fn)(void)) {
    if (!once || !fn) return EINVAL;
    // 0 = not run, 1 = running, 2 = done.
    int expected = 0;
    if (__atomic_compare_exchange_n(&once->done, &expected, 1, 0,
                                     __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) {
        fn();
        __atomic_store_n(&once->done, 2, __ATOMIC_RELEASE);
        return 0;
    }
    while (__atomic_load_n(&once->done, __ATOMIC_ACQUIRE) != 2) sys_yield();
    return 0;
}
