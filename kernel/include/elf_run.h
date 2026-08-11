#ifndef ELF_RUN_H
#define ELF_RUN_H

// Loads and runs a real ELF64 binary straight from the persistent
// filesystem -- the shared, generic path every /bin binary now runs
// through via the shell's `run <name>` (apps/shell_sys.c's cmd_run()).
// Used to be one of several near-identical elf_load()+
// process_run_ring3() wrappers (file_test.c, newsyscalls_test.c, and
// the rest of the old GRUB-module test harnesses each had their own);
// those were removed once every test binary moved to /bin and started
// running through this single function instead -- see
// docs/decisions.md. Arms SYS_SBRK unconditionally (see this file's
// own top comment) so any binary can use sbrk()-based allocation, and
// prints its own progress/errors to the console.
//
// `args` is an optional, space-separated argument string (NULL or ""
// for none) -- e.g. "-l /docs" -- passed to the binary as argv[1..],
// with `path` itself becoming argv[0] (see process_run_ring3_args()'s
// doc comment in process.h for how argc/argv actually reach ring 3;
// this function is what builds the argv string/pointer-array layout on
// the process's stack page before handing off to it). No quoting
// support -- a run of non-space characters is one argument, same as
// the shell's own dispatch()/cmd_run() splitting.
//
// Returns the process's real exit code (see process_run_ring3()'s doc
// comment for what a negative value means -- PROCESS_CRASHED, in
// practice) on success, or -1 if the binary couldn't even be loaded
// (bad path, corrupt/non-ELF64 file, out of memory, or `args` too long
// to fit in the one stack page) -- nothing ever runs in that case.
int elf_run_from_fs(const char *path, const char *args);

#endif
