#ifndef ULIB_SYS_TYPES_H
#define ULIB_SYS_TYPES_H

// POSIX's type names, in the one place every other header can take them
// from. It exists mostly so that `#include <sys/types.h>` at the top of
// ported code is not an error -- the definitions themselves are one
// line each.
//
// They are DEFINED HERE and guarded individually, because <unistd.h>
// declared ssize_t and off_t before this header existed and code
// including only that must keep working. The guards are the same
// spelling in both files, so whichever arrives first wins and the
// second is a no-op.
#include <stddef.h>

#ifndef __ULIB_SSIZE_T
#define __ULIB_SSIZE_T
typedef long ssize_t;
#endif

#ifndef __ULIB_OFF_T
#define __ULIB_OFF_T
typedef long off_t;
#endif

#ifndef __ULIB_PID_T
#define __ULIB_PID_T
// A PLAIN INT, matching every syscall that takes one. Not a distinct
// type: the kernel's spawn/wait/kill all speak `int`, and a typedef
// that disagreed with the ABI would be decoration.
typedef int pid_t;
#endif

#ifndef __ULIB_MODE_T
#define __ULIB_MODE_T
// PRESENT AND MEANINGLESS: there are no permission bits on this
// filesystem (docs/filesystem-layout.md). It is here so that a
// signature carrying one compiles; nothing reads the value.
typedef unsigned mode_t;
#endif

#endif
