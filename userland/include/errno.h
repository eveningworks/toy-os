#ifndef ULIB_ERRNO_H
#define ULIB_ERRNO_H

// C's <errno.h>: the codes, plus `errno` itself as an lvalue.
//
// The CODES are the kernel's -- abi/errno.h, reached as <kerrno.h>
// because this file has taken the plain name (see that header for the
// naming pattern). One list, so a number means the same thing on both
// sides of the syscall boundary.
//
// `errno` is set by every libsys wrapper whose failure value is -1, and
// READ ONLY AFTER A CALL HAS REPORTED FAILURE: it is not cleared on
// success, which is POSIX's rule, so a stale value from an earlier
// failure is still here after a hundred successful calls.
//
// rt/sys.h's sys_errno() is the same storage under the older name and
// keeps working; this is the C spelling of it, not a second copy.
#include <kerrno.h>

int *__errno_location(void);

#define errno (*__errno_location())

#endif
