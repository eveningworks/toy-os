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
// **WHAT A BROKEN VERSION WOULD STILL PASS -- including THIS phase.**
// The pattern is derived from the OFFSET, and Y's is X's inverted, so a
// mixed-up block cannot read back looking right. But with the drain
// removed it still passed (2026-09-24): the reader holds the ATA
// driver's lock for its transfer, so the reuse write queues behind it.
// Like heaprace_test.c, phase 1 is a fixture for a driver that queues
// commands, not proof. Phase 2's control DOES go red (see below).
//
// PHASE 2 IS THE WRITE SIDE (stage 3b): an OVERWRITE in place also drops
// the lock, and what it must never do is commit its own stale copy of the
// inode over a change another call made meanwhile. So one thread keeps
// rewriting the first 128 KiB of Z while this one appends numbered 4 KiB
// records to it; at the end Z's size must be the base plus EVERY record,
// and each record intact. A lost update shows as a short file.
//
// It must be SPAWNED, not `run` -- threads need a scheduler slot.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "rt/sys.h"
#include "lib/utest.h"

#define X_PATH "/var/tmp/fsrace.x"
#define Y_PATH "/var/tmp/fsrace.y"
#define SIZE   (256u * 1024u)   // 64 blocks: runs of whole blocks, so gaps
#define Z_PATH "/var/tmp/fsrace.z"
#define Z_BASE (128u * 1024u)    // the overwritten region
#define REC    4096u
#define MAX_RECS 2048u

static uint8_t x_at(uint32_t off) { return (uint8_t)(off * 131u + 7u); }
static uint8_t y_at(uint32_t off) { return (uint8_t)~x_at(off); }

static uint8_t g_xbuf[SIZE], g_ybuf[SIZE], g_rbuf[SIZE];
static volatile int g_stop;
static volatile unsigned g_cycles;

static int put_n(const char *path, const uint8_t *buf, uint32_t size) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return 0;
    uint32_t done = 0;
    while (done < size) {
        long n = write(fd, buf + done, size - done);
        if (n <= 0) break;
        done += (uint32_t)n;
    }
    close(fd);
    return done == size;
}

static int put(const char *path, const uint8_t *buf) { return put_n(path, buf, SIZE); }

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

static uint8_t g_zbuf[Z_BASE], g_rec[REC];

static void *overwriter(void *arg) {
    (void)arg;
    unsigned v = 0;
    while (!g_stop) {
        memset(g_zbuf, (int)(0x30 + (v++ & 0x3f)), Z_BASE);
        int fd = open(Z_PATH, O_WRONLY);
        if (fd < 0) continue;
        write(fd, g_zbuf, Z_BASE);                    // in place: the gapped path
        close(fd);
        g_cycles++;
    }
    return 0;
}

static void fill_rec(uint8_t *b, unsigned i) {
    for (unsigned k = 0; k < REC; k++) b[k] = (uint8_t)(i * 17u + k * 3u + 1u);
}

// Phase 2: the checks are its report.
static void write_side(int secs) {
    memset(g_zbuf, 0x30, Z_BASE);
    utest_check(put_n(Z_PATH, g_zbuf, Z_BASE), "wrote the base of the appended file");
    g_stop = 0;
    g_cycles = 0;
    pthread_t t;
    int started = pthread_create(&t, NULL, overwriter, NULL) == 0;
    utest_check(started, "started the overwriter thread");

    unsigned recs = 0;
    unsigned long long end = sys_monotonic_ns() + (unsigned long long)secs * 1000000000ull;
    while (started && recs < MAX_RECS && sys_monotonic_ns() < end) {
        fill_rec(g_rec, recs);
        int fd = open(Z_PATH, O_WRONLY | O_APPEND);
        if (fd < 0) break;
        long n = write(fd, g_rec, REC);
        close(fd);
        if (n != (long)REC) break;
        recs++;
    }
    g_stop = 1;
    if (started) pthread_join(t, NULL);

    struct stat st;
    long long size = stat(Z_PATH, &st) == 0 ? (long long)st.st_size : -1;
    unsigned bad = 0;
    int fd = open(Z_PATH, O_RDONLY);
    for (unsigned i = 0; fd >= 0 && i < recs; i++) {
        static uint8_t want[REC];
        fill_rec(want, i);
        lseek(fd, (off_t)(Z_BASE + i * REC), SEEK_SET);
        if (read(fd, g_rec, REC) != (long)REC || memcmp(g_rec, want, REC)) bad++;
    }
    if (fd >= 0) close(fd);
    unlink(Z_PATH);

    utest_checkf(recs > 0 && g_cycles > 0, "appended %u records against %u overwrites",
                 recs, g_cycles);
    utest_checkf(size == (long long)(Z_BASE + recs * REC),
                 "no append was lost: size %lld, expected %u", size, Z_BASE + recs * REC);
    utest_checkf(bad == 0, "every appended record reads back intact: %u bad", bad);
}

int main(int argc, char **argv) {
    int secs = 3;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--secs") && i + 1 < argc) secs = atoi(argv[++i]);
    utest_begin("fsrace_test", "fs I/O without the lock: no foreign bytes, no lost update",
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

    write_side(secs);
    return utest_end();
}
