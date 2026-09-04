// crashlog -- the ring-3 crash reports in /var/crash: list them, or
// print one's text header. The kernel writes a report on the way to
// killing a process that faulted (kernel/proc/crash_report.c); the raw
// stack after the header is for tools/panic_resolve.py --crash on the
// host, which names every address against the ELF's DWARF, so this
// program stops at the marker rather than printing a page of bytes.
//
// A KERNEL PANIC IS NOT HERE. This directory holds processes that
// faulted while the kernel carried on; a panic's record is a separate
// thing (docs/roadmap.md, crash reporting), kept apart so a list of
// "what crashed" never conflates the system working with the system
// failing.
#include <stdint.h>
#include "rt/sys.h"
#include "lib/cmd.h"
#include "lib/dirsort.h"
#include <string.h>
#include <stdio.h>

#define CRASH_DIR "/var/crash"
#define USAGE "crashlog [<report>]"
#define MAX_ENTRIES 64

static int list(void) {
    static struct sys_dirent ents[MAX_ENTRIES];
    int n = sys_listdir(CRASH_DIR, ents, MAX_ENTRIES);
    if (n < 0) {
        sys_print("crashlog: no reports (" CRASH_DIR " does not exist)\n");
        return 0;
    }
    // NEWEST FIRST, unlike the other listings here: the report anyone
    // wants is the one from the crash that just happened. dirsort's name
    // tie-break keeps it deterministic if two share a timestamp.
    dirsort(ents, n, DIRSORT_TIME, 0);
    int shown = 0;
    for (int i = 0; i < n; i++) {
        if (ents[i].is_dir) continue;
        char line[128];
        snprintf(line, sizeof line, "%-40s %8u bytes\n", ents[i].name, (unsigned)ents[i].size);
        sys_print(line);
        shown++;
    }
    if (!shown) sys_print("crashlog: no reports\n");
    return 0;
}

static int show(const char *name) {
    char path[64];
    if (name[0] == '/') snprintf(path, sizeof path, "%s", name);
    else                snprintf(path, sizeof path, CRASH_DIR "/%s", name);
    int fd = sys_open(path, 0);
    if (fd < 0) { cmd_fail("crashlog", path); return 1; }
    // Read up to the stack marker and no further: what follows is binary.
    static char buf[16384];
    int total = 0;
    while (total < (int)sizeof buf - 1) {
        int n = sys_read(fd, buf + total, (int)sizeof buf - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    sys_close(fd);
    buf[total] = 0;
    char *mark = strstr(buf, "---- stack ----");
    if (mark) *mark = 0;
    sys_print(buf);
    if (mark) sys_print("(stack bytes follow; resolve with tools/panic_resolve.py --crash on the host)\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 2) { cmd_usage(USAGE); return 1; }
    return argc == 2 ? show(argv[1]) : list();
}
