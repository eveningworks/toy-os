// A READ THAT DROPPED THE FILESYSTEM LOCK MUST NEVER RETURN ANOTHER
// FILE'S BYTES -- fslock stage 3 (docs/fslock-design.md).
//
// tfs3 reads a run of whole blocks with its mount's lock DROPPED, so the
// rest of the volume is not held up for a disk transfer. The hazard that
// opens: while the read is at the device, another thread truncates the
// file, its blocks are freed, a second file takes them and writes its
// own data there -- and the read lands on THAT. The guard is that a
// block free waits for such reads to finish (mount_io_drain(), Linux's
// inode_dio_wait()).
//
// So: a writer thread keeps truncating X, filling Y into what was freed,
// and rewriting X; the main thread keeps reading X and checks EVERY byte
// it gets against X's own pattern. A short or empty read is fine (the
// file really shrank); a byte that is not X's is the bug, and if it is
// Y's the report says so.
//
// **WHAT A BROKEN VERSION WOULD STILL PASS.** The pattern is derived
// from the OFFSET, and Y's is X's inverted, so a mixed-up block cannot
// read back looking right. And the race window is a whole disk wait,
// not a few instructions (heaprace_test.c's lesson): it IS reachable on
// one core, which is what the positive control -- the drain removed --
// has to show before a pass here means anything.
//
// It must be SPAWNED, not `run` -- threads need a scheduler slot.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include "rt/sys.h"
#include "lib/utest.h"

#define X_PATH "/var/tmp/fsrace.x"
#define Y_PATH "/var/tmp/fsrace.y"
#define SIZE   (256u * 1024u)   // 64 blocks: runs of whole blocks, so gaps

static uint8_t x_at(uint32_t off) { return (uint8_t)(off * 131u + 7u); }
static uint8_t y_at(uint32_t off) { return (uint8_t)~x_at(off); }

static uint8_t g_xbuf[SIZE], g_ybuf[SIZE], g_rbuf[SIZE];
static volatile int g_stop;
static volatile unsigned g_cycles;

static int put(const char *path, const uint8_t *buf) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return 0;
    uint32_t done = 0;
    while (done < SIZE) {
        long n = write(fd, buf + done, SIZE - done);
        if (n <= 0) break;
        done += (uint32_t)n;
    }
    close(fd);
    return done == SIZE;
}

static void *writer(void *arg) {
    (void)arg;
    while (!g_stop) {
        int fd = open(X_PATH, O_WRONLY | O_TRUNC);   // frees X's blocks
        if (fd >= 0) close(fd);
        put(Y_PATH, g_ybuf);                         // ...and reuses them
        put(X_PATH, g_xbuf);
        unlink(Y_PATH);
        g_cycles++;
    }
    return 0;
}

int main(int argc, char **argv) {
    int secs = 3;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--secs") && i + 1 < argc) secs = atoi(argv[++i]);
    utest_begin("fsrace_test", "a read without the fs lock never sees another file's bytes",
                UTEST_VERDICT_FILE);

    for (uint32_t o = 0; o < SIZE; o++) { g_xbuf[o] = x_at(o); g_ybuf[o] = y_at(o); }
    utest_check(put(X_PATH, g_xbuf), "wrote the file under test");

    pthread_t t;
    int started = pthread_create(&t, NULL, writer, NULL) == 0;
    utest_check(started, "started the writer thread");

    unsigned long long end = sys_monotonic_ns() + (unsigned long long)secs * 1000000000ull;
    unsigned long long bytes = 0, reads = 0, wrong = 0, theirs = 0;
    long first_bad = -1;
    while (started && sys_monotonic_ns() < end) {
        int fd = open(X_PATH, O_RDONLY);
        if (fd < 0) continue;
        long n = read(fd, g_rbuf, SIZE);
        close(fd);
        if (n <= 0) continue;
        reads++;
        bytes += (unsigned long long)n;
        for (long o = 0; o < n; o++) {
            if (g_rbuf[o] == x_at((uint32_t)o)) continue;
            wrong++;
            if (g_rbuf[o] == y_at((uint32_t)o)) theirs++;
            if (first_bad < 0) first_bad = o;
        }
    }
    g_stop = 1;
    if (started) pthread_join(t, NULL);
    unlink(X_PATH);
    unlink(Y_PATH);

    // The fixture reached the code: reads landed while the writer ran.
    utest_checkf(reads > 0 && g_cycles > 0, "reads %llu (%llu KiB) against %u writer cycles",
                 reads, bytes / 1024, g_cycles);
    utest_checkf(wrong == 0, "every byte read is the file's own: %llu wrong, %llu of them "
                 "the OTHER file's, first at offset %ld", wrong, theirs, first_bad);
    return utest_end();
}
