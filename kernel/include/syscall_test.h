#ifndef SYSCALL_TEST_H
#define SYSCALL_TEST_H

// Loads userland/exit_test.c (a real ELF64 binary, provided as a second
// Multiboot2 module -- see grub.cfg) into a fresh private address space
// and runs it via process_run_ring3() (process.h). Unlike ring3test and
// elftest, this one actually RETURNS -- exit_test.c calls the exit
// syscall instead of deliberately faulting, so control comes back
// cleanly and the shell keeps running afterward. Prints the exit code
// it got back.
void syscall_test_run(void);

#endif
