#ifndef ELF_RUN_H
#define ELF_RUN_H

#include <stdint.h>

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

// The argv-layout half of elf_run_from_fs() above, exposed on its own
// for scheduler.c's spawn_from_fs() (the scheduler's non-blocking
// counterpart) to reuse -- same "argv[0]=path plus each whitespace-
// separated token of `args`" convention, no quoting support. Writes
// the layout into the identity-mapped stack page at `stack_phys`/
// `stack_vaddr` (a single already-allocated+mapped page, `stack_vaddr`
// being that page's address in the target process's OWN address
// space), and returns via out-params: `*out_argc`, `*out_argv` (a
// *vaddr*, since ring-3 code dereferences it), and `*out_user_rsp`
// (the initial user-mode RSP -- always equal to `*out_argv`, since the
// argv pointer array sits at the top of what's actually been written,
// with nothing above it a process would need to `push` over). Returns
// 1 on success, 0 if `args` has too many tokens or the strings +
// pointer array don't fit in the one 4096-byte page -- callers must
// treat that as a hard failure, not silently truncate.
int elf_build_argv_on_stack(uint64_t stack_phys, uint64_t stack_vaddr,
                             const char *path, const char *args,
                             uint64_t *out_argc, uint64_t *out_argv,
                             uint64_t *out_user_rsp);

#endif
