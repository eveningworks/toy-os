// stalls -- how long each syscall held the CPU.
//
//   stalls               one line per syscall measured, worst first
//   stalls dist          the same as a log2 histogram, summed over all of them
//   stalls track on|off  start/stop recording
//   stalls reset         re-arm from zero
//
// WHAT IT MEASURES, AND WHY THAT IS THE INTERESTING NUMBER. A syscall
// handler runs with interrupts off, so its duration is not how long the
// CALLER waited -- it is how long nothing else on the machine ran. A
// disk write that takes 40 ms in the handler stops the compositor, the
// timer tick and every other process for 40 ms, and the desktop feels
// that as a dropped frame. `gui latency` counts the dropped frames from
// the other end; this says which syscall spent them.
//
// Linux's equivalents are ftrace's irqsoff tracer and bcc's funclatency;
// the histogram shape here is theirs, and the buckets are the
// compositor's own so the two reports can be read side by side.
//
// OFF BY DEFAULT, BECAUSE THE INSTRUMENT COSTS SOMETHING: armed, the
// kernel reads the TSC twice per syscall. A number measured with this on
// is from a slightly slower machine than the one without it.
//
// IT IS TIMED WITH THE TSC, NOT WITH THE SYSTEM CLOCK, and the rate is
// printed because that is what the microseconds mean. The system
// clocksource cannot answer this question at all -- it is the PIT tick
// counter on every default boot, and the timer interrupt that increments
// it is off for exactly the window being measured, so it reads the same
// value at both ends and every stall comes out as zero. Without an
// INVARIANT TSC the rate drifts as the CPU throttles, which is accepted
// here: this measures millisecond-scale stalls.
//
// THE OUTPUT SHAPE IS A CONTRACT -- tools/latency_under_io.py parses it.
// One line per syscall, columns in this order:
//
//   <name>(<nr>)  <calls>  <max us>  <avg us>  <total us>
#include "rt/sys.h"
#include "lib/cmd.h"
#include "lib/tunable.h"
#include <stdio.h>
#include <string.h>

#define STALL_TUNABLE "kernel.syscall_stall"

// Worst-first, because the question is "what held the machine", and the
// answer is one or two rows. Bounded by the syscall table, which is well
// under a hundred rows, so an insertion sort over the records read is
// cheaper than anything cleverer and needs no allocation.
#define MAX_ROWS 128

static struct query_syscall_stall g_rows[MAX_ROWS];

static int read_rows(void) {
    int n = 0;
    for (unsigned i = 0; n < MAX_ROWS; i++) {
        if (sys_query_record(QUERY_SYSCALL_STALL, i, &g_rows[n], sizeof g_rows[0])
            < (int)sizeof g_rows[0]) break;
        n++;
    }
    // Sort by the worst single call, not by the total: a syscall called
    // a million times for 2 us each has a large sum and stalls nothing,
    // and putting it at the top would bury the one that does.
    for (int i = 1; i < n; i++) {
        struct query_syscall_stall t = g_rows[i];
        int j = i - 1;
        while (j >= 0 && g_rows[j].max_us < t.max_us) { g_rows[j + 1] = g_rows[j]; j--; }
        g_rows[j + 1] = t;
    }
    return n;
}

static int tracking_on(void) {
    char cur[64];
    return tunable_get(STALL_TUNABLE, cur, sizeof cur) && strcmp(cur, "on") == 0;
}

// An empty list and a table of zeroes are different answers, and only
// one of them means the tracking is working. Say which this is.
static void say_why_empty(void) {
    if (tracking_on())
        sys_print("  (tracking is on, but no syscall has been measured yet)\n");
    else
        sys_print("  tracking is off -- `stalls track on`, run the workload,\n"
                  "  then `stalls` again\n");
}

