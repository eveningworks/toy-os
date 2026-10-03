// diskbench -- time the filesystem's read and write paths.
//
//   diskbench [--size MiB] [--path FILE]
//
// WHY THIS IS A PROGRAM AND NOT PART OF THE GUI. The passes take
// minutes, and the compositor pings every client on a cadence: an app
// doing this work inside its own event loop stops answering, earns
// "(Not Responding)" and cannot draw progress. Disk Mark spawns this and
// polls, which is the File Manager's pattern (it spawns /bin/cp rather
// than reimplementing copying) and CrystalDiskMark's own shape, where a
// worker does the I/O and the window stays live. It also means the
// numbers are reachable without a desktop at all.
//
// WHAT THE LABELS MEAN, AND WHY THEY ARE NOT CDM's. Two things this OS
// cannot deliver are stated rather than implied:
//
//   * **One syscall is not one request.** SEQ_BLOCK is SYS_WRITE_MAX
//     (abi/syscall_abi.h) -- an artefact of the bounce buffer the
//     syscall copies through -- and libsys loops to complete a larger
//     buffer, so a "1 MiB transfer" is several syscalls and the disk
//     never sees one. The profile is therefore plain SEQ, and the
//     program PRINTS the size it used as `syscall-bytes` rather than
//     naming a figure here that the constant can outgrow. It has: this
//     comment has said 1 KiB and 64 KiB, both wrong by the time it was
//     read.
//   * **Q1T1.** One request in flight, always: there is no asynchronous
//     block interface and no threads here, so CDM's Q8T1/Q32T1 have
//     nothing to express.
//
// THE OUTPUT IS PARSED, so its shape is a contract. One line per event:
//
//   diskbench: progress <profile> <percent> <bytes-moved> <timed-us>
//   diskbench: result <profile> <milli-MB/s> <iops> <micros>
//   diskbench: io <profile> <op> <calls> <sectors> <micros>
//   diskbench: lookup <profile> <calls> <reads> <micros>
//   diskbench: clock-granularity-ns <n>
//   diskbench: done
//   diskbench: error <reason>
//
// THE `io` LINES ARE WHERE THE TIME WENT, one per block-layer operation
// kind (QUERY_BLKSTAT), as a DELTA across the profile. They exist
// because a MB/s figure cannot tell apart the three things that make a
// disk slow -- commands too small, commands too many, and cache
// flushes. A flush moves no sectors and can still be most of the wall
// clock, and on emulated hardware it is nearly free while on a real SSD
// it forces DRAM to NAND. Reading the profile's throughput without them
// is how a measurement taken in QEMU gets believed about a laptop.
//
// AND `clock-granularity-ns` IS WHAT SAYS WHETHER TO BELIEVE THE `io`
// MICROSECONDS AT ALL. The kernel times each call with its clocksource,
// and in every default QEMU configuration that is the PIT -- an
// invariant TSC is not offered to a guest unless the CPU model says
// `+invtsc`, because it blocks migration. A single disk command then
// rounds to zero and the column reads as "this cost nothing", which is
// the most misleading answer a profiler can give. Real hardware has the
// TSC and resolves it; a granularity in the millions means the `io`
// micros are floor-zero noise rather than a measurement.
//
// A `lookup` line is `<profile> <calls> <reads> <micros>` -- what the
// filesystem spent RESOLVING PATHS during that profile. The `io` lines
// cannot separate it: every read a resolution issues is attributed to
// the block layer along with the data traffic beside it. There is no
// inode cache, so each read()/write() syscall resolves from the root
// again, and this is the only place that per-syscall cost is visible.
//
// Throughput is in THOUSANDTHS of a MB/s and latency in MICROSECONDS,
// so a reader needs no floating point -- there is none in this project's
// shared code and this program has no business being the exception.
#include "rt/sys.h"
#include "lib/uclock.h"
#include "syscall_abi.h"   // SYS_WRITE_MAX -- the sequential request size
#include "lib/cmd.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include "tmppath.h"

