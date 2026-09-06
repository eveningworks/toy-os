#ifndef ULIB_UTMPPATH_H
#define ULIB_UTMPPATH_H

#include "tmppath.h"

// A SCRATCH PATH IN ONE EXPRESSION, for ring 3.
//
// api/tmppath.h's tmppath() writes into a caller's buffer, which is the
// right shape for the kernel and an awkward one for a program that just
// wants to pass a path to fopen(). This is that, with the buffer owned
// here.
//
// `name` may carry sub-components -- "probe.txt/child" -- which is what
// the call sites that used to write PATH "/child" need: a path built at
// runtime cannot be concatenated with a string literal.
//
// FOUR ROTATING BUFFERS, so two paths can be live in one expression.
// That is a bound, not a guarantee: five in one expression would reuse
// the first. Nothing here comes close, and a caller that would is
// better off with tmppath() and its own buffer.
//
// RING 3 ONLY, and the statics are why -- a program is single-threaded
// unless it asks not to be, where kernel-side module scratch is exactly
// the shape vfs.c holds a preemption guard for.
const char *utest_path(enum tmp_kind kind, const char *name);

#endif
