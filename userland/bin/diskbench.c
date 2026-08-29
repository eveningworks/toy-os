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
//   * **64 KiB per syscall.** SYS_WRITE_MAX is 65536 bytes
//     (abi/syscall_abi.h) -- an artefact of the bounce buffer the
//     syscall copies through -- and libsys loops to complete a larger
//     buffer. So a "1 MiB transfer" is 1024 syscalls and the disk never
//     sees one. The sequential profile is therefore SEQ1K, not SEQ1M.
//   * **Q1T1.** One request in flight, always: there is no asynchronous
//     block interface and no threads here, so CDM's Q8T1/Q32T1 have
//     nothing to express.
//
// THE OUTPUT IS PARSED, so its shape is a contract. One line per event:
//
//   diskbench: progress <profile> <percent>
//   diskbench: result <profile> <milli-MB/s> <iops> <micros>
//   diskbench: done
//   diskbench: error <reason>
//
// Throughput is in THOUSANDTHS of a MB/s and latency in MICROSECONDS,
// so a reader needs no floating point -- there is none in this project's
// shared code and this program has no business being the exception.
#include "rt/sys.h"
#include "syscall_abi.h"   // SYS_WRITE_MAX -- the sequential request size
#include "lib/cmd.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

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
// polling GUI re-reads the file, and sys_read carries at most 1 KiB per
// call -- so an appended log puts the RESULTS after a kilobyte of
// progress lines where a poller never reaches them. That shipped: the
// window sat at "Done." with four empty tiles and nothing logged.
//
// So a file gets the whole state rewritten each time: at most six
// lines, always complete, always readable in one call.
static int g_to_file = 0;
static const char *g_out_path = 0;

static char g_report[512];
static unsigned g_report_len = 0;

static void report_flush(void) {
    if (!g_to_file) return;
    int fd = sys_open(g_out_path, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    if (fd < 0) return;
    sys_write(fd, g_report, g_report_len);
    sys_close(fd);
}

// One line of the snapshot. `sticky` lines (results) accumulate;
// progress replaces whatever transient line was there last.
static char g_sticky[384];
static unsigned g_sticky_len = 0;

static void emit_ex(int sticky, const char *fmt, va_list ap) {
    char line[192];
    int n = vsnprintf(line, sizeof line, fmt, ap);
    if (n <= 0) return;

    if (!g_to_file) { sys_write(1, line, (unsigned)n); return; }

    if (sticky && g_sticky_len + (unsigned)n < sizeof g_sticky) {
        for (int i = 0; i < n; i++) g_sticky[g_sticky_len++] = line[i];
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

#define DEFAULT_PATH "/tmp/diskbench.tmp"
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
// sys_write() completes a whole buffer itself; sys_read() may not.
static int64_t io_full(int fd, void *buf, uint32_t len, int writing) {
    uint32_t done = 0;
    while (done < len) {
        int64_t n = writing ? sys_write(fd, (uint8_t *)buf + done, len - done)
                            : sys_read(fd, (uint8_t *)buf + done, len - done);
        if (n < 0) return -1;
        if (n == 0) break;
        done += (uint32_t)n;
    }
    return (int64_t)done;
}

static void fail(const char *why) {
    emit("diskbench: error %s\n", why);
}

// Progress is reported at most once per percent: this is a pipe or a
// file the GUI re-reads, and a line per request would be thousands of
// them for a number that changed by nothing.
static void progress(int profile, uint64_t moved, uint64_t total, int *last) {
    int pct = total ? (int)((moved * 100ull) / total) : 100;
    // Every 5%, not every 1%: each one is a file rewrite, i.e. real disk
    // I/O in the middle of a disk benchmark.
    pct -= pct % 5;
    if (pct == *last) return;
    *last = pct;
    emit_transient("diskbench: progress %s %d\n", NAME[profile], pct);
}

// Scale to milli-MiB FIRST so the multiply cannot overflow at the
// largest size, and divide by `ns` LAST so a fast pass does not
// truncate to zero on the way.
static uint64_t mbps_milli(uint64_t bytes, uint64_t ns) {
    if (!ns) return 0;
    return (bytes * 1000ull / 1048576ull) * 1000000000ull / ns;
}

static int run_profile(int profile, const char *path, uint64_t total, int first) {
    int writing = (profile == P_SEQ_WRITE || profile == P_RND_WRITE);
    int random = (profile == P_RND_READ || profile == P_RND_WRITE);

    // Only the FIRST pass creates and truncates. A later write pass
    // truncating would leave it writing into a hole rather than over
    // real blocks -- a different measurement wearing the same label.
    int flags = writing ? SYS_O_WRITE : 0;
    if (first) flags |= SYS_O_CREAT | SYS_O_TRUNC;
    int fd = sys_open(path, flags);
    if (fd < 0) { fail("could-not-open-the-test-file"); return 0; }

    uint64_t moved = 0, ops = 0, elapsed = 0;
    uint64_t blocks = total / RND_BLOCK;
    int last_pct = -1;
    g_rand = 0x9E3779B9u;

    // A random pass covers an EIGHTH of the file: at 4 KiB an op, a full
    // pass over 256 MiB is 65536 seeks and minutes of them, and the
    // figure does not get truer for being slower.
    uint64_t want = random ? total / 8 : total;
    if (random && !blocks) { sys_close(fd); fail("file-too-small-for-random"); return 0; }

    uint64_t began = sys_monotonic_ns();
    while (moved < want) {
        uint32_t unit = random ? RND_BLOCK : SEQ_BLOCK;
        if (random) {
            uint64_t off = (uint64_t)(next_rand() % (uint32_t)blocks) * RND_BLOCK;
            if (sys_lseek(fd, (long long)off, SYS_SEEK_SET) < 0) {
                sys_close(fd); fail("seek-failed"); return 0;
            }
        }
        if (io_full(fd, g_buf, unit, writing) != (int64_t)unit) {
            sys_close(fd); fail("transfer-failed"); return 0;
        }
        moved += unit;
        ops++;
        // THE CLOCK STOPS FOR THE STATUS WRITE. Reporting to a file is
        // itself disk I/O, and leaving it inside the timed region would
        // put the benchmark's own bookkeeping into the number it is
        // producing.
        elapsed += sys_monotonic_ns() - began;
        progress(profile, moved, want, &last_pct);
        began = sys_monotonic_ns();
    }
    elapsed += sys_monotonic_ns() - began;
    sys_close(fd);

    uint64_t iops = elapsed ? (ops * 1000000000ull) / elapsed : 0;
    uint64_t us = ops ? (elapsed / ops) / 1000ull : 0;
    emit("diskbench: result %s %llu %llu %llu\n", NAME[profile],
           (unsigned long long)mbps_milli(moved, elapsed),
           (unsigned long long)iops, (unsigned long long)us);
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

    uint64_t total = (uint64_t)mib * 1024u * 1024u;
    for (int step = 0; step < PROFILES; step++) {
        if (!run_profile(ORDER[step], path, total, step == 0)) {
            sys_unlink(path);
            return 1;
        }
    }

    // SELF-CLEANING, and on the failure path above too: the temp file is
    // the only litter this can leave.
    sys_unlink(path);
    emit("diskbench: done\n");
    return 0;
}
