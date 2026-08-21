// kstack -- how deep the kernel stacks have been.
//
//   kstack               one line per live stack, with its high-water mark
//   kstack syscalls      the deepest each syscall has ever gone
//   kstack track on|off  start/stop recording per-syscall depth
//
// USE IT BEFORE A CRASH, not after. Each kernel stack is 16 KiB with an
// unmapped guard page below it, so an overflow faults rather than
// quietly corrupting the next thing -- but a stack at 90% is a bug
// waiting for one more nested call, and this is what shows that while
// the machine is still up.
//
// `used` IS A HIGH-WATER MARK. It is how deep a stack has EVER been,
// not how deep it is now -- a current depth would read near zero for
// every process that is not running at this instant, which is all of
// them.
#include "rt/sys.h"
#include "lib/cmd.h"
#include <stdio.h>
#include <string.h>
#include "lib/tunable.h"

#define TRACK_TUNABLE "kernel.kstack_track"

static int show_stacks(void) {
    struct query_kstack k;
    char line[192];
    int n = 0;

    // The stack size is the same for all of them and is stated once,
    // in the heading, rather than repeated down a column.
    sys_print("kernel stacks: 16 KiB each, guard page below (unmapped)\n"
              "slot pid name              used / size      canary\n");
    for (unsigned i = 0; ; i++) {
        if (sys_query_record(QUERY_KSTACK, i, &k, sizeof k) < (int)sizeof k) break;
        n++;
        // Percent on the SMALLER side first, so a multiply cannot
        // overflow before the divide.
        unsigned long long pct = k.size ? (k.used * 100ull) / k.size : 0;
        // Numbers are formatted THEN padded as strings: kfmt's numeric
        // widths zero-pad, which is right for a timestamp and wrong for
        // every column (CLAUDE.md).
        char used[24];
        snprintf(used, sizeof used, "%llu / %llu",
                 (unsigned long long)k.used, (unsigned long long)k.size);
        // The legacy loader has no slot and no pid, so those columns
        // would print 0 0 and read as init's. Dashes say "not
        // applicable" instead of asserting a wrong number.
        char slot[8], pid[8];
        if (k.flags & QUERY_KSTACK_LEGACY) {
            strlcpy(slot, "--", sizeof slot);
            strlcpy(pid, "--", sizeof pid);
        } else {
            snprintf(slot, sizeof slot, "%llu", (unsigned long long)k.slot);
            snprintf(pid, sizeof pid, "%llu", (unsigned long long)k.pid);
        }
        snprintf(line, sizeof line, "%-4s %-3s %-17s %-16s (%llu%%)  %s\n",
                 slot, pid, k.name, used, pct,
                 (k.flags & QUERY_KSTACK_CANARY_OK) ? "ok" : "DESTROYED");
        sys_print(line);
    }
    if (!n) sys_print("  (no live processes)\n");
    return 0;
}

// `kstack slots` -- the FRAME view: what each stack would resume into,
// rather than how deep it has been. A different question from the
// default, and the one to ask when a process is stuck: `state` says
// what the scheduler thinks it is doing, and rip/cs say where it would
// go back to.
//
// THE INTERESTING CASE IS A MISSING FRAME. A saved kernel_rsp that does
// not point inside this slot's own stack is a FINDING, not a gap -- so
// it is said in words rather than printed as three plausible-looking
// numbers read from wherever that pointer happened to land.
static int show_slots(void) {
    struct query_kstack k;
    char line[192];
    int n = 0;

    sys_print("slot pid name              state  krsp             cs   rip\n");
    for (unsigned i = 0; ; i++) {
        if (sys_query_record(QUERY_KSTACK, i, &k, sizeof k) < (int)sizeof k) break;
        n++;
        char slot[8], pid[8];
        if (k.flags & QUERY_KSTACK_LEGACY) {
            strlcpy(slot, "--", sizeof slot);
            strlcpy(pid, "--", sizeof pid);
        } else {
            snprintf(slot, sizeof slot, "%llu", (unsigned long long)k.slot);
            snprintf(pid, sizeof pid, "%llu", (unsigned long long)k.pid);
        }
        if (k.flags & QUERY_KSTACK_FRAME_OK) {
            snprintf(line, sizeof line, "%-4s %-3s %-17s %-6llu 0x%llx  0x%llx 0x%llx\n",
                     slot, pid, k.name, (unsigned long long)k.state,
                     (unsigned long long)k.kernel_rsp,
                     (unsigned long long)k.cs, (unsigned long long)k.rip);
        } else {
            snprintf(line, sizeof line, "%-4s %-3s %-17s %-6llu 0x%llx  <not in this stack>\n",
                     slot, pid, k.name, (unsigned long long)k.state,
                     (unsigned long long)k.kernel_rsp);
        }
        sys_print(line);
    }
    if (!n) sys_print("  (no live processes)\n");
    return 0;
}

static int show_syscalls(void) {
    struct query_kstack_syscall s;
    char line[128], label[32];
    int n = 0;

    for (unsigned i = 0; ; i++) {
        if (sys_query_record(QUERY_KSTACK_SYSCALL, i, &s, sizeof s) < (int)sizeof s) break;
        if (!n) sys_print("syscall            peak stack\n");
        n++;
        snprintf(label, sizeof label, "%s(%llu)", s.name, (unsigned long long)s.nr);
        snprintf(line, sizeof line, "%-18s %llu bytes\n", label,
                 (unsigned long long)s.peak);
        sys_print(line);
    }
    if (!n) {
        // An empty list and a table of zeroes are different answers,
        // and only one of them means the tracking is working. Say which
        // this is rather than printing an empty heading.
        char cur[64];
        int on = tunable_get(TRACK_TUNABLE, cur, sizeof cur) && strcmp(cur, "on") == 0;
        sys_print(on ? "  (tracking is on, but nothing has been recorded yet)\n"
                     : "  tracking is off -- `kstack track on` first, then\n"
                       "  exercise the paths you care about\n");
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 1) return show_stacks();
    if (argc == 2 && strcmp(argv[1], "syscalls") == 0) return show_syscalls();
    if (argc == 2 && strcmp(argv[1], "slots") == 0) return show_slots();

    if (strcmp(argv[1], "track") == 0) {
        char cur[64];
        if (argc == 2) {
            if (!tunable_get(TRACK_TUNABLE, cur, sizeof cur)) {
                cmd_fail("kstack", TRACK_TUNABLE);
                return 1;
            }
            char line[64];
            snprintf(line, sizeof line, "kstack track: %s\n", cur);
            sys_print(line);
            return 0;
        }
        if (argc == 3) {
            if (!tunable_set(TRACK_TUNABLE, argv[2])) {
                sys_print("kstack: track takes `on` or `off`\n");
                return 1;
            }
            char line[80];
            snprintf(line, sizeof line, "kstack track: %s%s\n", argv[2],
                     strcmp(argv[2], "on") == 0 ? " (table cleared)" : "");
            sys_print(line);
            return 0;
        }
    }

    cmd_usage("kstack | kstack slots | kstack syscalls | kstack track [on|off]");
    return 1;
}
