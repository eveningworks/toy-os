#ifndef ABI_ERRNO_H
#define ABI_ERRNO_H

// Error numbers -- what a failed syscall returns instead of a bare -1.
//
// THE ENCODING: a syscall that fails returns the NEGATED error number.
// open() on a missing file returns -ENOENT (-2); open() with a full
// descriptor table returns -EMFILE (-24). Success is 0, a positive
// count, an fd, or a pointer, exactly as before.
//
// This is Linux's kernel-side convention, and it is here for Linux's
// reason: the alternative is a global `errno`, which needs thread-local
// storage the moment a process can have two threads in a syscall at
// once. toy-os has no TLS (see docs/roadmap.md), and a returned code
// needs none. The -1-plus-errno shape POSIX programs expect is put back
// by libsys in ring 3 (userland/rt/sys.c's sys_errno()), which is where
// a C library does it too -- the kernel does not owe anyone a global.
//
// THE VALUES ARE LINUX'S, deliberately, so that anything ported here
// reads the way it does everywhere else and nobody has to learn a
// second table. What is NOT copied is the SIZE of that table: Linux
// defines ~130 of these. The list below is only what a handler in this
// kernel actually distinguishes today -- the same bar kernel/lib/ holds
// for a new helper, a second real caller rather than a plausible one.
// Add one when a handler genuinely tells that case apart, not because
// POSIX has a name for it.
//
// THE RANGE, and the one thing to check before adding anything here.
// A return in [-ERRNO_MAX, -1] means failure; everything else is a
// successful result. That test has to stay unambiguous against every
// syscall's legitimate return range, which is why the bound is small
// and the codes are dense at the bottom: SYS_SBRK returns a POINTER,
// and a ring-3 heap address is far above this window (see
// kernel/include/kernel/uaddr.h). SYS_RETRY sits just past the top of
// the range on purpose -- see abi/syscall_abi.h, which owns that
// constant and explains why it is not simply -EAGAIN.
#define ERRNO_MAX 4094

#define EPERM   1  // the caller is not allowed to ask this -- a compositor-only
                   // request from a client, or a call from the kernel context
                   // that only a scheduled process can make
#define ENOENT  2  // no such file
#define ESRCH   3  // no such process
#define EIO     5  // the device or filesystem refused the transfer
#define EBADF   9  // not an open descriptor, or open the wrong way (reading a
                   // write-only file, writing a pipe's read end) -- POSIX folds
                   // both into EBADF and so does this
#define ECHILD 10  // waitpid() with no children at all, which is PERMANENT and
                   // is why it must not read as "none have exited yet"
#define ENOMEM 12  // out of memory, or a heap request that would run past its
                   // ceiling
#define EFAULT 14  // the caller handed the kernel a pointer it may not have
#define EEXIST 17  // already exists
#define ENODEV 19  // nothing is registered to serve this -- no window server,
                   // no such hardware
#define EINVAL 22  // the arguments are wrong: a bad size, a count over a
                   // maximum, a reserved field that is not zero
#define ENFILE 23  // a SYSTEM-wide table is full (the pipe table)
#define EMFILE 24  // THIS PROCESS's descriptor table is full -- distinct from
                   // ENFILE, and distinct from ENOENT, which is the whole
                   // reason this header exists
#define ENOSYS 38  // the call exists and does nothing yet

#endif // ABI_ERRNO_H
