// waitpid(): the four rt/sys.h entry points behind one POSIX call.
//
// THE OPTIONS PICK THE ENTRY POINT, they are not passed through. Each
// combination already exists as its own function with its own blocking
// contract, and this is a TABLE of the four rather than a chain of ifs
// -- there are exactly four and the mapping is the documentation.
#include <sys/wait.h>
#include <errno.h>
#include "rt/sys.h"

int waitpid(int pid, int *status, int options) {
    int code = 0, r;

    switch (options & (WNOHANG | WUNTRACED)) {
    case 0:                    r = sys_waitpid(pid, &code); break;
    case WNOHANG:              r = sys_waitpid_nohang(pid, &code); break;
    case WUNTRACED:            r = sys_waitpid_untraced(pid, &code); break;
    default:                   r = sys_waitpid_check(pid, &code); break;
    }
    // dispatch-ok: four cases, bounded by the two flags POSIX defines
    // and this kernel implements. A fifth needs a fifth syscall.

    // "STILL RUNNING" IS 0, NOT AN ERROR. The non-blocking calls report
    // it as SYS_RETRY, which is a negative number and would otherwise
    // read as failure -- and POSIX's WNOHANG contract is specifically
    // that nothing to report is a return of 0 with errno untouched.
    if (r == SYS_RETRY) return 0;
    if (r < 0) return -1;

    if (status) *status = code;
    return r;
}
