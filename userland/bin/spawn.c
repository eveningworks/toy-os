// spawn -- start a program and DO NOT wait for it.
//
// The difference from typing the name, which runs it and waits: this
// returns immediately and the child keeps running. The child is
// reparented to init when this exits, so it survives -- which is what
// `nohup`/`setsid` are for on a real system, and what starting a
// desktop or a long-running service needs.
//
// IT MATTERS FOR MORE THAN CONVENIENCE. The legacy `run` loader has no
// scheduler slot, so SYS_SLEEP returns -1 there and anything that
// blocks or paces itself misbehaves under it (see /bin/less's own note,
// and SYS_SLEEP's ABI comment). A spawned program is a REAL scheduled
// process, so `spawn /bin/less f` idles correctly where `less f` spins.
//
// stdout is INHERITED (-1), not redirected: a spawned program writes
// where its parent was writing, which is what you want when you are
// starting something to watch. A caller wanting a pipe passes a real fd
// through SYS_SPAWN directly; that is not this program's job.
//
// stderr is the KERNEL LOG (SPAWN_FD_KMSG), as `nohup` moves a detached
// job's output off the terminal: the prompt is back before the child
// has said anything, so its diagnostics belong where they can be read
// later -- `dmesg` -- and where the harness reads a background test's.
#include "rt/sys.h"
#include "lib/cmd.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        cmd_usage("spawn <path> [args...]    (typing the name instead waits for it)");
        return 1;
    }

    // Everything after the path is rejoined into one argument string,
    // because SYS_SPAWN takes it that way -- the shell already split it
    // and this puts it back. Single spaces, since the original spacing
    // is not recoverable from argv and nothing here depends on it.
    char args[192];
    args[0] = '\0';
    for (int i = 2; i < argc; i++) {
        if (args[0]) strlcat(args, " ", sizeof args);
        if (strlcat(args, argv[i], sizeof args) >= sizeof args) {
            sys_print("spawn: arguments too long\n");
            return 1;
        }
    }

    struct sys_spawn_opts o;
    sys_spawn_opts_init(&o);
    o.args = args[0] ? args : 0;
    o.env = environ;
    o.stderr_fd = SPAWN_FD_KMSG;
    int pid = sys_spawn_opts(argv[1], &o);
    if (pid <= 0) {
        cmd_fail("spawn", argv[1]);
        return 1;
    }
    // "started as pid N" is the kernel builtin's exact wording, kept
    // deliberately: tools/compositor_death_test.py asserts on it as its
    // "the desktop can be restarted" marker, and changing a phrase for
    // cosmetics would break a check that is about something else. The
    // path is appended for symmetry with /bin/kill's "ended pid N -- x".
    char line[160];
    snprintf(line, sizeof line, "started as pid %d -- %s\n", pid, argv[1]);
    sys_print(line);
    return 0;
}
