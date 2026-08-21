#ifndef ULIB_DIRENT_H
#define ULIB_DIRENT_H

// POSIX's <dirent.h> over SYS_LISTDIR.
//
// **`struct dirent` here is POSIX's, with `d_name`.** The syscall ABI's
// own entry was called `struct dirent` until this header needed the
// name and is `struct sys_dirent` now (abi/syscall_abi.h) -- two structs
// cannot share a tag, and every other ABI struct is `sys_*` anyway.
//
// **THE WHOLE DIRECTORY IS READ AT opendir()**, not streamed. SYS_LISTDIR
// takes a path and fills an array; there is no directory handle in the
// kernel and no cursor to advance, so a DIR is a snapshot plus an index.
// Two consequences a caller can see, and neither is hidden:
//
//   - A directory larger than SYS_LISTDIR_MAX entries is TRUNCATED, and
//     readdir() simply ends early. That is the syscall's limit, not this
//     header's, and /bin/ls has always had it.
//   - Changes made after opendir() are invisible until the next one.
//     POSIX says a directory changed during iteration is unspecified,
//     so this is a legal reading of the standard rather than a gap in
//     it -- but it is a stronger guarantee than a real readdir gives,
//     and code that relies on seeing its own mkdir will be surprised
//     the other way round.
//
// The buffer is malloc'd at opendir() and freed at closedir(), so a DIR
// costs one allocation of SYS_LISTDIR_MAX entries -- which is why a
// program should closedir() rather than leaking it.
#include <stddef.h>
#include "syscall_abi.h"

struct dirent {
    // No d_ino: SYS_LISTDIR does not report one. SYS_STAT does, and a
    // caller that needs it has the name to ask with. A d_ino field that
    // was always zero would invite somebody to believe it.
    unsigned char d_type;      // DT_* below
    char d_name[64];           // FS_PATH_MAX -- the last component only
};

// The two that can occur. POSIX's full set (sockets, devices, symlinks)
// describes things this filesystem does not have; DT_UNKNOWN is here
// because portable code tests for it.
#define DT_UNKNOWN 0
#define DT_DIR     4
#define DT_REG     8

typedef struct _DIR DIR;

// NULL if the path does not exist, is not a directory, or the snapshot
// could not be allocated.
DIR           *opendir(const char *path);
// The next entry, or NULL at the end. The returned pointer is into the
// DIR and is invalid after closedir() -- as POSIX specifies, and unlike
// most things in this tree, which return caller-owned memory.
struct dirent *readdir(DIR *d);
void           rewinddir(DIR *d);
int            closedir(DIR *d);

#endif
