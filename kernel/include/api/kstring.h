#ifndef KAPI_KSTRING_H
#define KAPI_KSTRING_H

// A NAME FOR api/string.h THAT THE C LIBRARY DOES NOT ALSO USE.
//
// This file has no content of its own and exists for exactly one
// reason: userland/include/string.h is the C library's <string.h>, and
// it is built with -Iuserland/include ahead of -Ikernel/include/api --
// which it must be, or an app's <string.h> would find the kernel
// toolkit's header instead of the libc's. That leaves the libc header
// with no way to spell "the OTHER string.h": <string.h> and
// "string.h" both resolve back to itself, where the include guard turns
// the reference into a silent no-op and every k_* prototype vanishes.
//
// So the toolkit gets a second name. Include THIS from ring-3 code that
// wants the k_* functions directly rather than the C names.
//
// The quoted include below is load-bearing: a quoted search starts in
// the directory of the file doing the including, which is this one, so
// it reaches api/string.h and cannot reach the libc's.
#include "string.h"

#endif
