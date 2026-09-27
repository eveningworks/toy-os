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
#define O_EXCL   SYS_O_EXCL

// **O_NONBLOCK IS A fcntl(F_SETFL) FLAG, NOT AN open() ONE**, and the
// distinction is real rather than pedantic: non-blocking is set on an
// EXISTING descriptor (SYS_SET_NONBLOCK). open() REFUSES it below
// rather than accepting and ignoring it, which is the failure this
// library declines elsewhere -- a caller who passes it at open time
// and is told yes would then block.
#define O_NONBLOCK 0x00000800

// **CREATE, OR FAIL IF IT ALREADY EXISTS** -- only meaningful with
// O_CREAT, as POSIX says. The atomicity is the whole point: it is how a
// program claims a lock file without a window in which two of them both
// see it absent. Enforced in the kernel, not by a stat-then-open here,
// which would have exactly that window.

// Open `path`. The variadic `mode` argument POSIX requires with O_CREAT
// is accepted and IGNORED -- a file is created with the default for its
// type (kernel/fs/tfs3_internal.h's T3_MODE_DEFAULT) because there is no umask
// applied at creation and no chmod afterwards -- and is present so the
// universal `open(p, O_CREAT|O_WRONLY, 0644)` compiles unchanged.
int open(const char *path, int flags, ...);

// --- fcntl ------------------------------------------------------------
//
// **EVERY COMMAND HERE TAKES AN int, WHICH IS WHY THIS IS A FUNCTION
// AND ioctl() IS NOT.** <sys/ioctl.h> explains at length that a single
// entry point taking an untyped POINTER and an integer command cannot
// be checked at a ring boundary. fcntl's commands take integers and
// return integers, so there is nothing to validate and nothing to get
// wrong -- it forwards to typed syscalls exactly as ioctl() does.
//
// What is NOT here: the locking commands (F_GETLK/F_SETLK/F_SETLKW).
// There is no file locking in this system, and a lock that silently
// succeeded would be worse than none.
#define F_DUPFD  0
#define F_GETFD  1
#define F_SETFD  2
#define F_GETFL  3
#define F_SETFL  4

// The only descriptor flag there is.
#define FD_CLOEXEC 1

// Returns a value that depends on the command, or -1 with errno.
//   F_DUPFD  arg = the lowest descriptor the copy may take
//   F_GETFD  returns FD_CLOEXEC or 0
//   F_SETFD  arg = FD_CLOEXEC or 0
//   F_GETFL  returns the file-status flags (O_NONBLOCK is the only one
//            this system tracks)
//   F_SETFL  arg = the flags to set; only O_NONBLOCK is honoured, and
//            anything else is REFUSED with EINVAL rather than ignored
int fcntl(int fd, int cmd, ...);

// fcntl() itself is absent. Its two common uses are covered by named
// calls that cannot be got wrong -- dup()/dup2() for F_DUPFD and
// set_nonblock() for F_SETFL -- and the rest (locks, F_GETOWN, the
// close-on-exec flag) rest on machinery this kernel does not have.

#endif
