// system() -- run a command line through the shell and wait for it.
//
// **A REAL IMPLEMENTATION, not a stub returning -1**, and what made that
// possible is `/bin/tosh -c`. Before that flag existed this could only
// have reported "no command processor", which POSIX does define an
// answer for and which would have been honest -- but toy-os HAS a shell,
// a spawn syscall and a wait syscall, so the honest answer was simply
// out of date.
//
// The differences from POSIX worth knowing, because they are real:
//
//   * **Signals are not touched.** POSIX says system() blocks SIGCHLD
//     and ignores SIGINT/SIGQUIT in the caller for the duration, so that
//     a Ctrl-C reaches the child and not the parent. Doing that here
//     would need sigprocmask(), which this libc does not have; the
//     consequence is that a caller with a SIGCHLD handler sees the
//     child's death, rather than having it hidden. That is a smaller
//     surprise than silently swallowing a signal, and it is written down
//     rather than discovered.
//   * **There is no shell command language beyond what tosh parses** --
//     pipelines and `&` yes, quoting and substitution no. A caller
//     passing shell metacharacters gets tosh's reading of them, which is
//     the same reading a person typing at a prompt gets.
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "rt/sys.h"

#define SHELL_FALLBACK "/bin/tosh"

// WHICH SHELL, asked of the registry rather than baked in
// (`system.shell`, kernel/lib/shell_config.c). Read on EVERY call
// rather than cached: system() is about to create a process, so one
// syscall is noise beside it, and a cache would make a changed setting
// take effect only in programs started afterwards -- which is the kind
// of difference nobody can explain later.
//
// The fallback is not defensive padding: a registry that cannot answer
// (an old kernel, a failed syscall) must still leave system() working,
// because a C library call that stops working when a setting is
// unreadable is worse than one that ignores the setting.
static const char *shell_path(char *buf, size_t cap) {
    struct setting_msg m;
    memset(&m, 0, sizeof m);
    m.op = SETTING_OP_GET;
    strlcpy(m.name, "system.shell", sizeof m.name);
    if (sys_setting(&m) != 0 || !m.value[0]) return SHELL_FALLBACK;
    strlcpy(buf, m.value, cap);
    return buf;
}

int system(const char *command) {
    // POSIX: a NULL command asks whether a command processor is
    // AVAILABLE -- non-zero if so. It is, as long as the shell is
    // installed, so this is a real check rather than a constant.
    char shbuf[SETTING_ABI_VALUE_MAX];
    const char *sh = shell_path(shbuf, sizeof shbuf);
    if (!command) return access(sh, F_OK) == 0;

    // "-c " + the command. Built here rather than passed as two
    // arguments because sys_spawn() takes ONE argument string, which it
    // splits the way the shell would.
    char args[512];
    const size_t prefix = 3;              // "-c "
    if (strlen(command) + prefix >= sizeof args) {
        // REFUSED rather than truncated: half a command line is a
        // different command, and running it would be worse than not
        // running one. -1 is system()'s own "could not run the shell".
        return -1;
    }
    memcpy(args, "-c ", prefix);
    strcpy(args + prefix, command);

    int pid = sys_spawn(sh, args, -1);
    if (pid < 0) return -1;

    int status = 0;
    if (sys_waitpid(pid, &status) < 0) return -1;

    // The status is already in the shape <sys/wait.h>'s macros expect
    // (see its own comment: an exit code, or SIGNAL_EXIT_BASE + signal),
    // so WEXITSTATUS(system(...)) means what a caller expects without a
    // re-encoding step here.
    return status;
}
