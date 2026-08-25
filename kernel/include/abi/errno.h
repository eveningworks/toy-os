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
#define EINTR   4  // a SIGNAL arrived while this call was parked, so it gave
                   // up instead of finishing. NOT a failure of the thing
                   // asked for: the request may be repeated and may then
                   // succeed. Deliberately distinct from SYS_RETRY, which
                   // means "woken, nothing happened, ask again" and IS
                   // retried inside libsys -- a caller that saw a signal's
                   // interruption as a spurious wakeup would loop straight
                   // back into the call the signal was trying to end.
                   // **OBSERVABLE SINCE HANDLERS LANDED**, and it was
                   // not before -- every signal that could interrupt a
                   // call also terminated the process, so this existed
                   // for two stages as the honest answer at the ABI with
                   // no reader. A caught signal without SA_RESTART is
                   // what a program sees it through; with the flag the
                   // call is restarted instead and the caller never
                   // learns it was interrupted at all.
#define EIO     5  // the device or filesystem refused the transfer
#define EBADF   9  // not an open descriptor, or open the wrong way (reading a
                   // write-only file, writing a pipe's read end) -- POSIX folds
                   // both into EBADF and so does this
#define ECHILD 10  // waitpid() with no children at all, which is PERMANENT and
                   // is why it must not read as "none have exited yet"
#define ENOMEM 12  // out of memory, or a heap request that would run past its
                   // ceiling
#define EFAULT 14  // the caller handed the kernel a pointer it may not have
#define EBUSY  16  // the thing exists and is IN USE, so the operation is
                   // refused rather than done anyway -- umount of a
                   // filesystem holding an open file, or a mount point
                   // something is already mounted at. Distinct from
                   // EPERM (never allowed) and EINVAL (the request is
                   // wrong): the same call would succeed later.

#define EEXIST 17  // already exists
#define ENODEV 19  // nothing is registered to serve this -- no window server,
                   // no such hardware
#define EINVAL 22  // the arguments are wrong: a bad size, a count over a
                   // maximum, a reserved field that is not zero
#define E2BIG   7  // the argument or ENVIRONMENT list is too long. Its own
                   // code because "your environment does not fit" is a
                   // thing a caller can act on, unlike EINVAL
#define ESPIPE 29  // this stream has no POSITION to move: lseek() on a
                   // console, a pipe or a socket. POSIX's name for it
                   // mentions a pipe only because a pipe was the first
                   // one -- it covers every unseekable stream, and a
                   // libc's fseek() turns it straight into the errno a
                   // program expects
#define ENFILE 23  // a SYSTEM-wide table is full (the pipe table)
#define EMFILE 24  // THIS PROCESS's descriptor table is full -- distinct from
                   // ENFILE, and distinct from ENOENT, which is the whole
                   // reason this header exists
#define ENOTDIR 20 // a path component that had to be a directory is not one --
                   // chdir() into a file, distinct from ENOENT, which is
                   // the difference between "wrong" and "absent"
#define EISDIR  21 // the target IS a directory and the operation is
                   // meaningless on one (truncate, hardlink)
#define ERANGE  34 // out of range, in POSIX's broad sense, and it has two
                   // users here. getcwd(): the answer does not fit the
                   // buffer, and a TRUNCATED path names a different
                   // directory rather than being a shorter answer to the
                   // same question. SYS_QUERY: the index names no record,
                   // which is what ENDS an enumeration -- distinct from
                   // ENOENT, which means the CLASS does not exist
#define ENAMETOOLONG 36 // the path is longer than FS_PATH_MAX once
                   // resolved. Its own code because it is the one path
                   // failure that says nothing about the filesystem: the
                   // file may well exist, this kernel just cannot name it
#define EAGAIN 11  // the call would have BLOCKED and the fd was asked not
                   // to. Only ever returned to a caller that set it
                   // (SYS_SET_NONBLOCK): the default is still to park,
                   // so nothing that predates this can see it. Distinct
                   // from SYS_RETRY, which is not an error at all -- see
                   // that constant's own comment
#define ENOTTY 25  // this fd is not a TERMINAL, and the call only means
                   // something on one -- tcsetpgrp(), tcsetattr(). Its
                   // own code because "that is not a terminal" and "you
                   // may not do that to this terminal" (EPERM) send a
                   // reader to completely different places: one is a
                   // wrong fd, the other is a live terminal somebody
                   // else owns
#define ENOSPC 28  // a SYSTEM-wide table with no free entry, where the
                   // caller's own limits are fine -- no terminal left to
                   // hand out. ENFILE's sibling; distinct from EMFILE,
                   // which is this process's own table
#define ENOSYS 38  // the call exists and does nothing yet
#define ENOTSUP 95 // the thing exists but does not support being asked
                   // THIS way -- a query class that is a LIST has no
                   // single value, so `config get providers` is not the
                   // same answer as "no such fact". Distinguishing them
                   // is the difference between sending a reader to the
                   // right tool and sending them hunting for a typo

#endif // ABI_ERRNO_H
