// The exec family over SYS_EXEC. execve() is the syscall; execv() adds
// `environ`, as execv does everywhere; execvp() walks PATH for a name
// with no slash, as sh does. None returns on success.
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "rt/sys.h"

int execve(const char *path, char *const argv[], char *const envp[]) {
    return sys_execve(path, argv, envp);
}

int execv(const char *path, char *const argv[]) {
    return sys_execve(path, argv, environ);
}

int execvp(const char *file, char *const argv[]) {
    if (strchr(file, '/')) return execv(file, argv);
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/bin";
    int saw_noent = 0;
    for (const char *p = path; ; ) {
        const char *end = strchr(p, ':');
        size_t dlen = end ? (size_t)(end - p) : strlen(p);
        char full[128];
        if (dlen + 1 + strlen(file) + 1 <= sizeof full) {
            memcpy(full, p, dlen);
            full[dlen] = '/';
            strcpy(full + dlen + 1, file);
            execv(full, argv);          // returns only on failure
            if (errno == ENOENT) saw_noent = 1;
            else return -1;             // a real refusal: stop looking
        }
        if (!end) break;
        p = end + 1;
    }
    errno = saw_noent ? ENOENT : errno;
    return -1;
}

pid_t getppid(void) {
    int me = getpid();
    struct proc_info pi;
    for (int i = 0; sys_proc_info(i, &pi) == 0; i++)
        if (pi.pid == me) return pi.ppid;
    return 0;
}
