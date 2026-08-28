// kill -- send a signal to a process or a process group.
//
// SIGNALS NOW. This used to say "NOT SIGNALS -- this OS has none", and
// the name was the word people type while the semantics were `kill -9`
// and only that. SYS_KILL takes a signal number today
// (abi/signal_abi.h), so the default is SIGTERM, `-9` and `-KILL` mean
// what they mean everywhere, and a NEGATIVE pid names a group.
//
// IT NAMES THE PROCESS BEFORE SIGNALLING IT. After a fatal signal the
// slot is a zombie whose name is still readable, but "signalled pid 4"
// is only useful if it says WHICH process that was -- and the caller's
// mental model of pid 4 is what needs confirming, before it is too late
// to check.
//
// WHAT `kill` CANNOT PROMISE, and the page says so too: a signal other
// than SIGKILL is PENDING when this program exits. The kernel acts on it
// when the target next returns to ring 3, so a process that is running
// dies within a timer tick and one wedged inside a kernel path may not
// die at all -- which is exactly why SIGKILL bypasses the mechanism and
// why it is the last resort rather than the first.
#include <stdint.h>
#include "rt/sys.h"
#include "lib/cmd.h"
#include <stdio.h>
#include <string.h>
#include <knum.h>    // k_parse_u32 -- kernel/lib/, linked into libuapp.a
#include <ksignal.h> // signal_name/signal_from_name -- ONE table, both rings

// The name of `pid`, or "" if the process table has no such live entry.
// Walks by SLOT, which is not the pid -- proc_info carries the pid, and
// assuming slot+1 would be right only until a slot is reused.
static void name_of(int pid, char *out, unsigned long cap) {
    struct proc_info info;
    out[0] = '\0';
    for (int i = 0; sys_proc_info(i, &info) == 0; i++) {
        if (info.pid == pid) { strlcpy(out, info.name, cap); return; }
    }
}

int main(int argc, char **argv) {
    int sig = SIGTERM;   // POSIX's default, and the polite one
    int first = 1;

    // ONE leading `-SIG`, applying to every target after it. Not a
    // general flag parser: `kill -9 -TERM 4` is not a thing anybody
    // means, and accepting it would leave the question of which one won.
    if (argc >= 2 && argv[1][0] == '-' && argv[1][1] != '\0') {
        // `-<pid>` is a GROUP, not a signal -- POSIX's spelling, and the
        // reason this has to be decided before the signal parse: "-9"
        // reads as both. The signal wins, which is what every Unix does;
        // a group is named with an explicit `-- -9` or by putting the
        // signal first.
        sig = signal_from_name(argv[1] + 1);
        if (sig) {
            first = 2;
        } else if (!k_parse_u32((const char *)argv[1] + 1, &(uint32_t){0})) {
            // Neither a signal nor a number: a typo, and a typo in the
            // argument that says WHAT TO DO is not something to shrug
            // off and default.
            sys_print("kill: not a signal: ");
            sys_print(argv[1] + 1);
            sys_print("\n  (try TERM, KILL, INT, QUIT, or a number)\n");
            return 1;
        } else {
            sig = SIGTERM;   // `-9` was a group; keep the default signal
        }
    }

    if (argc <= first) {
        cmd_usage("kill [-SIGNAL] <pid|-pgid> [pid...]    (`ps` for pids)");
        return 1;
    }

    int failed = 0;
    for (int i = first; i < argc; i++) {
        const char *arg = argv[i];
        int group = (arg[0] == '-');
        if (group) arg++;

        // k_parse_u32 REJECTS rather than guessing (CLAUDE.md), so "12x"
        // is refused instead of silently becoming 12 -- which matters
        // when the argument names something to destroy.
        uint32_t parsed = 0;
        if (!k_parse_u32(arg, &parsed) || parsed == 0) {
            sys_print("kill: not a pid: ");
            sys_print(argv[i]);
            sys_print("\n");
            failed = 1;
            continue;
        }
        int pid = (int)parsed;

        char name[PROC_NAME_MAX];
        if (!group) name_of(pid, name, sizeof name);
        else        name[0] = '\0';

        char line[160];
        if (sys_kill(group ? -pid : pid, sig) != 0) {
            snprintf(line, sizeof line, "kill: no %s %d\n",
                     group ? "such process group" : "process with pid", pid);
            sys_print(line);
            failed = 1;
            continue;
        }
        snprintf(line, sizeof line, "sent SIG%s to %s %d%s%s\n",
                 signal_name(sig), group ? "group" : "pid", pid,
                 name[0] ? " -- " : "", name);
        sys_print(line);
    }
    return failed;
}
