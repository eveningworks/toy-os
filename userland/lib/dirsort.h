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

// The comparison dirsort() sorts BY, exposed on its own. <0, 0 or >0
// for a before/equal/after b, with `name` as the tie-break exactly as
// above.
//
// It exists because ui/uui_fileview.c sorts a VIEW rather than the
// array -- `uui_table` owns a permutation and asks the app to compare
// (uui_table.h says why), so it cannot call dirsort() at all. Sharing
// the comparison rather than reimplementing it is what keeps a file
// view's order identical to `/bin/ls`'s, which is the property this
// whole file exists to protect.
//
// NOTE THE DIRECTIONS, which are coreutils' and not a table header's:
// DIRSORT_SIZE puts the LARGEST first and DIRSORT_TIME the NEWEST, the
// way `ls -S` and `ls -t` do. A column header whose ascending arrow
// must mean smallest-first negates this rather than adding a third
// convention -- see uui_fileview.c.
int dirsort_cmp(const struct sys_dirent *a, const struct sys_dirent *b,
                 enum dirsort_key key);

#endif // USERLAND_LIB_DIRSORT_H