static void print_clock(unsigned mhz) {
    char line[128];
    if (!mhz) { sys_print("timed with the TSC (rate unknown)\n"); return; }
    snprintf(line, sizeof line,
             "timed with the TSC at %u MHz -- not the system clock, which cannot\n"
             "see a window with interrupts off (see stalls.c)\n", mhz);
    sys_print(line);
}

static int show_table(void) {
    int n = read_rows();
    if (!n) { say_why_empty(); return 0; }
    print_clock(g_rows[0].tsc_mhz);
    sys_print("syscall              calls      max us      avg us    total us\n");
    for (int i = 0; i < n; i++) {
        struct query_syscall_stall *r = &g_rows[i];
        char label[32], line[160];
        snprintf(label, sizeof label, "%s(%llu)", r->name, (unsigned long long)r->nr);
        unsigned long long avg = r->n ? r->sum_us / r->n : 0;
        snprintf(line, sizeof line, "%-20s %6llu  %10llu  %10llu  %10llu\n",
                 label, (unsigned long long)r->n, (unsigned long long)r->max_us,
                 avg, (unsigned long long)r->sum_us);
        sys_print(line);
    }
    return 0;
}

// The histogram summed over every syscall: the SHAPE of the machine's
// stalls, where the table above is the attribution. Empty buckets are
// skipped -- twenty-two rows of zero would bury the two that matter.
static int show_dist(void) {
    int n = read_rows();
    if (!n) { say_why_empty(); return 0; }
    print_clock(g_rows[0].tsc_mhz);

    unsigned long long total[QUERY_SYSCALL_STALL_BUCKETS] = {0};
    unsigned long long calls = 0, worst = 0;
    for (int i = 0; i < n; i++) {
        for (int b = 0; b < QUERY_SYSCALL_STALL_BUCKETS; b++)
            total[b] += g_rows[i].bucket[b];
        calls += g_rows[i].n;
        if (g_rows[i].max_us > worst) worst = g_rows[i].max_us;
    }

    char line[160];
    snprintf(line, sizeof line, "%llu call(s), worst single stall %llu us\n", calls, worst);
    sys_print(line);
    sys_print("      range      calls\n");
    for (int b = 0; b < QUERY_SYSCALL_STALL_BUCKETS; b++) {
        if (!total[b]) continue;
        unsigned long long lo = 1ull << b;
        if (b == QUERY_SYSCALL_STALL_BUCKETS - 1)
            snprintf(line, sizeof line, "  >= %8llu us %10llu\n", lo, total[b]);
        else if (b == 0)
            snprintf(line, sizeof line, "  <  %8llu us %10llu\n", 2ull, total[b]);
        else
            snprintf(line, sizeof line, "  >= %8llu us %10llu\n", lo, total[b]);
        sys_print(line);
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 1) return show_table();

    if (!strcmp(argv[1], "dist") && argc == 2) return show_dist();

    if (!strcmp(argv[1], "track") && argc == 3) {
        if (strcmp(argv[2], "on") && strcmp(argv[2], "off")) {
            cmd_usage("stalls [dist | track on|off | reset]");
            return 1;
        }
        if (!tunable_set(STALL_TUNABLE, argv[2])) {
            sys_print("stalls: the kernel refused that\n");
            return 1;
        }
        sys_print(strcmp(argv[2], "on") == 0
                  ? "syscall stall timing on (counters zeroed)\n"
                  : "syscall stall timing off\n");
        return 0;
    }

    if (!strcmp(argv[1], "reset") && argc == 2) {
        // Off then on, because arming from OFF is what zeroes -- and the
        // settings registry makes writing a value that is already set a
        // no-op, so a bare `on` while already on would leave the old
        // totals in place and look like a reset that did nothing.
        if (!tunable_set(STALL_TUNABLE, "off") || !tunable_set(STALL_TUNABLE, "on")) {
            sys_print("stalls: the kernel refused that\n");
            return 1;
        }
        sys_print("syscall stall timing on (counters zeroed)\n");
        return 0;
    }

    cmd_usage("stalls [dist | track on|off | reset]");
    return 1;
}
