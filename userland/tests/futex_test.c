// Does a futex actually park a process and does a wake actually release
// it -- ACROSS TWO PROCESSES, through a shared-memory page.
//
// The KTESTs beside sys_futex_wait() cover the key derivation and every
// refusal, and cannot cover this: they run on the kernel context, which
// has nobody to be woken by. What is only testable from here is the
// round trip, and one property inside it that nothing else can see --
// **the wake finds a waiter that mapped the word at a DIFFERENT
// ADDRESS.** A futex keyed on the caller's own pointer passes every
// KTEST and returns 0 here, having woken nobody.
//
// The child is this same binary re-spawned with an argument, which is
// how a /tests program gets a second process without a fork.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "rt/sys.h"
#include "syscall_abi.h"
#include "lib/utest.h"

#define SHM_NAME "futextest"
#define WAIT_MS  8000

// The shared page: a word the two processes hand back and forth.
struct page {
    volatile uint32_t word;   // 0 = child not yet parked, 1 = parent's
                               // wake, 2 = child acknowledging it
    volatile uint32_t ready;  // the child sets this before it parks
    // Where each side MAPPED this page. Compared rather than assumed:
    // the discriminating claim below is that a wake crosses two
    // different addresses, and two runs of ONE binary map at the same
    // one unless something makes them differ -- a first version of this
    // test passed with the kernel keying on the virtual address.
    volatile uint64_t parent_at;
    volatile uint64_t child_at;
};

static struct page *map_shared(int create) {
    int fd = sys_shm_open(SHM_NAME, create ? 4096 : 0,
                          create ? SHM_CREATE : 0);
    if (fd < 0) return 0;
    void *p = sys_mmap(0, 4096, SYS_PROT_READ | SYS_PROT_WRITE,
                       SYS_MAP_SHARED, fd, 0);
    sys_close(fd);
    return p == (void *)-1 ? 0 : p;
}

// The child half: park on the word, then say so.
static int child_main(void) {
    // A SPACER FIRST, to move this process's mmap arena on before the
    // shared page lands -- otherwise the child maps at exactly the
    // address the parent did and the check that a wake crosses two
    // addresses is testing nothing.
    sys_mmap(0, 4096, SYS_PROT_READ | SYS_PROT_WRITE,
             SYS_MAP_PRIVATE | SYS_MAP_ANONYMOUS, -1, 0);

    // RETRIED: a named object belongs to its creator, and the parent
    // cannot grant before spawning because it does not know this pid
    // yet. The first opens fail with EPERM, legitimately.
    struct page *p = 0;
    for (int i = 0; i < 200 && !p; i++) {
        p = map_shared(0);
        if (!p) sys_sleep_ms(10);
    }
    if (!p) return 2;
    p->child_at = (uint64_t)(uintptr_t)p;
    p->ready = 1;
    // A deadline, so a broken wake fails this test instead of wedging
    // the machine and reading as a hang.
    if (sys_futex_wait(&p->word, 0, WAIT_MS) != 0) return 3;
    if (p->word != 1) return 4;
    p->word = 2;
    sys_futex_wake(&p->word, 0);
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "child")) return child_main();

    utest_begin("futex_test", "a futex parks and wakes across two processes",
                UTEST_VERDICT_FILE);

    sys_shm_unlink(SHM_NAME);
    struct page *p = map_shared(1);
    utest_check(p != 0, "the shared page maps");
    if (!p) return utest_end();
    p->word = 0;
    p->ready = 0;

    p->parent_at = (uint64_t)(uintptr_t)p;

    int pid = sys_spawn("/tests/futex_test", "child", -1);
    utest_check(pid > 0, "the child spawns");
    if (pid <= 0) return utest_end();
    // The child is let in by name; before objects had an owner it simply
    // opened the page, which anything could.
    utest_check(sys_shm_grant(SHM_NAME, pid) == 0, "the child is granted");

    // Wait for the child to reach its park. Polled rather than slept:
    // "it has set ready" is the observable, and a fixed sleep would be
    // a guess that gets slower under load rather than a condition.
    // 10 ms a turn, because a 1 ms sleep still costs a whole timer tick
    // -- asking for 4000 of them is 40 seconds, not 4, which reads as a
    // hung test rather than a slow one.
    int spins = 0;
    while (!p->ready && spins++ < 400) sys_sleep_ms(10);
    utest_check(p->ready != 0, "the child reached its wait");

    // It sets `ready` just BEFORE parking, so give it the moment between
    // the two. A wake that arrives early is not lost -- it returns 0
    // woken and the child then parks forever, which is exactly the
    // failure this margin avoids and the deadline above catches.
    sys_sleep_ms(50);

    p->word = 1;
    int woken = sys_futex_wake(&p->word, 0);

    // THE CHECK THIS FILE EXISTS FOR. One waiter, found through a page
    // both processes map at addresses nothing made equal.
    utest_checkf(woken == 1,
                 "the wake finds the waiter in the other process (woke %d)",
                 woken);

    // WHAT MAKES THE CHECK ABOVE MEAN ANYTHING. With both sides at one
    // address, a kernel keying on the caller's pointer passes it too --
    // so state the precondition instead of reporting a green check that
    // discriminates nothing. The KTEST beside futex_key() covers the
    // property directly either way.
    utest_checkf(p->child_at != p->parent_at,
                 "the two sides mapped it at DIFFERENT addresses "
                 "(parent %llx, child %llx)",
                 (unsigned long long)p->parent_at,
                 (unsigned long long)p->child_at);

    int code = 0;
    utest_check(sys_waitpid(pid, &code) == pid, "the child is reaped");
    utest_checkf(code == 0, "the child returned 0 (got %d)", code);
    utest_checkf(p->word == 2,
                 "the child ran on past its wait (word %u)", (unsigned)p->word);

    // --- the wakeword ------------------------------------------------
    //
    // THE POINT OF IT: one word that BOTH a sender and the KERNEL bump,
    // so a process can wait for a message and for a window event at the
    // same time. Without it a futex covers one source and there is no
    // poll() here to cover two.
    //
    // Checked from this process alone, because what needs proving is
    // that the kernel's own event post reaches the word -- and this test
    // has no window, so the event it can provoke is the one every
    // process can: none. So it checks the two halves separately:
    // registering works, and a wake on that word releases a waiter.
    p->word = 0;
    utest_check(sys_wakeword(&p->word) == 0, "a wakeword registers");
    utest_check(sys_wakeword(0) == 0, "and deregisters");

    // A word that already moved must not park -- the same race the
    // futex closes, now through the word everything shares.
    p->word = 5;
    utest_checkf(sys_futex_wait(&p->word, 4, 100) < 0,
                 "a moved wakeword does not park");

    sys_shm_unlink(SHM_NAME);
    return utest_end();
}
