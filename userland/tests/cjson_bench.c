// A BENCHMARK, not a test -- and the difference matters for how it is
// used. cjson_test asserts a round trip and is in the gate; this
// measures throughput and is NOT, because a timing number varies with
// the host and would make the gate flap for reasons that have nothing
// to do with this OS.
//
// **WHAT THE NUMBER IS WORTH.** Every automated run here is TCG, which
// interprets rather than executes -- so the absolute figure measures
// the EMULATOR, not the machine, and comparing it against a real
// computer is meaningless. It is worth exactly two things: the same
// build under `vm.py --kvm` against the same build under TCG, and one
// commit against another on the same host. Both are ratios, which is
// what a number from an emulator can honestly support.
//
// It reports CPU time and WALL time separately. clock() is real
// per-process cpu_ns (SYS_GETPID made that possible); wall comes from
// the monotonic clock. Under TCG the two diverge whenever the host is
// busy, and a large gap between them means the measurement was
// disturbed rather than that the guest got slower.
//
// **RUN IT WITH `spawn`, NOT `run`.** The legacy `run` loader has no
// scheduler slot, so SYS_GETPID answers -1, clock() reports
// "unavailable" and every CPU figure collapses to zero -- which looks
// like an impossibly fast machine rather than like a missing
// measurement. It says so at the top of its output rather than leaving
// a reader to wonder, and the wall-clock numbers are still valid there.
//
//     spawn /tests/cjson_bench     (CPU + wall)
//     run cjson_bench              (wall only)
//
// WHAT IT EXERCISES, which is the reason it is cJSON and not a loop
// doing arithmetic: parse and print between them hit malloc/realloc on
// every node, strtod on every number, sprintf on every number written
// back, and most of <string.h>. It is a load test of tolibc's
// allocator and formatter wearing a JSON hat.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdarg.h>
#include "rt/sys.h"
#include "ports/cjson/cJSON.h"

// Sized against the CLOCK'S RESOLUTION, not by taste. The monotonic
// clock ticks at 100 Hz here, so a phase that finishes in 40 ms is
// measured in four ticks and every figure it produces is quantised to
// 25%. At 200 rounds each phase runs for a few hundred milliseconds and
// the quantisation drops under 3%.
//
// The first version used 20 rounds and reported timings that were exact
// multiples of 10000 us -- which is what a measurement pinned to the
// tick looks like, and is worth recognising before believing a number.
//
// Both constants are printed, so a figure always arrives with the work
// it describes: "2000 us/op" means nothing on its own.
#define RECORDS 40
#define ROUNDS  200

static char *g_doc;
static size_t g_doclen;

#define LOG_PATH "/tmp/cjson_bench.out"
static FILE *g_log;

// Everything goes to the console AND to a file, because the useful way
// to run this is `spawn` (only that gives CPU figures -- see the header)
// and a spawned program's console output arrives while the harness is
// between commands, where it is dropped. A file can be asked for at any
// time: wait on the artifact, not on the timing.
//
// It is also this benchmark quietly eating its own cooking -- vfprintf
// into a buffered FILE is exactly the machinery being measured.
static void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (g_log) {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
    }
}

// Builds the fixture through cJSON's own API, so the benchmark does not
// also depend on a hand-written JSON string staying valid.
static void build_fixture(void) {
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < RECORDS; i++) {
        cJSON *rec = cJSON_CreateObject();
        char nm[32];
        snprintf(nm, sizeof nm, "record-%d", i);
        cJSON_AddStringToObject(rec, "name", nm);
        cJSON_AddNumberToObject(rec, "index", i);
        cJSON_AddNumberToObject(rec, "ratio", i * 1.375);
        cJSON_AddBoolToObject(rec, "active", i % 3 != 0);
        cJSON *tags = cJSON_CreateArray();
        for (int j = 0; j < 4; j++) cJSON_AddItemToArray(tags, cJSON_CreateNumber(i * 4 + j));
        cJSON_AddItemToObject(rec, "tags", tags);
        cJSON_AddItemToArray(arr, rec);
    }
    cJSON_AddItemToObject(root, "records", arr);
    g_doc = cJSON_PrintUnformatted(root);
    g_doclen = g_doc ? strlen(g_doc) : 0;
    cJSON_Delete(root);
}

// The clock ticks at 100 Hz, so anything under two ticks carries less
// than one significant figure. REPORTING THAT IS THE POINT: an earlier
// version divided by a floor of 1 us and printed "657800000 KB/s" for a
// phase that had simply finished between ticks, which is not a fast
// result but an unmeasured one. Under KVM the print phase does exactly
// that, so this is a case the benchmark meets in normal use rather than
// a theoretical guard.
#define RESOLUTION_US 20000   // two ticks at 100 Hz

