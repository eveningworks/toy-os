#ifndef UPATH_H
#define UPATH_H

// PATH lookup for a ring-3 program that has to RUN another one.
//
// **EXTRACTED WHEN IT GOT ITS SECOND REAL CALLER**, which is the bar
// this project sets for anything shared: `/bin/tosh` had it as a static
// function for as long as it was the only thing resolving a command
// name, and `/bin/strace` -- which takes a command name exactly as the
// shell does, and must find the same program the shell would -- is what
// made a third hand-rolled copy the alternative. There is already a
// fourth-in-waiting: `apps/shell_path.c` is the KERNEL shell's version
// of the same search, and it cannot use this one (ring 0, and it reads
// PATH out of `/etc/toyos.conf`). That divergence is real and is why
// the search ORDER is stated in both places rather than derived.
//
// The list is fixed here rather than read from the environment: a
// program that inherits a mangled PATH and silently runs the wrong
// binary is worse than one that always looks in the same three places.
// When ring 3 grows a real PATH, this is the one function that changes.
#define UPATH_MAX 64 // FS_PATH_MAX -- every caller-side path budget here

// Finds the program `name` names, writing the path into `out` (`cap`
// bytes). A name containing '/' is a path and is used as given.
//
// **THREE ANSWERS, NOT TWO**: 1 if `out` names a program, 0 if no PATH
// entry had it, and -1 if the search could not be COMPLETED. The third
// is not a shade of "no" -- until open() could say why it refused, a
// machine with a full fd table reported "not found" for a command that
// was sitting right there, because every probe failed for a reason that
// had nothing to do with the candidate. See docs/errno-design.md.
int upath_find_program(const char *name, char *out, int cap);

#endif
