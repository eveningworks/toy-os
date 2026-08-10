#ifndef ECHO_TEST_H
#define ECHO_TEST_H

// echo_test_run(): loads and runs userland/echo.c (see grub.cfg's
// module2 lines) -- a small interactive ring-3 program that exercises
// two syscalls no earlier test used: SYS_READ_KEY (keyboard input read
// straight from ring 3, no kernel-space console involved) and SYS_SBRK
// (a per-process heap). Typed characters get echoed back to the console
// by the process itself, via SYS_WRITE, live -- this is the first test
// command where what you type actually shows up because a ring-3
// program is reading it, not the kernel's own keyboard_read_line().
// Esc exits the process cleanly via SYS_EXIT, same as write_test.c's
// pattern (process_run_ring3() returns normally, the shell keeps
// running afterwards).
void echo_test_run(void);

#endif
