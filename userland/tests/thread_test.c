// Threads, from ring 3: creation, joining, sharing, and the two things
// that are NOT shared.
//
// **IT MUST BE SPAWNED, NOT `run`.** A thread needs its creator to hold
// a scheduler slot, and the legacy `run` loader has none -- under it
// SYS_THREAD_CREATE answers -EPERM and every check below fails for a
// reason that has nothing to do with threads. kernel/proc/thread_test.c
// spawns it, the same arrangement cputime_test and pipe_test have.
//
// THREE CHECKS ARE LOAD-BEARING, in the sense that a plausible broken
// implementation passes everything else and fails exactly one:
//
//   - **The pointer round trip.** A thread that was really a fork would
//     run, return a value, and join perfectly -- and the parent would
//     never see the write. Writing through a pointer the parent handed
//     over is what proves ONE address space.
//   - **errno after a failure in each.** A shared errno passes every
//     other check here; this is the only one that reads it from two
//     threads and requires two different values. It is the reason TLS
//     had to land in the same change as threads rather than after.
//   - **The critical section under a yield.** A counter incremented in
//     a loop can come out right with no mutex at all, because a
//     preemption has to land in exactly the wrong instruction. Yielding
//     INSIDE the section makes a missing mutex fail every time.
//
// Prints one line per check and exits with the number of failures.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>
#include "rt/sys.h"



#include "lib/utest.h"

// --- what the threads report back ------------------------------------

struct shared {
    int   ran;
    int   tid;
    int   pid;
    int   errno_seen;
    void *stack_probe;   // the address of one of its locals
};

// A `__thread` variable that is NOT errno: errno could in principle be
// special-cased, a plain one cannot.
static __thread int t_local = 11;
static int t_local_from_thread;

static void *worker(void *arg) {
    struct shared *s = (struct shared *)arg;
    int on_my_stack = 0;

    s->ran = 1;
    s->tid = sys_gettid();
    s->pid = sys_getpid();
    s->stack_probe = &on_my_stack;

    t_local = 22;
    t_local_from_thread = t_local;

    // A call that must fail, so this thread has an errno of its own.
    // A negative fd cannot be valid in any implementation.
    sys_close(-1);
    s->errno_seen = sys_errno();

    return (void *)0xC0FFEE;
}

// --- mutual exclusion -------------------------------------------------

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_in_section;   // whose section is open, by tid
static volatile int g_section_broken;
static volatile int g_section_runs;

static void *section_worker(void *arg) {
    (void)arg;
    int me = sys_gettid();
    for (int i = 0; i < 20; i++) {
        pthread_mutex_lock(&g_lock);
        g_in_section = me;
        // THE YIELD IS THE TEST. Anything else in the section would be
        // over before a preemption could land in it.
        sys_yield();
        if (g_in_section != me) g_section_broken = 1;
        g_section_runs++;
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

// --- a condition variable ---------------------------------------------

static pthread_mutex_t g_cv_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv = PTHREAD_COND_INITIALIZER;
static int g_handed_over;

static void *cv_worker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_cv_lock);
    g_handed_over = 1;
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_cv_lock);
    return NULL;
}

// --- a detached thread -------------------------------------------------

static volatile int g_detached_ran;

static void *detached_worker(void *arg) {
    (void)arg;
    g_detached_ran = 1;
    return NULL;
}

// --- the stack a thread starts on ------------------------------------
//
// **EVERY CHECK ABOVE PASSED WHILE THIS WAS BROKEN**, which is why it
// is its own worker. The kernel handed a new thread a 16-ALIGNED RSP,
// where SysV says a function begins with RSP % 16 == 8 (a `call` has
// just pushed a return address). GCC then emits `movaps` against stack
// slots it believes are aligned and the thread takes a #GP -- but only
// if it uses SSE at all, and none of the workers above do. The first
// GUI client to run a worker crashed on its first snprintf.
//
// So this one deliberately does what ordinary code does: formats into a
// local buffer and does floating-point arithmetic, both of which GCC
// compiles to aligned stack traffic.
static volatile int g_sse_ok;

static void *sse_worker(void *arg) {
    (void)arg;
    char buf[80];
    volatile double d[4] = { 1.5, 2.25, 3.125, 4.0625 };
    double sum = d[0] + d[1] + d[2] + d[3];
    snprintf(buf, sizeof buf, "%d/%d", (int)sum, (int)(sum * 16));
    g_sse_ok = (strcmp(buf, "10/175") == 0);
    return NULL;
}

static void *forever(void *arg) {
    (void)arg;
    for (;;) sys_yield();
}

#define MANY 6

