#ifndef ELF_RUN_H
#define ELF_RUN_H

// Loads and runs a real ELF64 binary straight from the persistent
// filesystem -- the disk-hosted counterpart to file_test.c/
// newsyscalls_test.c's GRUB-module-sourced ring-3 processes (see those
// for the exact same elf_load() + process_run_ring3() shape, just with
// fs_read() standing in for multiboot_get_module()). Prints its own
// progress/errors to the console the same way those do.
//
// Returns the process's real exit code (see process_run_ring3()'s doc
// comment for what a negative value means -- PROCESS_CRASHED, in
// practice) on success, or -1 if the binary couldn't even be loaded
// (bad path, corrupt/non-ELF64 file, out of memory) -- nothing ever
// runs in that case.
int elf_run_from_fs(const char *path);

#endif
