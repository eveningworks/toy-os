#ifndef WRITE_TEST_H
#define WRITE_TEST_H

// Loads userland/write_test.c (a real ELF64 binary, GRUB's third
// Multiboot2 module -- see grub.cfg) into a fresh private address space
// and runs it via process_run_ring3() (process.h). Unlike syscall_test.c
// (exit only), this process prints its own message via a real write
// syscall *before* exiting -- the first userland program in this
// project whose console output the process produced itself, rather than
// the kernel narrating on its behalf.
void write_test_run(void);

#endif
