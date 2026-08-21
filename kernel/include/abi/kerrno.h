#ifndef KABI_KERRNO_H
#define KABI_KERRNO_H

// A NAME FOR abi/errno.h THAT THE C LIBRARY DOES NOT ALSO USE.
//
// The same problem api/kstring.h exists for, one directory over:
// userland/include/errno.h is the C library's <errno.h> and comes first
// on the ring-3 include path, so neither <errno.h> nor "errno.h" can
// reach THIS header from there -- both resolve back to the libc's,
// where the include guard makes the reference a silent no-op and every
// E* constant vanishes.
//
// **The pattern, stated once because there are now two of them**: when
// the C library takes a name a kernel header already uses, the KERNEL
// header gets a k-prefixed alias and the C library keeps the plain
// name. The alias is a forwarding header with no content of its own,
// and the quoted include below is load-bearing -- a quoted search
// starts in this file's own directory, so it reaches abi/errno.h and
// cannot reach the libc's.
//
// Only two names collide today (string.h and errno.h). Check this list
// before adding a header to userland/include/: api/ and abi/ between
// them already own fs.h, heap.h, timer.h, pipe.h and query.h, none of
// which the C library wants.
#include "errno.h"

#endif
