#ifndef ULIB_SYS_RESOURCE_H
#define ULIB_SYS_RESOURCE_H

// Per-process resource limits. **THIS SYSTEM IMPOSES NONE**, so every
// limit reads back as RLIM_INFINITY and setrlimit() refuses to lower
// one rather than accepting a number it will not enforce.
//
// That refusal is the interesting half. A setrlimit() that returned 0
// and did nothing would let a shell's `ulimit -f 100` print success and
// then write a 4 GB file -- the caller is told the limit is in force
// and has no way to discover otherwise. Answering EPERM is the same
// rule <termios.h> follows for VMIN/VTIME and <locale.h> for a locale
// that is not "C": refuse what cannot be honoured.

#include <sys/types.h>
#include <stdint.h>

typedef uint64_t rlim_t;

#define RLIM_INFINITY  ((rlim_t)-1)
#define RLIM_SAVED_MAX RLIM_INFINITY
#define RLIM_SAVED_CUR RLIM_INFINITY

struct rlimit {
    rlim_t rlim_cur;  // the soft limit
    rlim_t rlim_max;  // the ceiling the soft limit may be raised to
};

// The resources POSIX names. All of them read RLIM_INFINITY; they exist
// so that code naming one compiles, and so a future limit has a number
// already allocated rather than shifting everything below it.
#define RLIMIT_CPU     0
#define RLIMIT_FSIZE   1
#define RLIMIT_DATA    2
#define RLIMIT_STACK   3
#define RLIMIT_CORE    4
#define RLIMIT_NOFILE  5
#define RLIMIT_AS      6
#define RLIMIT_NPROC   7
#define RLIMIT_MEMLOCK 8
#define RLIMIT_NLIMITS 9

// Fills *rlp with RLIM_INFINITY for both fields. Returns 0, or -1 with
// EINVAL for a resource number this system does not name.
int getrlimit(int resource, struct rlimit *rlp);

// Returns 0 if the request leaves both fields RLIM_INFINITY -- setting
// a limit to the value it already has is not a change, the same rule
// the settings registry follows. Otherwise -1 with EPERM: this system
// cannot enforce it and will not say it has.
int setrlimit(int resource, const struct rlimit *rlp);

#endif