// WHERE THE REPORT GOES. Default stdout, so a shell run reads normally;
// `--out FILE` writes it to a file instead.
//
// A FILE RATHER THAN A PIPE, and that is the point rather than a
// shortcut: SYS_SPAWN's stdout_fd must be a pipe write end, and a pipe
// here would re-couple the two processes -- PIPE_MAX is 8 KiB kernel
// wide, so a GUI that was slow to drain would BLOCK the benchmark it is
// timing, which is both a stall and a corrupted measurement. With a
// file neither side waits for the other.
// TO STDOUT IT STREAMS; TO A FILE IT SNAPSHOTS, and the difference is
// forced by how each is consumed. A shell reads a log top to bottom. A
// polling GUI re-reads the file in ONE read -- so an appended log puts
// the RESULTS after a wall of progress lines where a poller never
// reaches them. That shipped: the window sat at "Done." with four empty
// tiles and nothing logged. (This used to say sys_read carried at most
// 1 KiB per call; that was true when SYS_WRITE_MAX was 1024 and is not
// now -- the live bound is the READER's buffer, 4 KiB in diskmark.c.)
//
// So a file gets the whole state rewritten each time: at most six
// lines, always complete, always readable in one call.
static int g_to_file = 0;
static const char *g_out_path = 0;

// SIZED FROM THE WORST CASE, which is not obvious and was wrong once:
// 3 header lines + PROFILES result lines + PROFILES * 4 `io` lines (one
// per block-layer op), at ~64 bytes each -- about 1.4 KB. It was 512
// while only the results were sticky, and adding the `io` lines
// silently dropped every line past the fourth profile's first: the
// benchmark ran correctly and the report simply stopped growing, which
// reads exactly like a hang. Kept under the READER's 4 KiB buffer
// (diskmark.c's g_out), which is the real ceiling.
#define REPORT_MAX 2048
static char g_report[REPORT_MAX + 64];
static unsigned g_report_len = 0;
// Set once a sticky line did not fit. A truncated report must SAY so --
// silently losing results is the bug this buffer already had.
static int g_truncated = 0;

static void report_flush(void) {
    if (!g_to_file) return;
    int fd = open(g_out_path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return;
    write(fd, g_report, g_report_len);
    close(fd);
}

// One line of the snapshot. `sticky` lines (results) accumulate;
// progress replaces whatever transient line was there last.
static char g_sticky[REPORT_MAX];
static unsigned g_sticky_len = 0;

static void emit_ex(int sticky, const char *fmt, va_list ap) {
    char line[192];
    int n = vsnprintf(line, sizeof line, fmt, ap);
    if (n <= 0) return;

    if (!g_to_file) { write(1, line, (unsigned)n); return; }

    if (sticky) {
        if (g_sticky_len + (unsigned)n < sizeof g_sticky) {
            for (int i = 0; i < n; i++) g_sticky[g_sticky_len++] = line[i];
        } else {
            g_truncated = 1;
        }
    }
    // The snapshot is every sticky line so far, plus this one if it is
    // transient.
    g_report_len = 0;
    for (unsigned i = 0; i < g_sticky_len && g_report_len < sizeof g_report; i++)
        g_report[g_report_len++] = g_sticky[i];
    if (!sticky) {
        for (int i = 0; i < n && g_report_len < sizeof g_report; i++)
            g_report[g_report_len++] = line[i];
    }
    if (g_truncated) {
        const char *t = "diskbench: error report-truncated\n";
        for (int i = 0; t[i] && g_report_len < sizeof g_report; i++)
            g_report[g_report_len++] = t[i];
    }
    report_flush();
}

static void emit(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); emit_ex(1, fmt, ap); va_end(ap);
}

// A progress line REPLACES the last one rather than accumulating -- it
// is a transient, and 400 of them is what buried the results.
static void emit_transient(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); emit_ex(0, fmt, ap); va_end(ap);
}

