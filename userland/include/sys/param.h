#ifndef ULIB_SYS_PARAM_H
#define ULIB_SYS_PARAM_H

// BSD's grab-bag of system constants. Almost nothing here is standard --
// POSIX specifies none of it -- but ported code includes it constantly
// for MAXPATHLEN, and dash includes it from five files.
//
// **MAXPATHLEN IS FS_PATH_MAX, NOT A SECOND OPINION.** A header that
// invented its own number would let a caller size a buffer the kernel
// then refuses to fill, which is the failure this project keeps
// deleting. See kernel/include/abi/fs_abi.h.

#include <limits.h>

#ifndef MAXPATHLEN
#define MAXPATHLEN PATH_MAX
#endif

// BSD spells these in caps and expects them to work on any arithmetic
// type. Parenthesised to survive being used in an expression, and
// double-evaluating is BSD's behaviour too -- a caller writing
// MAX(i++, j) has the same bug on every system that defines this.
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

// Round `x` up to the next multiple of `n`, which must be a power of 2.
#define roundup(x, n) ((((x) + ((n) - 1)) / (n)) * (n))
#define powerof2(x)   ((((x) - 1) & (x)) == 0)

#endif
