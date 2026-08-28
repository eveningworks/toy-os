#ifndef ULIB_SYS_MMAN_H
#define ULIB_SYS_MMAN_H

// mmap()/munmap() over SYS_MMAP. The constants are the kernel's
// SYS_PROT_*/SYS_MAP_* verbatim (which are Linux's on purpose --
// abi/syscall_abi.h says why), restated here rather than included
// because a libc header pulling in the raw syscall ABI would hand
// every ported program the whole kernel interface.
//
// What this mmap DOES NOT do, so a port finds out here rather than in
// a debugger: no MAP_SHARED (every mapping is private, writes never
// reach the file), no mprotect() yet, and munmap()'s range must lie
// within one mapping. A file is snapshot-at-first-touch per page, not
// coherent with later writes to it.

#include <sys/types.h>
#include <stdint.h>

#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

#define MAP_FAILED ((void *)-1)

void *mmap(void *addr, size_t length, int prot, int flags,
           int fd, long offset);
int munmap(void *addr, size_t length);

#endif
