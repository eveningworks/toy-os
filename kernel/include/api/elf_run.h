#ifndef ELF_RUN_H
#define ELF_RUN_H

#include <stdint.h>
#include <stddef.h>

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
// with `path` itself becoming argv[0]. The STRING form: split by
// elf_argv_from_string() below into the vector the loader lays out, so
// a run of non-space characters is one argument. A caller with a real
// argv (a shell that quotes) goes through SYS_SPAWN's SPAWN_ARGV.
//
// Returns the process's real exit code (see process_run_ring3()'s doc
// comment for what a negative value means -- PROCESS_CRASHED, in
// practice) on success, or -1 if the binary couldn't even be loaded
// (bad path, corrupt/non-ELF64 file, out of memory, or `args` too long
// to fit in the one stack page) -- nothing ever runs in that case.
int elf_run_from_fs(const char *path, const char *args);

// Converts the whitespace-separated string form of arguments into the
// VECTOR elf_build_argv_on_stack() takes: "path\0tok1\0tok2\0", with
// `path` as argv[0], its length in `*out_len`. NULL/"" `args` gives a
// one-entry vector. Returns 0 if it does not fit `cap` -- refused, never
// truncated. This is the ONE place the string form is split; everything
// below it carries a vector.
int elf_argv_from_string(const char *path, const char *args, char *out, size_t cap,
                         size_t *out_len);

// The argv-layout half of elf_run_from_fs() above, exposed on its own
// for scheduler.c's spawn_from_fs() (the scheduler's non-blocking
// counterpart) to reuse. `argv` is a VECTOR: `argv_len` bytes of
// NUL-terminated strings back to back, argv[0] first
// (elf_argv_from_string() builds one from the string form); a NULL or
// zero-length one means argv = {path}. Sized by LENGTH rather than a
// terminator because an entry may be empty (abi/syscall_abi.h).
// Writes the layout into the identity-mapped stack page at `stack_phys`/
// `stack_vaddr` (a single already-allocated+mapped page, `stack_vaddr`
// being that page's address in the target process's OWN address
// space), and returns via out-params: `*out_argc`, `*out_argv` (a
// *vaddr*, since ring-3 code dereferences it), and `*out_user_rsp`
// (the initial user-mode RSP -- always equal to `*out_argv`, since the
// argv pointer array sits at the top of what's actually been written,
// with nothing above it a process would need to `push` over). Returns
// 1 on success, 0 if the strings + pointer arrays don't fit in the one
// 4096-byte page -- callers must treat that as a hard failure, not
// silently truncate. There is no separate count limit: the page is it.
// `env` is the child's ENVIRONMENT: a NUL-separated run of "KEY=VALUE"
// strings terminated by an empty one, or NULL for none. One blob rather
// than a char** array, so the kernel copies a single validated string
// instead of walking a pointer array in user memory and validating each
// entry -- the same shape `args` already has.
//
// It is passed EXPLICITLY on every spawn and the kernel stores none of
// it. Inheritance is tolibc's job, exactly as execve() is the primitive
// on Unix and execv() is the libc function that passes `environ` for
// you. See docs/decisions.md.
int elf_build_argv_on_stack(uint64_t stack_phys, uint64_t stack_vaddr,
                             const char *path, const char *argv, size_t argv_len,
                             const char *env,
                             const uint64_t (*auxv)[2], int auxc,
                             uint64_t *out_argc, uint64_t *out_argv,
                             uint64_t *out_user_rsp);

#endif
