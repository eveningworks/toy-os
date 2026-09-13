#ifndef ULIB_FCNTL_H
#define ULIB_FCNTL_H

// POSIX's <fcntl.h>: open(), and the flags for it.
//
// THE FLAG NAMES ARE THE ONLY NEW THING. SYS_OPEN has always taken
// exactly these bits under SYS_* names (abi/syscall_abi.h); this header
// gives them the spelling every program is written against, one list
// still, aliased rather than re-numbered.
//
// O_RDONLY IS 0, WHICH IS WHY THE KERNEL HAS NO NAME FOR IT. "Not
// SYS_O_WRITE" is read-only, so a program passing O_RDONLY passes 0 and
// gets what it asked for. O_RDWR is the one that has to be spelled out
// below.
#include <syscall_abi.h>
#include <sys/types.h>

#define O_RDONLY 0
#define O_WRONLY SYS_O_WRITE
// READ AND WRITE ON ONE FD IS NOT A MODE THIS KERNEL HAS -- an open is
// either a reader or a writer (abi/syscall_abi.h).
//
// **O_RDWR THEREFORE FAILS THE open(), and it used to be an alias for
// O_WRONLY.** The old reasoning was that a writable fd lets ported code
// compile and the eventual read fails with EBADF, naming itself. What
// that missed is O_TRUNC: `open(p, O_RDWR | O_CREAT | O_TRUNC)` is the
// ordinary way to open a file for update, and aliasing destroyed the
// file's contents BEFORE the read failed. A refusal that costs nothing
// beats a success that costs the data.
//
// It has a bit of its own so open() can recognise and reject it with
// EINVAL, rather than being indistinguishable from O_WRONLY. Defined
// rather than omitted because leaving it out breaks the BUILD of
// anything that opens for update, and a compile error names the wrong
// thing -- this way the program builds and the failure arrives with an
// errno that says what happened.
#define O_RDWR   0x40000000

#define O_CREAT  SYS_O_CREAT
#define O_TRUNC  SYS_O_TRUNC
#define O_APPEND SYS_O_APPEND

// O_NONBLOCK is NOT an open flag here: non-blocking is set on an
// existing fd (SYS_SET_NONBLOCK), which is fcntl(F_SETFL)'s job on a
// real system. <unistd.h> has set_nonblock() for it. Defining an
// O_NONBLOCK that open() ignored would be the exact failure this
// library refuses elsewhere.

// Open `path`. The variadic `mode` argument POSIX requires with O_CREAT
// is accepted and IGNORED -- there are no permission bits on this
// filesystem (docs/filesystem-layout.md) -- and is present so that the
// universal `open(p, O_CREAT|O_WRONLY, 0644)` compiles unchanged.
int open(const char *path, int flags, ...);

// fcntl() itself is absent. Its two common uses are covered by named
// calls that cannot be got wrong -- dup()/dup2() for F_DUPFD and
// set_nonblock() for F_SETFL -- and the rest (locks, F_GETOWN, the
// close-on-exec flag) rest on machinery this kernel does not have.

#endif
