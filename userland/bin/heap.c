// heap -- the kernel heap's stats, its debug mode, and its scan.
//
//   heap                what kmalloc has and what it has handed out
//   heap debug on|off   red-zone new allocations, poison freed ones
//   heap check          verify every poisoned free block RIGHT NOW
//
// THREE PARTS, TWO MECHANISMS, ONE COMMAND. The stats and the scan are
// FACTS (QUERY_HEAP and QUERY_HEAPCHECK -- reading the second performs
// the scan); the debug switch is a TUNABLE (kernel.heap_debug). That is
// what let this stop being a builtin: moving only the read half would
// have put one command in two rings.
//
// The violation count is printed unconditionally, and that is half the
// point of the command: a violation is logged when it happens and dmesg
// scrolls, so a running total is what makes "has this kernel corrupted
// its heap since boot?" answerable at a glance.
#include "rt/sys.h"
#include "lib/cmd.h"
#include "lib/human.h"
#include "lib/stdio.h"
#include "lib/string.h"
#include "lib/tunable.h"

#define DEBUG_TUNABLE "kernel.heap_debug"

static int show_stats(void) {
    struct query_heap h;
    if (sys_query_record(QUERY_HEAP, 0, &h, sizeof h) < (int)sizeof h) {
        cmd_fail("heap", 0);
        return 1;
    }
    char t[16], u[16], f[16], line[160];
    human_size(t, sizeof t, h.total_bytes);
    human_size(u, sizeof u, h.used_bytes);
    human_size(f, sizeof f, h.free_bytes);

    snprintf(line, sizeof line, "Kernel heap (kmalloc/kfree)\n"
                                "  claimed: %s\n  used:    %s\n  free:    %s\n", t, u, f);
    sys_print(line);
    if (h.quarantined_bytes) {
        // Said only when nonzero, and said plainly: these bytes belong
        // to neither `used` nor `free`, so the three stop summing to
        // `claimed` exactly here. Printing it always would invite the
        // reader to do arithmetic that is usually a no-op.
        human_size(t, sizeof t, h.quarantined_bytes);
        snprintf(line, sizeof line,
                 "  quarantined: %s (withdrawn by red-zone violations,\n"
                 "               counted in neither used nor free)\n", t);
        sys_print(line);
    }
    snprintf(line, sizeof line, "  debug mode: %s\n  red-zone checks: %llu, violations: %llu\n",
             h.debug ? "on" : "off",
             (unsigned long long)h.rz_checks, (unsigned long long)h.violations);
    sys_print(line);
    return 0;
}

static int do_check(void) {
    struct query_heapcheck c;
    // The READ is the scan -- see kernel/mm/heap_query.c.
    if (sys_query_record(QUERY_HEAPCHECK, 0, &c, sizeof c) < (int)sizeof c) {
        cmd_fail("heap", 0);
        return 1;
    }
    char line[160];
    if (!c.checked) {
        // Nothing to look at is not the same as nothing wrong: with the
        // debug mode off there are no poisoned blocks, so a bare "0
        // damaged" would read as a clean bill of health it cannot give.
        sys_print("heap check: no poisoned blocks to verify\n"
                  "  (`heap debug on` first -- only blocks freed while it\n"
                  "   was on carry the poison this scan looks for)\n");
        return 0;
    }
    snprintf(line, sizeof line, "heap check: %llu of %llu poisoned block(s) damaged\n",
             (unsigned long long)c.damaged, (unsigned long long)c.checked);
    sys_print(line);
    // Non-zero exit is the thing a script would branch on.
    return c.damaged ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc == 1) return show_stats();

    if (strcmp(argv[1], "check") == 0 && argc == 2) return do_check();

    if (strcmp(argv[1], "debug") == 0) {
        char cur[64];
        if (argc == 2) {
            if (!tunable_get(DEBUG_TUNABLE, cur, sizeof cur)) {
                cmd_fail("heap", DEBUG_TUNABLE);
                return 1;
            }
            char line[64];
            snprintf(line, sizeof line, "heap debug: %s\n", cur);
            sys_print(line);
            return 0;
        }
        if (argc == 3) {
            if (!tunable_set(DEBUG_TUNABLE, argv[2])) {
                sys_print("heap: debug takes `on` or `off`\n");
                return 1;
            }
            char line[64];
            snprintf(line, sizeof line, "heap debug: %s\n", argv[2]);
            sys_print(line);
            // NOT retroactive, and worth saying: only allocations made
            // from here on carry a red zone, so turning it on does not
            // make the heap you already have checkable.
            sys_print("  (affects subsequent allocations only)\n");
            return 0;
        }
    }

    cmd_usage("heap | heap debug [on|off] | heap check");
    return 1;
}