// TMP_PERSISTENT, never TMP_VOLATILE: a disk benchmark pointed at the
// RAM scratch directory measures memcpy and reports a number that is
// enormous and meaningless, with nothing about it looking wrong.
static const char *default_path(void) {
    static char p[64];
    if (!p[0]) tmppath(p, sizeof p, TMP_PERSISTENT, "diskbench.tmp");
    return p;
}
#define DEFAULT_PATH default_path()
#define DEFAULT_MIB  64

// The sequential request IS whatever one syscall carries. Asking for
// more does not make a bigger transfer, it makes more syscalls -- and
// each one is a whole TFS3 transaction with two barriers, which is what
// made this measure a thousandth of the ring-0 `stress` command.
//
// DERIVED, not a literal, so the profile cannot claim a size the kernel
// no longer uses: the label is built from it below.
#define SEQ_BLOCK SYS_WRITE_MAX
#define RND_BLOCK 4096

#define P_SEQ_READ  0
#define P_SEQ_WRITE 1
#define P_RND_READ  2
#define P_RND_WRITE 3
#define PROFILES    4

// "SEQ", not "SEQ1M" and not "SEQ1K": the size is SYS_WRITE_MAX, which
// has changed once and would strand any number baked into a name here
// (CLAUDE.md: prefer facts that cannot go stale). Callers that want the
// figure read the `syscall-bytes` line below.
static const char *NAME[PROFILES] = {
    "SEQ-read", "SEQ-write", "RND4K-read", "RND4K-write",
};

// WRITE FIRST: the write is what lays the file down, so a separate
// prepare pass would move the same bytes again for no result. The
// caller displays them in read-then-write order regardless.
static const int ORDER[PROFILES] = { P_SEQ_WRITE, P_SEQ_READ, P_RND_WRITE, P_RND_READ };

// BIG ENOUGH FOR THE LARGEST REQUEST, which is the sequential one and
// is therefore SYS_WRITE_MAX. Sized at RND_BLOCK once, back when the
// sequential unit was 1 KiB -- raising the syscall cap to 64 KiB turned
// every sequential transfer into a 60 KiB overrun of this array, and
// the kernel's copy-from-user correctly refused the range rather than
// reading whatever followed it.
#define IO_BUF_MAX (SEQ_BLOCK > RND_BLOCK ? SEQ_BLOCK : RND_BLOCK)
static uint8_t g_buf[IO_BUF_MAX];
static uint32_t g_rand = 0x9E3779B9u;

// A repeatable spread of offsets, not entropy: a benchmark whose access
// pattern changed between runs would not be comparable with itself.
static uint32_t next_rand(void) {
    g_rand ^= g_rand << 13;
    g_rand ^= g_rand >> 17;
    g_rand ^= g_rand << 5;
    return g_rand;
}

// A short READ is legal and is not an error -- Unix's rule everywhere.
// write() completes a whole buffer itself; read() may not.
static int64_t io_full(int fd, void *buf, uint32_t len, int writing) {
    uint32_t done = 0;
    while (done < len) {
        int64_t n = writing ? write(fd, (uint8_t *)buf + done, len - done)
                            : read(fd, (uint8_t *)buf + done, len - done);
        if (n < 0) return -1;
        if (n == 0) break;
        done += (uint32_t)n;
    }
    return (int64_t)done;
}

static void fail(const char *why) {
    emit("diskbench: error %s\n", why);
}

// Progress: the percentage every 5%, and -- for a GUI drawing the rate
// over time -- at least every quarter second of TIMED work, with the
// bytes moved and the timed microseconds so far. Cumulative, so a
// reader that misses a line loses resolution and never data. Each line
// is a rewrite of the report, so it is never per request.
#define PROGRESS_EVERY_NS 250000000ull

