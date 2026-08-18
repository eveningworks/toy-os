// `ps` -- what is running, and who started it.
//
// A real ring-3 ELF in /bin driven by SYS_PROC_INFO, like `lscpu` and
// `lspci` beside it, rather than a kernel shell builtin. Two reasons
// that matters here: the same binary answers from the physical shell
// (through PATH) and from the ring-3 Terminal, and it is the shape
// docs/init-design.md's stage 5 wants every kernel-shell command to end
// up in -- so building it kernel-side would have been building
// something that has to move again.
//
// **The PPID column is the point.** The process tree gained parent
// links (stage 0) and init gained a pid to adopt orphans to (stage 1),
// and until this there was no way to SEE either from a shell: a ppid
// existed in the ABI with no reader. `--tree` draws it.
//
// Enumeration is by SLOT, and an empty slot is a SUCCESSFUL report of
// pid 0 rather than an error -- so this skips rather than stopping, per
// SYS_PROC_INFO's own ABI comment.
//
// **It cannot see ITSELF when typed at the physical shell**, and that
// is not a bug in the listing: the kernel shell runs a /bin binary
// through the legacy blocking loader, which has no procs[] slot at all,
// so there is nothing in the table to report. Spawned properly (`spawn
// /bin/ps`, or from the ring-3 Terminal) it appears like anything
// else.
#include <stdint.h>
#include "rt/sys.h"
#include "lib/string.h"
#include "lib/stdio.h"
#include "proc_info.h"

static void put(const char *s) { sys_write(1, s, strlen(s)); }

static const char *state_name(uint32_t s) {
    switch (s) {
        case PROC_STATE_READY:   return "ready";
        case PROC_STATE_RUNNING: return "run";
        case PROC_STATE_BLOCKED: return "block";
        case PROC_STATE_ZOMBIE:  return "zombie";
        default:                 return "?";
    }
}

// CPU time as seconds with two decimals. Milliseconds would be the
// obvious unit and is the wrong one: cpu_ns is CUMULATIVE over a
// process's whole life, so a long-lived desktop reads in the thousands
// and the column stops being scannable.
static void fmt_cpu(uint64_t ns, char *out, int cap) {
    uint64_t cs = ns / 10000000ull; // centiseconds
    snprintf(out, cap, "%u.%u%u", (unsigned)(cs / 100),
             (unsigned)((cs / 10) % 10), (unsigned)(cs % 10));
}

static void print_row(const struct proc_info *p, int depth) {
    // Every column goes through %Ns, including the numeric ones: kfmt's
    // numeric width ZERO-pads (`%5u` of 1 is "00001"), which is right
    // for a timestamp and wrong for a table. Formatting the number
    // first and padding it as a STRING is how you get a right-aligned
    // column here.
    char pid[12], ppid[12], cpu[24], mem[24], line[160], indent[24];
    snprintf(pid, sizeof pid, "%u", (unsigned)p->pid);
    snprintf(ppid, sizeof ppid, "%u", (unsigned)p->ppid);
    fmt_cpu(p->cpu_ns, cpu, sizeof cpu);
    snprintf(mem, sizeof mem, "%u", (unsigned)(p->mem_bytes / 1024));

    int n = depth * 2;
    if (n > (int)sizeof indent - 1) n = (int)sizeof indent - 1;
    for (int i = 0; i < n; i++) indent[i] = ' ';
    indent[n] = '\0';

    snprintf(line, sizeof line, "%5s %5s %-7s %8s %8s  %s%s\n",
             pid, ppid, state_name(p->state), cpu, mem, indent, p->name);
    put(line);
}

static void header(void) {
    put("  PID  PPID STATE      CPU(s)   MEM(K)  NAME\n");
}

// Reads the whole table once. A snapshot rather than a slot-at-a-time
// walk because --tree has to look at every row before drawing any of
// them, and because a table that changes under a walk would otherwise
// print a child above the parent it names.
static int snapshot(struct proc_info *out, int cap) {
    int n = 0;
    struct proc_info info;
    for (int i = 0; i < SYS_PROC_MAX && n < cap; i++) {
        if (!sys_proc_info(i, &info)) continue;
        if (info.pid == 0) continue; // empty slot -- skip, never stop
        out[n++] = info;
    }
    return n;
}

// Depth-first, printing every child of `parent` under it. O(n^2) over a
// table bounded at SYS_PROC_MAX, which is 64 -- a real tree walk would
// need a child list the ABI does not carry.
static void print_tree(struct proc_info *procs, int n, int parent, int depth) {
    for (int i = 0; i < n; i++) {
        if (procs[i].ppid != parent) continue;
        print_row(&procs[i], depth);
        // A process cannot be its own parent (scheduler_reparent()
        // refuses it), so this cannot recurse forever -- but bound the
        // depth anyway rather than trusting a value that crossed the
        // syscall boundary.
        if (depth < 8) print_tree(procs, n, procs[i].pid, depth + 1);
    }
}

int main(int argc, char **argv) {
    int tree = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--tree") == 0 || strcmp(argv[i], "-t") == 0) {
            tree = 1;
        } else {
            put("usage: ps [--tree]\n");
            return 1;
        }
    }

    static struct proc_info procs[SYS_PROC_MAX];
    int n = snapshot(procs, SYS_PROC_MAX);

    header();
    if (!tree) {
        for (int i = 0; i < n; i++) print_row(&procs[i], 0);
        return 0;
    }

    // Roots first (ppid 0 -- "the kernel spawned it"), then everything
    // reachable below them. Anything left over is printed flat rather
    // than dropped: a row whose parent is not in the table would
    // otherwise vanish, which is the one failure mode a process listing
    // must not have.
    print_tree(procs, n, 0, 0);
    for (int i = 0; i < n; i++) {
        if (procs[i].ppid == 0) continue;
        int seen = 0;
        for (int j = 0; j < n; j++) if (procs[j].pid == procs[i].ppid) seen = 1;
        if (!seen) print_row(&procs[i], 0);
    }
    return 0;
}
