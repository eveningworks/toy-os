// SYS_MMAP / SYS_MUNMAP, from ring 3 -- the ONLY place they can be
// tested, since a mapping belongs to a process's address space and a
// KTEST runs in the kernel context, which has none (fd_test.c's
// reasoning exactly).
//
// The write-through-the-mapping patterns are ADDRESS-DERIVED, for
// memtest.c's reason: a constant fill cannot tell two virtual pages
// sharing one frame apart, an address-derived value names the loser.
//
// What is deliberately NOT here: touching a PROT_READ page and
// touching an unmapped hole both KILL the process, so they belong with
// the fault-on-purpose tests, not in a suite that must exit 0.
#include <stdint.h>
#include "rt/sys.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/mman.h>
#include "syscall_abi.h"
#include "tmppath.h"

// Console AND /tmp/mmap_test.out (UTEST_VERDICT_FILE): this test is
// SPAWNED (mmap needs a scheduler slot), and a spawned program's
// console output arrives while the harness is between commands -- the
// artifact is what usertest_run.py actually reads.
#include "lib/utest.h"

#define ARENA_BASE 0x9000000000ULL
#define ARENA_END  0x9800000000ULL

static int in_arena(void *p) {
    uint64_t a = (uint64_t)p;
    return a >= ARENA_BASE && a < ARENA_END && !(a & 0xFFF);
}

// The pattern a byte at user address `a` should hold.
static uint8_t pat(uint64_t a) { return (uint8_t)((a * 131) ^ (a >> 12)); }

static void fill(uint8_t *p, uint64_t n) {
    for (uint64_t i = 0; i < n; i++) p[i] = pat((uint64_t)p + i);
}

static int holds(const uint8_t *p, uint64_t n) {
    for (uint64_t i = 0; i < n; i++)
        if (p[i] != pat((uint64_t)p + i)) return 0;
    return 1;
}

static const char *p_file_path(void) {
    static char p[64];
    if (!p[0]) tmppath(p, sizeof p, TMP_VOLATILE, "mmap_test.dat");
    return p;
}
#define FILE_PATH p_file_path()
#define FILE_BYTES (3 * 4096 + 100) // three pages and a ragged tail

static uint8_t fbyte(uint64_t off) { return (uint8_t)((off * 7) + 3); }