static void progress(int profile, uint64_t moved, uint64_t total, uint64_t elapsed,
                     int *last, uint64_t *last_ns) {
    int pct = total ? (int)((moved * 100ull) / total) : 100;
    pct -= pct % 5;
    if (pct == *last && elapsed - *last_ns < PROGRESS_EVERY_NS) return;
    *last = pct;
    *last_ns = elapsed;
    emit_transient("diskbench: progress %s %d %llu %llu\n", NAME[profile], pct,
                   (unsigned long long)moved, (unsigned long long)(elapsed / 1000ull));
}

// Scale to milli-MiB FIRST so the multiply cannot overflow at the
// largest size, and divide by `ns` LAST so a fast pass does not
// truncate to zero on the way.
static uint64_t mbps_milli(uint64_t bytes, uint64_t ns) {
    if (!ns) return 0;
    return (bytes * 1000ull / 1048576ull) * 1000000000ull / ns;
}

// A snapshot of QUERY_BLKSTAT, so a profile can report its OWN I/O
// rather than everything since boot. Deltas rather than a reset,
// because a reset would be a WRITE and this class is a fact: two
// readers of it must not be able to blank each other's baseline.
#define BLKSTAT_OPS_MAX 8
struct io_snap {
    unsigned n;
    struct query_blkstat op[BLKSTAT_OPS_MAX];
};

static void io_snapshot(struct io_snap *s) {
    s->n = 0;
    struct query_blkstat r;
    QUERY_FOREACH(QUERY_BLKSTAT, r, i) {
        if (s->n >= BLKSTAT_OPS_MAX) break;
        s->op[s->n++] = r;
    }
}

// SILENT WHEN AN OP DID NOTHING. A zero row per profile is four lines of
// noise in a report a GUI re-reads whole; a missing row already means
// "no calls", which is the only thing the row would have said.
static void io_report(int profile, const struct io_snap *before,
                      const struct io_snap *after) {
    for (unsigned i = 0; i < after->n && i < before->n; i++) {
        uint64_t calls = after->op[i].calls - before->op[i].calls;
        if (!calls) continue;
        emit("diskbench: io %s %s %llu %llu %llu\n", NAME[profile],
             after->op[i].name, (unsigned long long)calls,
             (unsigned long long)(after->op[i].sectors - before->op[i].sectors),
             (unsigned long long)((after->op[i].ns - before->op[i].ns) / 1000ull));
    }
}

static void k_memset_fsstat(struct query_fsstat *f) {
    f->lookup_calls = f->lookup_reads = f->lookup_ns = 0;
}

