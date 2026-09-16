// A BENCHMARK, not a test -- it is not in the gate, because a timing
// number varies with the host and would make the gate flap.
//
// **WHAT IT ANSWERS: what should BUFSIZ be?** stdio batches small
// writes into one syscall per BUFSIZ bytes, so the constant trades
// memory for syscalls. It was 1024 because SYS_WRITE_MAX was also 1024
// and a bigger buffer could only have meant more short writes; the cap
// is far larger now and that reasoning is gone.
//
// **IT DOES NOT GO THROUGH stdio, AND THAT IS THE POINT.** tolibc's
// setvbuf() only sets the MODE -- it ignores `size` and refuses a
// caller-supplied buffer -- so a stdio-based sweep would measure one
// size six times. This batches into its own buffer and calls
// sys_write() per batch, which is what fwrite() does with f->buf, so
// the figure is the quantity BUFSIZ trades: one syscall per N bytes.
// The total memcpy is the same at every N and cancels out.
//
// **THE DISK PASS IS SIZED SO THE BIGGEST BUFFER STILL MAKES SEVERAL
// SYSCALLS.** At 64 KB it made ONE at 65536 and one at 262144, so those
// two rows measured the same single write and could not be told apart --
// which is exactly the range the choice of BUFSIZ turns on.
//
// **WHERE IT WRITES DECIDES WHAT IT MEASURES.** On /tmp (ramfs) there
// is no device, so the figure is per-syscall overhead alone -- the
// ceiling on what this constant can buy. On /var/tmp (the disk) the
// device dominates and the same change looks smaller, which is what a
// user would actually feel.
//
// Two things it does NOT measure, both deliberate. A tty is _IOLBF, so
// it flushes at every newline whatever BUFSIZ is -- terminal output
// cannot benefit. And fwrite() sends a block at least as big as the
// buffer straight out (stdio.c), so bulk writers already bypass this.
// The workload is SMALL writes, which is what printf()/fputs()/putc()
// do and what the constant exists for.
//
// **ROUNDS EXISTS BECAUSE OF THE CLOCK, NOT FOR STATISTICS.** The
// monotonic clock ticks at 100 Hz, so one 1 MiB pass finishes inside a
// single tick and every figure it produces is 0. Each configuration is
// repeated until it spans enough ticks for the quantisation to fall
// under a few percent; the disk gets fewer rounds because it is far
// slower per byte and reaches that on its own.
//
//     spawn /tests/stdio_bench     (CPU + wall)
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "rt/sys.h"
#include "tmppath.h"
#include "syscall_abi.h"
#include <stdarg.h>

#define LINE_LEN     64
#define PASS_KB      1024   // ramfs pass
#define DISK_KB      4096   // the disk is ~100x slower per byte; see the header

#define ROUNDS_RAM   40
#define ROUNDS_DISK  4

// A duration of 0 is a legitimate answer from a coarse clock, so it
// cannot double as "this pass failed" -- that was the first version's
// bug and it reported every row as a failure.
#define FAILED  ((uint64_t)~0ull)

// 8192 and 32768 are here to resolve the KNEE rather than to be
// candidates: the first real-hardware run jumped 12x between 1024 and
// 4096 and then doubled again by 65536, with nothing measured between.
static const int SIZES[] = { 512, 1024, 4096, 8192, 16384, 32768, 65536, 262144 };
#define NSIZES ((int)(sizeof SIZES / sizeof SIZES[0]))

// The report goes to a FILE as well as stdout: this runs for longer
// than a console capture stays attached, and a table that scrolls past
// the listener is a measurement nobody gets. `cat` it afterwards.
static char g_logpath[64];
#define LOG_PATH g_logpath
static FILE *g_log;

static void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    fflush(stdout);
    if (g_log) {
        va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
        fflush(g_log);
    }
}

static char g_line[LINE_LEN];

static void build_line(void) {
    for (int i = 0; i < LINE_LEN - 1; i++) g_line[i] = (char)('a' + (i % 26));
    g_line[LINE_LEN - 1] = '\n';
}

