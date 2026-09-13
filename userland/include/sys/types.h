#ifndef ULIB_SYS_TYPES_H
#define ULIB_SYS_TYPES_H

#include <stdint.h>

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

#ifndef __ULIB_TIME_T
#define __ULIB_TIME_T
// Also in <time.h>, guarded so both headers can define it -- POSIX
// requires it from this one too, and ported code includes whichever it
// happens to reach first (dash's mail.c takes this route).
typedef int64_t time_t;
#endif

#ifndef __ULIB_SUSECONDS_T
#define __ULIB_SUSECONDS_T
// SIGNED microseconds, which is what the `s` is for: a timeval
// difference can be negative.
typedef long suseconds_t;
#endif

#ifndef __ULIB_UID_T
// PRESENT AND ALWAYS ZERO, for the same reason mode_t below is present
// and meaningless: this is a single-user system with no login, no
// /etc/passwd and no notion of a second user. getuid()/geteuid() answer
// 0 (<unistd.h> says why that is root rather than nobody), and a
// signature carrying one compiles.
#define __ULIB_UID_T
typedef unsigned uid_t;
typedef unsigned gid_t;
#endif

#ifndef __ULIB_MODE_T
#define __ULIB_MODE_T
// PRESENT AND MEANINGLESS: there are no permission bits on this
// filesystem (docs/filesystem-layout.md). It is here so that a
// signature carrying one compiles; nothing reads the value.
typedef unsigned mode_t;
#endif

#endif
