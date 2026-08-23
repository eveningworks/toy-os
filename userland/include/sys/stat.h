#ifndef ULIB_SYS_STAT_H
#define ULIB_SYS_STAT_H

// Directory creation, and the permission bits that come with the
// signature.
//
// **THERE IS NO stat() HERE**, which is the surprising half of a header
// called <sys/stat.h>. `struct stat` is the union of everything a
// filesystem might report about a file -- owner, group, three sets of
// permission bits, three timestamps, a device and an inode number -- and
// TFS3 has almost none of it (docs/filesystem-layout.md). A struct whose
// fields were mostly invented zeroes would let ported code compile and
// then take wrong branches on `st_mode`, which is worse than not
// compiling. `access()` and <dirent.h>'s `d_type` answer the questions
// this OS can actually answer. When TFS3 grows the fields, this is where
// stat() goes.
//
// So this header exists for `mkdir()`, and for the `#include
// <sys/stat.h>` at the top of ported code that wants it.

#include <sys/types.h>

// **THE MODE IS ACCEPTED AND IGNORED, and that is not the same as being
// dishonoured.** There are no permission bits on this filesystem at all,
// so 0755 does not fail to be applied -- there is nothing for it to
// mean. That is why this does not follow CLAUDE.md's usual rule of
// REFUSING what it cannot honour (a non-empty `sa_mask` is `EINVAL`,
// <termios.h>'s VMIN/VTIME are undefined on purpose): those refuse
// because obeying halfway would change behaviour a caller depends on.
// Here the caller depends on a directory existing, and it does.
//
// Returns 0, or -1 with errno set -- EEXIST if the name is taken,
// ENOENT if a parent directory is missing. Creates ONE level, as POSIX
// says: `mkdir("/a/b/c")` fails with ENOENT if `/a/b` does not exist.
int mkdir(const char *path, mode_t mode);

// The mode bits themselves, so that a caller writing 0755 in symbols
// compiles. Standard octal values; nothing here reads them.
#define S_IRWXU 0700
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRWXG 0070
#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IXGRP 0010
#define S_IRWXO 0007
#define S_IROTH 0004
#define S_IWOTH 0002
#define S_IXOTH 0001

#endif
