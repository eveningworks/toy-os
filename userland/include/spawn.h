// <spawn.h> -- posix_spawn(), over SYS_SPAWN, which was shaped for it.
//
// This is the call every program here should reach for before fork():
// one syscall, no address-space copy, and the child's streams and
// group named up front (docs/fork-design.md). The two option structs
// are accepted as NULL only: toy-os's spawn names fd 0/1 and the
// process group in the message itself (see sys_spawn_opts() in
// "rt/sys.h" for those), and a non-NULL file_actions/attr here is
// refused with EINVAL rather than half-honoured.
#ifndef _SPAWN_H
#define _SPAWN_H

#include <sys/types.h>

typedef struct { int _unused; } posix_spawn_file_actions_t;
typedef struct { int _unused; } posix_spawnattr_t;

// Returns 0 with the child's pid in *pid, or an errno VALUE (POSIX
// returns the error rather than setting errno here).
int posix_spawn(pid_t *pid, const char *path,
                const posix_spawn_file_actions_t *file_actions,
                const posix_spawnattr_t *attrp,
                char *const argv[], char *const envp[]);
// The same with a PATH walk for a bare name, as execvp() does.
int posix_spawnp(pid_t *pid, const char *file,
                 const posix_spawn_file_actions_t *file_actions,
                 const posix_spawnattr_t *attrp,
                 char *const argv[], char *const envp[]);

#endif
