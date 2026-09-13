// times() and sysconf() -- see <sys/times.h> for which fields are real.
#include <sys/times.h>
#include <unistd.h>
#include <time.h>
#include "rt/sys.h"  // sys_monotonic_ns -- there is no clock_gettime here
#include <errno.h>

long sysconf(int name) {
    switch (name) {
    // The same number as CLOCKS_PER_SEC, deliberately -- <unistd.h>
    // says why.
    case _SC_CLK_TCK:  return 1000000L;
    case _SC_PAGESIZE: return 4096L;
    case _SC_NPROCESSORS_ONLN: return 1L;   // no SMP yet (docs/smp-design.md)
    // _SC_OPEN_MAX is deliberately NOT answered. The descriptor table
    // has a size, but nothing in the ABI states it, and a number
    // invented here would be exactly the fabricated limit
    // <sys/resource.h> refuses to report. EINVAL is POSIX's "no limit
    // available for that name".
    default:
        errno = EINVAL;
        return -1;
    }
}

clock_t times(struct tms *buf) {
    // clock() already answers "CPU time for THIS process" out of the
    // kernel's cpu_ns, in the same unit this returns. Going through it
    // rather than repeating its proc_info scan keeps one definition of
    // where that number comes from.
    // **A PROCESS WITH NO CPU ACCOUNTING IS NOT AN ERROR HERE.** clock()
    // answers -1 for a caller with no scheduler slot (the legacy `run`
    // loader), but this call's RETURN value is elapsed real time, which
    // does not depend on that. Failing the whole call because one field
    // is unavailable would make times() useless in exactly the context
    // where a shell's `times` is most likely to be run.
    clock_t cpu = clock();
    if (cpu == (clock_t)-1) cpu = 0;
    if (buf) {
        buf->tms_utime  = cpu;  // the whole of it -- see <sys/times.h>
        buf->tms_stime  = 0;    // not split out by this kernel
        buf->tms_cutime = 0;    // a reaped child's time is not accumulated
        buf->tms_cstime = 0;
    }
    // ELAPSED REAL TIME, not CPU time, and the two are different
    // questions -- a caller timing a pipeline wants this one. Monotonic
    // so that a clock change cannot make a duration negative.
    return (clock_t)(sys_monotonic_ns() / 1000ull);
}
