#ifndef PTHREAD_H
#define PTHREAD_H

// POSIX threads, over the kernel's four thread syscalls.
//
// **A THREAD SHARES EVERYTHING EXCEPT ITS STACK AND ITS TLS.** Same
// memory, same file descriptors, same cwd, same pid: getpid() answers
// the same value in every thread, and sys_gettid() (rt/sys.h) is what
// tells them apart.
//
// What this implementation does NOT do, stated rather than discovered:
//
//   - **A THREAD STACK HAS NO GUARD PAGE.** It is ordinary malloc'd
//     memory, so an overrun quietly walks into another allocation
//     instead of faulting. The kernel's own stacks have guard pages;
//     giving one to a thread needs an mprotect this system has no
//     syscall for (docs/roadmap.md).
//   - **A DETACHED THREAD'S STACK IS NOT RECLAIMED** until the process
//     exits -- nothing can safely free memory a dying thread is still
//     standing on, which is what Linux's CLONE_CHILD_CLEARTID futex
//     wake exists to solve. Join a thread if you want its memory back.
//   - **A MUTEX SPINS AND YIELDS** rather than blocking: the futex it
//     could park on exists, and this has not been moved onto it.
//     Correct under this preemptive scheduler, and it costs the waiter
//     its timeslice.
//   - No cancellation, no thread-specific data keys (`__thread` is what
//     to use), no scheduling attributes, no barriers or rwlocks.
//
// pthread_exit() from main() exits the PROCESS here, not just the
// initial thread -- see SYS_THREAD_EXIT in abi/syscall_abi.h.

#include <stdint.h>
#include <stddef.h>

struct pthread;
typedef struct pthread *pthread_t;

#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1

typedef struct {
    size_t stacksize;   // 0 means the default
    int    detachstate; // PTHREAD_CREATE_*
} pthread_attr_t;

typedef struct {
    volatile int locked;
    int          owner;  // tid, for the recursion check; 0 when free
} pthread_mutex_t;

#define PTHREAD_MUTEX_INITIALIZER { 0, 0 }

typedef struct {
    volatile unsigned generation;
} pthread_cond_t;

#define PTHREAD_COND_INITIALIZER { 0 }

typedef struct {
    volatile int done;
} pthread_once_t;

#define PTHREAD_ONCE_INIT { 0 }

// The default stack for a thread, in bytes. Fixed and ungrowable, where
// the initial thread's grows on fault -- so a thread that recurses
// deeply needs pthread_attr_setstacksize().
#define PTHREAD_STACK_DEFAULT (64 * 1024)
#define PTHREAD_STACK_MIN     (16 * 1024)

int  pthread_create(pthread_t *out, const pthread_attr_t *attr,
                    void *(*start)(void *), void *arg);
int  pthread_join(pthread_t t, void **retval);
int  pthread_detach(pthread_t t);
void pthread_exit(void *retval) __attribute__((noreturn));
pthread_t pthread_self(void);
int  pthread_equal(pthread_t a, pthread_t b);

int  pthread_attr_init(pthread_attr_t *a);
int  pthread_attr_destroy(pthread_attr_t *a);
int  pthread_attr_setstacksize(pthread_attr_t *a, size_t size);
int  pthread_attr_getstacksize(const pthread_attr_t *a, size_t *out);
int  pthread_attr_setdetachstate(pthread_attr_t *a, int state);
int  pthread_attr_getdetachstate(const pthread_attr_t *a, int *out);

int  pthread_mutex_init(pthread_mutex_t *m, const void *attr);
int  pthread_mutex_destroy(pthread_mutex_t *m);
int  pthread_mutex_lock(pthread_mutex_t *m);
int  pthread_mutex_trylock(pthread_mutex_t *m);
int  pthread_mutex_unlock(pthread_mutex_t *m);

int  pthread_cond_init(pthread_cond_t *c, const void *attr);
int  pthread_cond_destroy(pthread_cond_t *c);
int  pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int  pthread_cond_signal(pthread_cond_t *c);
int  pthread_cond_broadcast(pthread_cond_t *c);

int  pthread_once(pthread_once_t *once, void (*fn)(void));

#endif
