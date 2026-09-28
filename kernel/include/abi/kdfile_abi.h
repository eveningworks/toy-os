#ifndef KDFILE_ABI_H
#define KDFILE_ABI_H

// FILES SENT THROUGH THE KERNEL DEBUGGER -- gdb's `remote put` -- and the
// contract between the kernel, which STAGES them (kernel/debug/
// kdebug_files.c: bytes copied into RAM while the machine is stopped,
// because the stopped stub may not touch the filesystem), and /bin/kdfiled,
// which writes each one to disk once the machine runs again.
//
// SYS_KDFILE, RDI = the op:
//   KDFILE_TAKE  RSI = struct kdfile_info *. Fills it with the next
//                COMPLETE staged file and returns its handle (> 0). Blocks
//                while none is ready and returns 0 when woken -- ask again.
//                -ENODEV when the debugger is not armed on this boot.
//   KDFILE_READ  RSI = struct kdfile_read *. Copies up to `len` bytes of
//                the file at `offset` into `buf`; returns how many.
//   KDFILE_DONE  RSI = the handle, RDX = 0 written, or -errno. Releases
//                the staged copy and logs the outcome.

#include <stdint.h>

#define KDFILE_TAKE 0
#define KDFILE_READ 1
#define KDFILE_DONE 2

#define KDFILE_PATH_MAX 256

struct kdfile_info {
    char     path[KDFILE_PATH_MAX];   // absolute, as gdb named it
    uint64_t size;
    char     sha256[65];              // hex, of the bytes as staged
};

struct kdfile_read {
    int32_t  handle;
    uint32_t len;
    uint64_t offset;
    uint64_t buf;                     // a user pointer
};

#endif