// Rates are integer arithmetic: the numbers are large enough to be
// exact, and a %f nobody can compare by eye is worse.
static void report(const char *what, uint64_t us, uint64_t ops, uint64_t bytes) {
    if (us < RESOLUTION_US) {
        say("  %-22s %8llu us total  -- too fast to measure at %llu rounds\n",
            what, (unsigned long long)us, (unsigned long long)ops);
        return;
    }
    say("  %-22s %8llu us total  %7llu us/op  %6llu KB/s\n",
           what,
           (unsigned long long)us,
           (unsigned long long)(us / ops),
           (unsigned long long)((bytes * 1000ull) / us));
}

int main(void) {
    // Opened before anything is printed, so the file holds the whole
    // report rather than everything after the first timing.
    g_log = fopen(LOG_PATH, "w");
    say("cjson_bench: tolibc under load, via cJSON\n");

    build_fixture();
    if (!g_doc) { say("  FAILED to build the fixture\n"); return 1; }
    say("  document: %llu bytes, %d records, %d rounds\n",
           (unsigned long long)g_doclen, RECORDS, ROUNDS);

    // Asked ONCE, up front. A benchmark that silently reports zero CPU
    // time reads as an impossibly fast machine; this reads as a missing
    // measurement, which is what it is.
    int have_cpu = (clock() != (clock_t)-1);
    if (!have_cpu)
        say("  NOTE: no CPU time available -- this is the legacy `run` loader,\n"
               "        which has no scheduler slot. Use `spawn /tests/cjson_bench`\n"
               "        for CPU figures. Wall-clock numbers below are still valid.\n");

    // --- parse ---------------------------------------------------------
    clock_t c0 = clock();
    uint64_t w0 = sys_monotonic_ns();
    for (int r = 0; r < ROUNDS; r++) {
        cJSON *d = cJSON_Parse(g_doc);
        if (!d) { say("  FAILED: parse returned NULL on round %d\n", r); return 1; }
        cJSON_Delete(d);
    }
    uint64_t parse_cpu = (uint64_t)(clock() - c0);
    uint64_t parse_wall = (sys_monotonic_ns() - w0) / 1000u;

    // --- print ---------------------------------------------------------
    cJSON *doc = cJSON_Parse(g_doc);
    c0 = clock();
    w0 = sys_monotonic_ns();
    for (int r = 0; r < ROUNDS; r++) {
        char *s = cJSON_PrintUnformatted(doc);
        if (!s) { say("  FAILED: print returned NULL on round %d\n", r); return 1; }
        free(s);
    }
    uint64_t print_cpu = (uint64_t)(clock() - c0);
    uint64_t print_wall = (sys_monotonic_ns() - w0) / 1000u;
    cJSON_Delete(doc);

    // --- round trip ----------------------------------------------------
    c0 = clock();
    w0 = sys_monotonic_ns();
    for (int r = 0; r < ROUNDS; r++) {
        cJSON *d = cJSON_Parse(g_doc);
        char *s = cJSON_PrintUnformatted(d);
        free(s);
        cJSON_Delete(d);
    }
    uint64_t trip_cpu = (uint64_t)(clock() - c0);
    uint64_t trip_wall = (sys_monotonic_ns() - w0) / 1000u;

    uint64_t total = g_doclen * ROUNDS;
    if (have_cpu) {
        say("  --- CPU time (clock(), real per-process cpu_ns) ---\n");
        report("parse", parse_cpu, ROUNDS, total);
        report("print", print_cpu, ROUNDS, total);
        report("parse+print", trip_cpu, ROUNDS, total);
    }
    say("  --- wall time (monotonic) ---\n");
    report("parse", parse_wall, ROUNDS, total);
    report("print", print_wall, ROUNDS, total);
    report("parse+print", trip_wall, ROUNDS, total);

    // A large gap between CPU and wall means the measurement was
    // DISTURBED -- another process ran, or the host was busy -- not
    // that the guest is slow. Saying so beats leaving a reader to
    // compare two tables and guess.
    if (have_cpu && trip_wall > trip_cpu * 2 && trip_cpu > 0)
        say("  NOTE: wall time is more than twice CPU time -- something else\n"
               "        was running, so treat these numbers as disturbed.\n");

    free(g_doc);
    say("cjson_bench: done (a TCG figure is a RATIO, not a speed)\n");
    if (g_log) fclose(g_log);   // the flush is what puts it on disk
    return 0;
}
