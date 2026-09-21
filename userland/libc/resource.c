// Resource limits: there are none, and setrlimit() says so rather than
// accepting a number nothing enforces. See <sys/resource.h>.
#include <sys/resource.h>
#include <errno.h>
#include "rt/sys.h"

int getrlimit(int resource, struct rlimit *rlp) {
    if (resource < 0 || resource >= RLIMIT_NLIMITS || !rlp) {
        errno = EINVAL;
        return -1;
    }
    rlp->rlim_cur = RLIM_INFINITY;
    rlp->rlim_max = RLIM_INFINITY;
    return 0;
}

int setrlimit(int resource, const struct rlimit *rlp) {
    if (resource < 0 || resource >= RLIMIT_NLIMITS || !rlp) {
        errno = EINVAL;
        return -1;
    }
    // Setting a limit to the value it already has is not a change --
    // the same rule the settings registry follows, and it is what makes
    // `ulimit -f unlimited` succeed instead of failing pointlessly.
    if (rlp->rlim_cur == RLIM_INFINITY && rlp->rlim_max == RLIM_INFINITY)
        return 0;
    errno = EPERM;
    return -1;
}

int setpriority(int which, int who, int value) {
    return sys_setpriority(which, who, value);
}

int getpriority(int which, int who) {
    return sys_getpriority(which, who);
}
