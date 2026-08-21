#ifndef ULIB_ASSERT_H
#define ULIB_ASSERT_H

// C's <assert.h>.
//
// A failed assertion prints file, line, function and the expression to
// STDERR -- which is unbuffered (<stdio.h>), so the message is out
// before abort() runs and cannot be lost with the process. That is the
// whole reason stderr is unbuffered and it is why this header is one
// line of code and twenty of comment.
//
// abort() exits with 134 and does NOT flush the buffered streams: an
// assertion has just said the program's state is wrong, and committing
// a half-written file on the way out is worse than losing it.
//
// NDEBUG compiles assert() to nothing, as C requires. Nothing in this
// tree defines it -- the kernel's own KTESTs are a different mechanism
// entirely (kernel/include/kernel/ktest.h) and this is for ring 3.
//
// **This is NOT the kernel's panic.** An assertion here ends one
// process; a kernel panic stops the machine. A ring-3 program has no
// way to do the second and should not look like it does.

#ifdef NDEBUG
#define assert(expr) ((void)0)
#else
void __assert_fail(const char *expr, const char *file, int line, const char *fn)
    __attribute__((noreturn));
#define assert(expr) \
    ((expr) ? (void)0 : __assert_fail(#expr, __FILE__, __LINE__, __func__))
#endif

#endif