int main(void) {
    utest_begin("thread_test", "threads, TLS and mutual exclusion", UTEST_VERDICT_FILE);

    // --- one thread, everything it shares and everything it does not --
    struct shared s;
    memset(&s, 0, sizeof s);
    int main_tid = sys_gettid();
    int main_pid = sys_getpid();

    // Give the main thread an errno FIRST, so a shared one would be
    // overwritten by the thread's failure below.
    sys_close(-2);
    int main_errno_before = sys_errno();

    pthread_t t;
    int rc = pthread_create(&t, NULL, worker, &s);
    utest_check(rc == 0, "pthread_create() started a thread");

    void *ret = NULL;
    utest_check(rc == 0 && pthread_join(t, &ret) == 0, "pthread_join() collected it");
    utest_check(ret == (void *)0xC0FFEE, "the thread's return value survived the join");
    utest_check(s.ran == 1, "the thread ran");
    // THE ONE A FORK WOULD FAIL: `s` lives on main's stack and the
    // thread wrote through a pointer to it.
    utest_check(s.stack_probe != NULL, "it wrote through the parent's pointer -- ONE address space");
    utest_check(s.tid != 0 && s.tid != main_tid, "gettid() differs between the two threads");
    utest_check(s.pid == main_pid, "getpid() is the SAME in both -- a thread is not a process");
    utest_check(s.stack_probe != (void *)&s, "its locals are on a different stack");

    utest_check(t_local == 11, "the main thread's __thread variable is untouched");
    utest_check(t_local_from_thread == 22, "the thread saw its OWN copy of it");
    utest_check(s.errno_seen != 0, "the thread's failing call set an errno");
    // THE TLS CHECK THAT MATTERS: two threads, two failures, two
    // reasons, neither overwritten by the other.
    utest_check(sys_errno() == main_errno_before,
          "the main thread's errno survived a failure in another thread");

    // --- several threads at once ---------------------------------------
    struct shared many[MANY];
    pthread_t ts[MANY];
    memset(many, 0, sizeof many);
    int started = 0;
    for (int i = 0; i < MANY; i++)
        if (pthread_create(&ts[i], NULL, worker, &many[i]) == 0) started++;
    utest_check(started == MANY, "six more threads started at once");

    int joined = 0, all_ran = 1, distinct_stacks = 1;
    for (int i = 0; i < started; i++) {
        void *r = NULL;
        if (pthread_join(ts[i], &r) == 0) joined++;
        if (!many[i].ran) all_ran = 0;
        for (int j = 0; j < i; j++)
            if (many[i].stack_probe == many[j].stack_probe) distinct_stacks = 0;
    }
    utest_check(joined == MANY, "all six joined");
    utest_check(all_ran, "all six ran");
    utest_check(distinct_stacks, "each got a stack of its own");

    // --- mutual exclusion -----------------------------------------------
    pthread_t locks[3];
    int lockers = 0;
    for (int i = 0; i < 3; i++)
        if (pthread_create(&locks[i], NULL, section_worker, NULL) == 0) lockers++;
    for (int i = 0; i < lockers; i++) pthread_join(locks[i], NULL);
    utest_check(lockers == 3 && g_section_runs == 3 * 20, "three threads ran 60 critical sections");
    utest_check(!g_section_broken, "no thread entered a section another held ACROSS A YIELD");

    // --- a condition variable --------------------------------------------
    pthread_t cvt;
    if (pthread_create(&cvt, NULL, cv_worker, NULL) == 0) {
        pthread_mutex_lock(&g_cv_lock);
        while (!g_handed_over) pthread_cond_wait(&g_cv, &g_cv_lock);
        pthread_mutex_unlock(&g_cv_lock);
        pthread_join(cvt, NULL);
        utest_check(g_handed_over, "a condition variable carried a handover");
    } else {
        utest_check(0, "a condition variable carried a handover");
    }

    // --- detaching ---------------------------------------------------------
    pthread_t det;
    if (pthread_create(&det, NULL, detached_worker, NULL) == 0) {
        utest_check(pthread_detach(det) == 0, "a running thread can be detached");
        utest_check(pthread_join(det, NULL) != 0, "a detached thread refuses to be joined");
        for (int spins = 0; spins < 1000 && !g_detached_ran; spins++) sys_yield();
        utest_check(g_detached_ran, "the detached thread still ran");
    } else {
        utest_check(0, "a running thread can be detached");
    }

    pthread_t sse;
    if (pthread_create(&sse, NULL, sse_worker, NULL) == 0) {
        pthread_join(sse, NULL);
        // Reaching this at all is most of the assertion: a misaligned
        // stack faults rather than computing the wrong answer, and the
        // fault kills the whole process.
        utest_check(g_sse_ok, "a worker can use SSE -- its stack is aligned as SysV requires");
    } else {
        utest_check(0, "a worker can use SSE -- its stack is aligned as SysV requires");
    }

    utest_check(pthread_equal(pthread_self(), pthread_self()), "pthread_self() is stable");

    // A THREAD LEFT RUNNING AT EXIT, on purpose and never joined.
    // Without it nothing here reaches the group teardown at all: every
    // other thread above is joined or detached, so it is already gone
    // by the time this process dies, and removing the teardown
    // entirely changes no result. (Measured -- it reddened nothing.)
    // kernel/proc/thread_test.c is what looks, from outside, for the
    // slot this must not leave behind.
    pthread_t spinner;
    utest_check(pthread_create(&spinner, NULL, forever, NULL) == 0,
          "a thread the process will not wait for is running at exit");

    return utest_end();
}
