#ifndef PTR_TEST_H
#define PTR_TEST_H

// Loads userland/write_bad_test.c (GRUB's fourth Multiboot2 module) -- a
// process that deliberately passes a kernel-only pointer (0x1000,
// present in every process's shared low address range, but never
// user-accessible) to the write syscall -- and confirms the kernel's
// pointer validation (vmm_validate_user_range(), see vmm.h) actually
// rejects it, instead of just assuming it would.
void ptr_test_run(void);

#endif
