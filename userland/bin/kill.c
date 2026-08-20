// kill -- end a process by pid.
//
// NOT SIGNALS. This OS has none (docs/roadmap.md's "Signals & process
// control"), so there is no -9, no -TERM, and nothing here pretends
// otherwise: SYS_KILL ends the process and sets its exit code. The name
// is the word people type; the semantics are `kill -9` and only that.
//
// IT NAMES THE PROCESS BEFORE ENDING IT. After the kill the slot is a
// zombie whose name is still readable, but "ended pid 4" is only useful
// if it says WHICH process that was -- and the caller's mental model of
// pid 4 is what needs confirming, before it is too late to check.
#include <stdint.h>
#include "rt/sys.h"
#include "lib/cmd.h"
#include "lib/stdio.h"
#include "lib/string.h"
#include <knum.h>   // k_parse_u32 -- kernel/lib/, linked into libuapp.a

// The name of `pid`, or "" if the process table has no such live entry.
// Walks by SLOT, which is not the pid -- proc_info carries the pid, and
// assuming slot+1 would be right only until a slot is reused.
static void name_of(int pid, char *out, unsigned long cap) {
    struct proc_info info;
    out[0] = '\0';
    for (int i = 0; sys_proc_info(i, &info); i++) {
        if (info.pid == pid) { strlcpy(out, info.name, cap); return; }
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        cmd_usage("kill <pid> [pid...]    (`ps` for pids)");
        return 1;
    }

    int failed = 0;
    for (int i = 1; i < argc; i++) {
        // k_parse_u32 REJECTS rather than guessing (CLAUDE.md), so
        // "12x" is refused instead of silently becoming 12 -- which
        // matters when the argument names something to destroy.
        uint32_t parsed = 0;
        if (!k_parse_u32(argv[i], &parsed) || parsed == 0) {
            sys_print("kill: not a pid: ");
            sys_print(argv[i]);
            sys_print("\n");
            failed = 1;
            continue;
        }
        int pid = (int)parsed;

        char name[PROC_NAME_MAX];
        name_of(pid, name, sizeof name);

        // SYS_KILL's failure value is 0 rather than a negative errno
        // (see abi/syscall_abi.h on why flipping those is its own
        // change), so there is no code to report -- say what it means.
        if (sys_kill(pid, -1) <= 0) {
            char line[96];
            snprintf(line, sizeof line, "kill: no process with pid %d\n", pid);
            sys_print(line);
            failed = 1;
            continue;
        }
        char line[128];
        snprintf(line, sizeof line, "ended pid %d%s%s\n",
                 pid, name[0] ? " -- " : "", name);
        sys_print(line);
    }
    return failed;
}
