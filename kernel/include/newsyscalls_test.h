#ifndef NEWSYSCALLS_TEST_H
#define NEWSYSCALLS_TEST_H

// newsyscalls_test_run(): loads and runs userland/newsyscalls_test.c
// (see grub.cfg's module2 lines) -- a ring-3 program that exercises the
// four newest syscalls: SYS_UNLINK, SYS_LISTDIR, SYS_GETTIME, SYS_YIELD
// (see syscall_abi.h). Same shape as file_test_run()/syscall_test_run()
// -- returns normally once the process exits, prints the exit code.
void newsyscalls_test_run(void);

#endif
