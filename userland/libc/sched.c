// <sched.h>: the scheduling class, over SYS_SCHED_SETSCHEDULER and
// SYS_SCHED_GETSCHEDULER.
#include <sched.h>
#include <errno.h>
#include "rt/sys.h"

int sched_setscheduler(pid_t pid, int policy, const struct sched_param *param) {
    if (!param) {
        errno = EINVAL;
        return -1;
    }
    return sys_sched_setscheduler(pid, policy, param->sched_priority);
}

int sched_getscheduler(pid_t pid) {
    int r = sys_sched_getscheduler(pid);
    return r < 0 ? -1 : (r & 0xff);
}

int sched_getparam(pid_t pid, struct sched_param *param) {
    if (!param) {
        errno = EINVAL;
        return -1;
    }
    int r = sys_sched_getscheduler(pid);
    if (r < 0) return -1;
    param->sched_priority = r >> 8;
    return 0;
}

int sched_get_priority_min(int policy) {
    if (policy == SCHED_OTHER) return 0;
    if (policy == SCHED_FIFO || policy == SCHED_RR) return SCHED_RT_PRIO_MIN;
    errno = EINVAL;
    return -1;
}

int sched_get_priority_max(int policy) {
    if (policy == SCHED_OTHER) return 0;
    if (policy == SCHED_FIFO || policy == SCHED_RR) return SCHED_RT_PRIO_MAX;
    errno = EINVAL;
    return -1;
}

int sched_yield(void) {
    sys_yield();
    return 0;
}