static uint64_t pass(const char *path, int bufsz, int rounds, int kb, uint64_t *cpu) {
    const int lines = (kb * 1024) / LINE_LEN;
    char *buf = (char *)malloc((size_t)bufsz);
    if (!buf) return FAILED;

    // Opened ONCE, outside the timed rounds: reopening per round would
    // put open() and a truncate inside a figure that is meant to be
    // write cost and nothing else.
    int fd = sys_open(path, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    if (fd < 0) { free(buf); return FAILED; }

    int pos = 0;
    clock_t c0 = clock();
    uint64_t w0 = sys_monotonic_ns();
    for (int r = 0; r < rounds; r++) {
        for (int i = 0; i < lines; i++) {
            if (pos + LINE_LEN > bufsz) {
                if (sys_write(fd, buf, (size_t)pos) < 0) { pos = -1; break; }
                pos = 0;
            }
            memcpy(buf + pos, g_line, LINE_LEN);
            pos += LINE_LEN;
        }
        if (pos < 0) break;
    }
    if (pos > 0 && sys_write(fd, buf, (size_t)pos) < 0) pos = -1;
    uint64_t wall = (sys_monotonic_ns() - w0) / 1000u;
    *cpu = (uint64_t)(clock() - c0);
    sys_close(fd);
    free(buf);
    return pos < 0 ? FAILED : wall;
}

static void run(const char *label, const char *path, int rounds, int kb) {
    say("  --- %s (%d rounds x %d KB) ---\n", label, rounds, kb);
    say("    %8s %10s %10s %10s %9s\n", "bufsz", "syscalls", "wall us", "cpu us", "KB/s");
    for (int i = 0; i < NSIZES; i++) {
        uint64_t cpu = 0;
        uint64_t wall = pass(path, SIZES[i], rounds, kb, &cpu);
        long sc = (long)(((long)kb * 1024 + SIZES[i] - 1) / SIZES[i]);
        if (wall == FAILED) { say("    %8d %10ld    FAILED\n", SIZES[i], sc); continue; }
        if (wall == 0)      { say("    %8d %10ld  too fast to time -- raise ROUNDS\n", SIZES[i], sc); continue; }
        unsigned long kbs = (unsigned long)((uint64_t)kb * rounds * 1000000u / wall);
        say("    %8d %10ld %10llu %10llu %9lu\n", SIZES[i], sc,
               (unsigned long long)wall, (unsigned long long)cpu, kbs);
    }
}

int main(void) {
    char vol[64], per[64];
    tmppath(g_logpath, sizeof g_logpath, TMP_PERSISTENT, "stdio_bench.log");
    build_line();
    g_log = fopen(LOG_PATH, "w");
    tmppath(vol, sizeof vol, TMP_VOLATILE, "stdio_bench.out");
    tmppath(per, sizeof per, TMP_PERSISTENT, "stdio_bench.out");

    say("stdio_bench: %d-byte writes; ramfs pass %d KB, disk pass %d KB\n", LINE_LEN, PASS_KB, DISK_KB);
    say("  BUFSIZ today is %d; the syscalls column is per pass\n", BUFSIZ);

    int have_cpu = (clock() != (clock_t)-1);
    if (!have_cpu)
        say("  NOTE: no CPU time -- the legacy `run` loader has no scheduler\n"
               "        slot. Use `spawn /tests/stdio_bench`.\n");

    run("ramfs (/tmp): syscall overhead alone", vol, ROUNDS_RAM, PASS_KB);
    run("disk (/var/tmp): what a user would feel", per, ROUNDS_DISK, DISK_KB);

    remove(vol);
    remove(per);
    // Said BEFORE the log is closed, so the file ENDS with it: that is
    // what lets a reader (or a poll) tell a finished run from a
    // truncated one.
    say("stdio_bench: done\n");
    if (g_log) fclose(g_log);
    return 0;
}
