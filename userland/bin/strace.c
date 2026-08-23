// /bin/strace -- run a program with syscall tracing on.
//
// **THE WHOLE PROGRAM IS A SPAWN AND A WAIT**, and that is the point of
// where the work went. Tracing is the kernel's -- every ring-3 syscall
// funnels through one dispatcher (kernel/proc/syscall.c), so three
// hooks in that one function cover all of them and a tracer has nothing
// to instrument. What a tracer has to do is name the process to trace,
// and this asks for that AT THE SPAWN, through SYS_SPAWN's SPAWN_TRACE
// flag.
//
// IT WAS A SHELL BUILTIN, IN RING 0. That was fine while the only shell
// was the kernel's own, and it stopped being fine for the reasons every
// other builtin moved out: it could only be typed at the physical
// console, its output went to the physical screen whoever ran it was
// looking at or not, and it was ring-0 code maintained forever for a
// job a program does perfectly well. `apps/` holds no applications; see
// CLAUDE.md's rule about what a builtin has to be.
//
// **WHAT THIS DELIBERATELY DOES NOT DO: print the trace.** The lines
// come out of the kernel, into the terminal this process's fd 1 names,
// resolved once when the child is created (kernel/proc/strace.c) -- fd 1
// and not fd 2, because fd 2 in this OS is the kernel log rather than a
// second terminal stream (userland/lib/cmd.h says so at length). A
// tracer that relayed them would need the kernel to hand it every line
// through a pipe, which is a channel to build when something needs the
// text rather than the sight of it -- `dmesg` already answers that,
// since every line is klogged too. So there is nothing here between the
// spawn and the wait, and that is not a stub: it is the design.
//
// The summary line is the kernel's for the same reason -- only the
// kernel can count the calls, and asking for the number back would be a
// syscall for one integer.
#include "rt/sys.h"
#include "lib/upath.h"
#include "lib/cmd.h"   // cmd_usage()/cmd_fail() -- the words every /bin command shares
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        // The literal here is what docs/commands/strace.md must carry
        // VERBATIM -- tools/check_docs.py reads it straight out of this
        // call, so a flag added to one and not the other fails the build.
        cmd_usage("strace <program> [args...]");
        printf("Runs <program> with syscall tracing on: one decoded line per\n");
        printf("syscall, plus a count when it exits. The trace goes to this\n");
        printf("terminal and to `dmesg`.\n");
        return 2;
    }

    char path[UPATH_MAX];
    int found = upath_find_program(argv[1], path, sizeof path);
    if (found < 0) {
        // NOT "no such executable". The search could not be COMPLETED --
        // a full descriptor table, say -- and reporting that as "not
        // found" is how a machine tells you a program is missing when it
        // is sitting right there. See upath.h.
        cmd_fail("strace", argv[1]);
        return 1;
    }
    if (!found) {
        printf("strace: no such executable: %s\n", argv[1]);
        return 1;
    }

    // The program's OWN arguments, rejoined into the single
    // whitespace-separated string SYS_SPAWN takes. argv[0] is this
    // program and argv[1] is the one being traced, so the child's
    // arguments start at 2.
    //
    // Rejoining loses nothing today because the shell that produced
    // this argv split on spaces in the first place -- there is no
    // quoting anywhere in this tree yet (docs/roadmap.md). When there
    // is, this is one of the places that has to stop flattening.
    char args[256];
    size_t n = 0;
    for (int i = 2; i < argc; i++) {
        size_t len = strlen(argv[i]);
        if (n + len + 2 > sizeof args) break; // REFUSE to half-pass an argument
        if (n) args[n++] = ' ';
        memcpy(args + n, argv[i], len);
        n += len;
    }
    args[n] = '\0';

    int pid = sys_spawn_flags(path, n ? args : 0, -1, environ, 0, SPAWN_TRACE);
    if (pid <= 0) {
        cmd_fail("strace", path);
        return 1;
    }

    // The child is in THIS process's group and this process is not the
    // shell's foreground job holder, so a Ctrl-C at the terminal reaches
    // both of us together -- which is what you want from a tracer: the
    // trace ends when the traced program does.
    int code = 0;
    sys_waitpid(pid, &code);
    return code;
}
