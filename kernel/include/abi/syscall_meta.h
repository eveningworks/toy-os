#ifndef ABI_SYSCALL_META_H
#define ABI_SYSCALL_META_H
// What a syscall's arguments and return value ARE, for a tracer to print
// them: the kinds abi/syscall_rows.h lists per syscall. Shared because
// the decoder is /bin/strace's (userland/lib/utrace.h) and the rows are
// the kernel's.

// How one argument prints. A_END must be 0, so an unlisted argument is
// absent rather than garbage.
enum sc_arg {
    A_END = 0, // no more arguments
    A_INT,     // signed decimal
    A_HEX,     // pointer/opaque, as hex
    A_FD,      // a file descriptor -- decimal, but named for readability
    A_PATH,    // pointer to a NUL-terminated path, printed as a quoted string
    A_BUF,     // pointer to a byte buffer whose length is the NEXT argument
    A_OFLAGS,  // SYS_O_* bitmask
};

enum sc_ret { R_DEC = 0, R_HEX };

#endif