static int run_profile(int profile, const char *path, uint64_t total, int first) {
    int writing = (profile == P_SEQ_WRITE || profile == P_RND_WRITE);
    int random = (profile == P_RND_READ || profile == P_RND_WRITE);

    // Only the FIRST pass creates and truncates. A later write pass
    // truncating would leave it writing into a hole rather than over
    // real blocks -- a different measurement wearing the same label.
    int flags = writing ? O_WRONLY : 0;
    if (first) flags |= O_CREAT | O_TRUNC;
    int fd = open(path, flags);
    if (fd < 0) { fail("could-not-open-the-test-file"); return 0; }

    uint64_t moved = 0, ops = 0, elapsed = 0;
    uint64_t blocks = total / RND_BLOCK;
    int last_pct = -1;
    uint64_t last_ns = 0;
    g_rand = 0x9E3779B9u;

    // A random pass covers an EIGHTH of the file: at 4 KiB an op, a full
    // pass over 256 MiB is 65536 seeks and minutes of them, and the
    // figure does not get truer for being slower.
    uint64_t want = random ? total / 8 : total;
    if (random && !blocks) { close(fd); fail("file-too-small-for-random"); return 0; }

    struct io_snap io_before, io_after;
    io_snapshot(&io_before);
    struct query_fsstat fs_before;
    k_memset_fsstat(&fs_before);
    sys_query_record(QUERY_FSSTAT, 0, &fs_before, sizeof fs_before);

    uint64_t began = sys_monotonic_ns();
    while (moved < want) {
        uint32_t unit = random ? RND_BLOCK : SEQ_BLOCK;
        if (random) {
            uint64_t off = (uint64_t)(next_rand() % (uint32_t)blocks) * RND_BLOCK;
            if (lseek(fd, (long long)off, SYS_SEEK_SET) < 0) {
                close(fd); fail("seek-failed"); return 0;
            }
        }
        if (io_full(fd, g_buf, unit, writing) != (int64_t)unit) {
            close(fd); fail("transfer-failed"); return 0;
        }
        moved += unit;
        ops++;
        // THE CLOCK STOPS FOR THE STATUS WRITE. Reporting to a file is
        // itself disk I/O, and leaving it inside the timed region would
        // put the benchmark's own bookkeeping into the number it is
        // producing.
        elapsed += sys_monotonic_ns() - began;
        progress(profile, moved, want, elapsed, &last_pct, &last_ns);
        began = sys_monotonic_ns();
    }
    elapsed += sys_monotonic_ns() - began;
    io_snapshot(&io_after);
    close(fd);

    uint64_t iops = elapsed ? (ops * 1000000000ull) / elapsed : 0;
    uint64_t us = ops ? (elapsed / ops) / 1000ull : 0;
    emit("diskbench: result %s %llu %llu %llu\n", NAME[profile],
           (unsigned long long)mbps_milli(moved, elapsed),
           (unsigned long long)iops, (unsigned long long)us);
    io_report(profile, &io_before, &io_after);
    // WHAT THE FILESYSTEM SPENT FINDING THE FILE, which the `io` lines
    // above cannot separate out: every one of those reads is attributed
    // to the block layer, and some fraction of them is the path being
    // resolved from the root again for this profile's every syscall.
    struct query_fsstat fs_after;
    if (sys_query_record(QUERY_FSSTAT, 0, &fs_after, sizeof fs_after)
            >= (int)sizeof fs_after) {
        emit("diskbench: lookup %s %llu %llu %llu\n", NAME[profile],
             (unsigned long long)(fs_after.lookup_calls - fs_before.lookup_calls),
             (unsigned long long)(fs_after.lookup_reads - fs_before.lookup_reads),
             (unsigned long long)((fs_after.lookup_ns - fs_before.lookup_ns) / 1000ull));
    }
    return 1;
}

int main(int argc, char **argv) {
    const char *path = DEFAULT_PATH;
    long mib = DEFAULT_MIB;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--size") && i + 1 < argc) {
            mib = strtol(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--path") && i + 1 < argc) {
            path = argv[++i];
        } else if (!strcmp(argv[i], "--out") && i + 1 < argc) {
            g_out_path = argv[++i];
            g_to_file = 1;
        } else {
            cmd_usage("diskbench [--size MiB] [--path FILE] [--out FILE]");
            return 1;
        }
    }
    if (mib <= 0 || mib > 4096) {
        fail("size-out-of-range");
        return 1;
    }

    for (unsigned i = 0; i < sizeof g_buf; i++) g_buf[i] = (uint8_t)(i * 31u + 7u);

    // STATED, so a reader of the report knows what "SEQ" meant on the
    // build that produced it.
    emit("diskbench: syscall-bytes %u\n", (unsigned)SEQ_BLOCK);
    emit("diskbench: clock-granularity-ns %llu\n",
         (unsigned long long)uclock_granularity_ns());

    uint64_t total = (uint64_t)mib * 1024u * 1024u;
    for (int step = 0; step < PROFILES; step++) {
        if (!run_profile(ORDER[step], path, total, step == 0)) {
            unlink(path);
            return 1;
        }
    }

    // SELF-CLEANING, and on the failure path above too: the temp file is
    // the only litter this can leave.
    unlink(path);
    emit("diskbench: done\n");
    return 0;
}
