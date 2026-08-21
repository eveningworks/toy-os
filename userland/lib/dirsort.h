#ifndef USERLAND_LIB_DIRSORT_H
#define USERLAND_LIB_DIRSORT_H

#include "syscall_abi.h" // struct sys_dirent

// Ordering a directory listing, in one place.
//
// WHY THIS IS SHARED RATHER THAN WRITTEN TWICE. SYS_LISTDIR returns
// entries in whatever order the filesystem walks them, which is not an
// order at all -- the same directory can list differently on two
// machines, and nothing a person reads should do that. So both /bin/ls
// and Notepad's file dialog sort, and THE TWO MUST AGREE: a test that
// derives a row index from `ls` output and then clicks that row in the
// dialog is only correct while they do. They disagreed for exactly one
// build, and that test is what caught it.
//
// IT SORTS THE ENTRIES, NOT A PERMUTATION, and that is a deliberate
// retreat. A permutation moves less memory -- a struct sys_dirent is a
// 64-byte name plus a size and a timestamp -- but then every caller has
// to apply it, and applying one in place is a cycle walk that is easy to
// get subtly wrong. This one WAS got wrong, and produced a listing that
// looked plausible and was in the wrong order. At SYS_LISTDIR_MAX
// entries the copying is not worth a bug of that shape.
enum dirsort_key {
    DIRSORT_NAME, // ascending, and the tie-break for the other two
    DIRSORT_TIME, // newest first, as coreutils' -t
    DIRSORT_SIZE, // largest first, as coreutils' -S
};

void dirsort(struct sys_dirent *e, int n, enum dirsort_key key, int reverse);

#endif // USERLAND_LIB_DIRSORT_H