static int write_fixture(void) {
    int fd = sys_open(FILE_PATH, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    if (fd < 0) return 0;
    static uint8_t buf[FILE_BYTES];
    for (uint64_t i = 0; i < FILE_BYTES; i++) buf[i] = fbyte(i);
    int64_t n = sys_write(fd, buf, FILE_BYTES);
    sys_close(fd);
    return n == FILE_BYTES;
}

int main(void) {
    utest_begin("mmap_test", "SYS_MMAP / SYS_MUNMAP", UTEST_VERDICT_FILE);

    // --- anonymous ---------------------------------------------------
    uint8_t *a = sys_mmap(0, 3 * 4096, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    utest_check(in_arena(a), "anonymous map lands page-aligned in the arena");
    if (in_arena(a)) {
        int zero = 1;
        for (int i = 0; i < 3 * 4096; i++) if (a[i]) zero = 0;
        utest_check(zero, "fresh anonymous pages read as zeros");
        fill(a, 3 * 4096);
        utest_check(holds(a, 3 * 4096), "pattern survives across all three pages");
    }

    uint8_t *b = sys_mmap(0, 4096, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    utest_check(in_arena(b) && (b >= a + 3 * 4096 || b + 4096 <= a),
          "second map does not overlap the first");

    // The mapping is visible through the fact registry (what /bin/pmap
    // prints): our own pid, kind anon, at exactly this base.
    {
        int me = sys_getpid();
        int found = 0;
        struct query_procmap q;
        for (unsigned i = 0; ; i++) {
            if (sys_query_record(QUERY_PROCMAP, i, &q, sizeof q) <
                (int)sizeof q) break;
            if ((int)q.pid == me && q.kind == QUERY_PROCMAP_ANON &&
                q.base == (uint64_t)a && q.bytes == 3 * 4096)
                found = 1;
        }
        utest_check(found, "QUERY_PROCMAP reports the region");
    }

    // A free, aligned hint is honoured; a garbage hint falls back.
    uint8_t *want = (uint8_t *)(ARENA_BASE + 64 * 4096);
    uint8_t *c = sys_mmap(want, 4096, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    utest_check(c == want, "a usable hint is honoured");
    uint8_t *d = sys_mmap((void *)0x1234, 4096, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    utest_check(in_arena(d), "a garbage hint falls back to the allocator");

    // --- MAP_FIXED ---------------------------------------------------
    void *fx = sys_mmap(c, 4096, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    utest_check(fx == MAP_FAILED && sys_errno() == EEXIST,
          "MAP_FIXED over a live mapping refuses with EEXIST");
    utest_check(sys_munmap(c, 4096) == 0, "munmap of a whole region returns 0");
    fx = sys_mmap(want, 4096, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    utest_check(fx == (void *)want, "MAP_FIXED into the freed hole gets that address");
    sys_munmap(fx, 4096);

    // --- munmap: trims and the middle split --------------------------
    utest_check(holds(a, 3 * 4096), "neighbouring mapping untouched by the above");
    utest_check(sys_munmap(a + 4096, 4096) == 0, "munmap of a region's middle page");
    utest_check(holds(a, 4096) && holds(a + 2 * 4096, 4096),
          "the split's head and tail both keep their bytes");
    void *mid = sys_mmap(a + 4096, 4096, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    utest_check(mid == (void *)(a + 4096), "the split's hole is really free");
    sys_munmap(mid, 4096);
    utest_check(sys_munmap(a, 4096) == 0, "front trim");
    utest_check(sys_munmap(a + 2 * 4096, 4096) == 0, "back piece");
    sys_munmap(b, 4096);
    sys_munmap(d, 4096);

    // --- file-backed -------------------------------------------------
    utest_check(write_fixture(), "fixture file written");
    int fd = sys_open(FILE_PATH, 0);
    utest_check(fd >= 0, "fixture opens for read");

    // Five pages over a three-and-a-bit-page file: the ragged tail's
    // page must be zeros past EOF, and the two pages wholly past it
    // must be all zeros.
    uint8_t *f = sys_mmap(0, 5 * 4096, PROT_READ,
                          MAP_PRIVATE, fd, 0);
    utest_check(in_arena(f), "file map lands in the arena");
    if (in_arena(f)) {
        int match = 1;
        for (uint64_t i = 0; i < FILE_BYTES; i++)
            if (f[i] != fbyte(i)) { match = 0; break; }
        utest_check(match, "mapped bytes equal the file's");
        int tail0 = 1;
        for (uint64_t i = FILE_BYTES; i < 5 * 4096; i++)
            if (f[i]) { tail0 = 0; break; }
        utest_check(tail0, "past EOF reads as zeros");
    }

    // Offset: page 1 of the mapping is page 1 of the file.
    uint8_t *g = sys_mmap(0, 4096, PROT_READ, MAP_PRIVATE, fd, 4096);
    utest_check(in_arena(g) && g[0] == fbyte(4096) && g[4095] == fbyte(2 * 4096 - 1),
          "a page-aligned offset maps the right slice");

    // MAP_PRIVATE: a write through the mapping never reaches the file.
    uint8_t *w = sys_mmap(0, 4096, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE, fd, 0);
    if (in_arena(w)) {
        w[0] = (uint8_t)~fbyte(0);
        uint8_t back[8];
        int rfd = sys_open(FILE_PATH, 0);
        sys_read(rfd, back, sizeof back);
        sys_close(rfd);
        utest_check(back[0] == fbyte(0), "a private write never reaches the file");
    } else {
        utest_check(0, "writable private file map");
    }
    sys_close(fd); // the region survives its fd, POSIX's rule
    utest_check(in_arena(f) && f[100] == fbyte(100),
          "an untouched page still faults in after close(fd)");
    sys_munmap(f, 5 * 4096);
    sys_munmap(g, 4096);
    sys_munmap(w, 4096);

    // --- refusals ----------------------------------------------------
    void *e;
    e = sys_mmap(0, 0, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    utest_check(e == MAP_FAILED && sys_errno() == EINVAL, "length 0 is EINVAL");
    e = sys_mmap(0, 4096, PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    utest_check(e == MAP_FAILED && sys_errno() == EINVAL,
          "PROT without READ is EINVAL");
    e = sys_mmap(0, 4096, PROT_READ, MAP_PRIVATE, 999, 0);
    utest_check(e == MAP_FAILED && sys_errno() == EBADF, "a bad fd is EBADF");
    int wfd = sys_open(FILE_PATH, SYS_O_WRITE);
    e = sys_mmap(0, 4096, PROT_READ, MAP_PRIVATE, wfd, 0);
    utest_check(e == MAP_FAILED && sys_errno() == EBADF,
          "a write-mode fd is EBADF (open the wrong way)");
    sys_close(wfd);
    e = sys_mmap(0, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 100);
    utest_check(e == MAP_FAILED && sys_errno() == EINVAL,
          "an unaligned offset is EINVAL");
    utest_check(sys_munmap((void *)(ARENA_BASE + 1), 4096) == -1 &&
          sys_errno() == EINVAL, "unaligned munmap is EINVAL");
    utest_check(sys_munmap((void *)(ARENA_BASE + 0x100000000ULL), 4096) == -1 &&
          sys_errno() == EINVAL, "munmap of a hole is EINVAL");

    // --- the region table's bound ------------------------------------
    // Fill all 16 slots (15 singles beside one 3-pager), then require
    // the 17th mapping AND a middle split -- which needs a free slot
    // for the tail -- to refuse without half-applying.
    uint8_t *three = sys_mmap(0, 3 * 4096, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint8_t *slots[15];
    int got = 0;
    for (int i = 0; i < 15; i++) {
        slots[i] = sys_mmap(0, 4096, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (in_arena(slots[i])) got++;
    }
    utest_check(in_arena(three) && got == 15, "sixteen regions coexist");
    e = sys_mmap(0, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    utest_check(e == MAP_FAILED && sys_errno() == ENOMEM,
          "a seventeenth region is ENOMEM");
    if (in_arena(three)) {
        fill(three, 3 * 4096);
        utest_check(sys_munmap(three + 4096, 4096) == -1 && sys_errno() == ENOMEM,
              "a middle split with a full table refuses whole");
        utest_check(holds(three, 3 * 4096),
              "...and the refused region kept every byte");
        sys_munmap(three, 3 * 4096);
    } else {
        utest_check(0, "a middle split with a full table refuses whole");
        utest_check(0, "...and the refused region kept every byte");
    }
    for (int i = 0; i < 15; i++)
        if (in_arena(slots[i])) sys_munmap(slots[i], 4096);

    // --- the libc face -----------------------------------------------
    void *lc = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    utest_check(lc != MAP_FAILED && in_arena(lc) && munmap(lc, 4096) == 0,
          "<sys/mman.h>'s mmap()/munmap() link and work");

    sys_unlink(FILE_PATH);

    return utest_end();
}
