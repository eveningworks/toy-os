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
#include <string.h>
#include <stdio.h>
#include "proc_info.h"
#include <unistd.h>

static void put(const char *s) { write(1, s, strlen(s)); }

static const char *state_name(uint32_t s) {
    switch (s) {
        case PROC_STATE_READY:   return "ready";
        case PROC_STATE_RUNNING: return "run";
        case PROC_STATE_BLOCKED: return "block";
        case PROC_STATE_ZOMBIE:  return "zombie";
        case PROC_STATE_STOPPED: return "stopped";
        default:                 return "?";
    }
}

static const char *wait_name(uint32_t w) {
    switch (w) {
        case PROC_WAIT_EVENT: return "event";
        case PROC_WAIT_PIPE:  return "pipe";
        case PROC_WAIT_CHILD: return "child";
        case PROC_WAIT_TIMER: return "timer";
        case PROC_WAIT_KEY:   return "key";
        case PROC_WAIT_TTY:   return "tty";
        case PROC_WAIT_THREAD: return "thread";
        case PROC_WAIT_NET:   return "net";
        case PROC_WAIT_FUTEX: return "futex";
        default:              return "?";
    }
}

// The STATE column: `block(pipe)` rather than a bare `block`.
//
// **INSIDE THE STATE, NOT BESIDE IT.** Linux's `ps` puts this in a
// separate WCHAN column naming the kernel symbol being slept on
// (`ps -o stat,wchan`), which is the right shape when the answer is one
// of hundreds of addresses. Here there are five reasons and every one of
// them is meaningless in any other state, so a seventh column would be
// blank on every runnable row and would cost width an 80-column console
// does not have. One column, one fact.
//
// An unknown reason prints `block(?)` rather than a number: this is a
// ring-3 binary reading an enumeration the kernel owns, and a kernel
// that gained a sixth reason without teaching this one about it should
// say so visibly. The `procinfo` KTESTs are what make that a rare sight.
static void fmt_state(const struct proc_info *p, char *out, int cap) {
    if (p->state != PROC_STATE_BLOCKED || p->wait_reason == PROC_WAIT_NONE) {
        snprintf(out, cap, "%s", state_name(p->state));
        return;
    }
    snprintf(out, cap, "block(%s)", wait_name(p->wait_reason));
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

// The branch drawn in front of a name in --tree. `prefix` is what every
// ANCESTOR contributed -- a vertical guide where that ancestor still has
// siblings below it, blanks where it was the last -- and the caller adds
// this row's own connector. ASCII, not the box-drawing characters this
// would otherwise want: the font draws 101 glyphs (api/font_ttf.h) and
// none of them is a line, so `|` and a backtick is the whole palette.
// pstree -A is the same drawing for the same reason.
static void print_row(const struct proc_info *p, const char *prefix) {
    // Every column goes through %Ns, including the numeric ones: kfmt's
    // numeric width ZERO-pads (`%5u` of 1 is "00001"), which is right
    // for a timestamp and wrong for a table. Formatting the number
    // first and padding it as a STRING is how you get a right-aligned
    // column here.
    char pid[12], ppid[12], pgid[12], state[16], cpu[24], mem[24], line[224];
    snprintf(pid, sizeof pid, "%u", (unsigned)p->pid);
    snprintf(ppid, sizeof ppid, "%u", (unsigned)p->ppid);
    snprintf(pgid, sizeof pgid, "%u", (unsigned)p->pgid);
    fmt_state(p, state, sizeof state);
    fmt_cpu(p->cpu_ns, cpu, sizeof cpu);
    snprintf(mem, sizeof mem, "%u", (unsigned)(p->mem_bytes / 1024));

    // A THREAD IS NAMED IN BRACES, because it has no name of its own:
    // it carries its leader's, so `tosh` twice in a listing would look
    // like two shells rather than one with a thread. `htop` colours
    // them instead, which a text column cannot.
    snprintf(line, sizeof line, "%5s %5s %5s %-12s %8s %8s  %s%s%s%s\n",
             pid, ppid, pgid, state, cpu, mem, prefix ? prefix : "",
             p->tgid == p->pid ? "" : "{", p->name,
             p->tgid == p->pid ? "" : "}");
    put(line);
}

static void header(void) {
    // PGID is beside PPID because the two answer different questions
    // that look alike: the parent is who STARTED it, the group is what a
    // signal REACHES. `kill -TERM -<pgid>` and Ctrl-C act on the second,
    // and neither was visible from a shell before.
    //
    // STATE is twelve wide because the longest thing it holds is
    // `block(child)` -- widened rather than truncated, since it is the
    // one column here whose tail is the whole content.
    put("  PID  PPID  PGID STATE          CPU(s)   MEM(K)  NAME\n");
}

// Reads the whole table once. A snapshot rather than a slot-at-a-time
// walk because --tree has to look at every row before drawing any of
// them, and because a table that changes under a walk would otherwise
// print a child above the parent it names.
static int snapshot(struct proc_info *out, int cap, int with_threads) {
    int n = 0;
    struct proc_info info;
    for (int i = 0; i < SYS_PROC_MAX && n < cap; i++) {
        if (sys_proc_info(i, &info) != 0) continue;
        if (info.pid == 0) continue; // empty slot -- skip, never stop
        // HIDDEN BY DEFAULT, as in every Unix ps: a program's threads
        // are an implementation detail of that program, and a listing
        // that shows six of them where a person expects one process is
        // a worse default than one that needs a flag.
        if (!with_threads && info.tgid != info.pid) continue;
        out[n++] = info;
    }
    return n;
}

// Depth-first, printing every child of `parent` under it. O(n^2) over a
// table bounded at SYS_PROC_MAX, which is 64 -- a real tree walk would
// need a child list the ABI does not carry.
// THE LAST CHILD IS DRAWN DIFFERENTLY, which is the whole reason to draw
// branches rather than indent: a backtick closes a subtree, so a reader
// can see where one ends. Plain indentation cannot express that at all.
// Finding it needs a look ahead, since the ABI carries no child list.
static int last_child(const struct proc_info *procs, int n, int parent, int i) {
    for (int j = i + 1; j < n; j++)
        if (procs[j].ppid == parent) return 0;
    return 1;
}

static void print_tree(struct proc_info *procs, int n, int parent, int depth,
                       const char *prefix) {
    char self[64], next[64];
    for (int i = 0; i < n; i++) {
        if (procs[i].ppid != parent) continue;
        int last = last_child(procs, n, parent, i);

        // A ROOT HAS NO CONNECTOR. Drawing one would put a branch in
        // front of init with nothing above it to branch from.
        if (depth == 0) {
            print_row(&procs[i], "");
            k_snprintf(next, sizeof next, "%s", "");
        } else {
            k_snprintf(self, sizeof self, "%s%s", prefix, last ? "`-- " : "|-- ");
            print_row(&procs[i], self);
            // What this row contributes to its OWN children's prefix: a
            // guide while it still has siblings coming, blanks once it
            // is the last -- otherwise a vertical line runs down the
            // page past a subtree that has already closed.
            k_snprintf(next, sizeof next, "%s%s", prefix, last ? "    " : "|   ");
        }

        // A process cannot be its own parent (scheduler_reparent()
        // refuses it), so this cannot recurse forever -- but bound the
        // depth anyway rather than trusting a value that crossed the
        // syscall boundary. The prefix is bounded with it: 8 levels of
        // four characters fits `next` with room to spare.
        if (depth < 8) print_tree(procs, n, procs[i].pid, depth + 1, next);
    }
}

int main(int argc, char **argv) {
    int tree = 0, threads = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--tree") == 0 || strcmp(argv[i], "-t") == 0) {
            tree = 1;
        } else if (strcmp(argv[i], "--threads") == 0 || strcmp(argv[i], "-T") == 0) {
            threads = 1;
        } else {
            put("usage: ps [--tree] [--threads]\n");
            return 1;
        }
    }

    static struct proc_info procs[SYS_PROC_MAX];
    int n = snapshot(procs, SYS_PROC_MAX, threads);

    header();
    if (!tree) {
        for (int i = 0; i < n; i++) print_row(&procs[i], "");
        return 0;
    }

    // Roots first (ppid 0 -- "the kernel spawned it"), then everything
    // reachable below them. Anything left over is printed flat rather
    // than dropped: a row whose parent is not in the table would
    // otherwise vanish, which is the one failure mode a process listing
    // must not have.
    print_tree(procs, n, 0, 0, "");
    for (int i = 0; i < n; i++) {
        if (procs[i].ppid == 0) continue;
        int seen = 0;
        for (int j = 0; j < n; j++) if (procs[j].pid == procs[i].ppid) seen = 1;
        if (!seen) print_row(&procs[i], "");
    }
    return 0;
}
