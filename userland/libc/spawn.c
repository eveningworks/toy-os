// posix_spawn() and posix_spawnp() -- see <spawn.h> for what they refuse.
#include <errno.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "rt/sys.h"

static int spawn_one(pid_t *pid, const char *path, char *const argv[], char *const envp[]) {
    struct sys_spawn_opts o;
    sys_spawn_opts_init(&o);
    o.argv = argv;
    o.env = envp ? (char **)envp : environ;
    int r = sys_spawn_opts(path, &o);
    if (r < 0) return errno;
    if (pid) *pid = r;
    return 0;
}

int posix_spawn(pid_t *pid, const char *path,
                const posix_spawn_file_actions_t *file_actions,
                const posix_spawnattr_t *attrp,
                char *const argv[], char *const envp[]) {
    if (file_actions || attrp) return EINVAL;
    return spawn_one(pid, path, argv, envp);
}

int posix_spawnp(pid_t *pid, const char *file,
                 const posix_spawn_file_actions_t *file_actions,
                 const posix_spawnattr_t *attrp,
                 char *const argv[], char *const envp[]) {
    if (file_actions || attrp) return EINVAL;
    if (strchr(file, '/')) return spawn_one(pid, file, argv, envp);
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/bin";
    int last = ENOENT;
    for (const char *p = path; ; ) {
        const char *end = strchr(p, ':');
        size_t dlen = end ? (size_t)(end - p) : strlen(p);
        char full[128];
        if (dlen + 1 + strlen(file) + 1 <= sizeof full) {
            memcpy(full, p, dlen);
            full[dlen] = '/';
            strcpy(full + dlen + 1, file);
            int r = spawn_one(pid, full, argv, envp);
            if (r == 0) return 0;
            if (r != ENOENT) return r;
            last = r;
        }
        if (!end) break;
        p = end + 1;
    }
    return last;
}
