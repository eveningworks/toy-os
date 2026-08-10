#ifndef ELF_TEST_H
#define ELF_TEST_H

// Loads userland/hello.c (a real, separately compiled and linked ELF64
// binary, provided to the kernel as a Multiboot2 module -- see grub.cfg's
// `module2` line) into a fresh private address space and runs it in
// ring 3, the same way ring3_test_run() does for its 17 hand-encoded
// bytes -- except this time it's a real ELF, loaded via elf.c's PT_LOAD
// parser instead of bytes written directly into a page.
//
// Same halting behavior as ring3_test_run() and for the same reason:
// there's no syscall/exit path yet, so this can't recover after the
// program's deliberate hlt fault and hand control back to the shell.
void elf_test_run(void);

#endif
