#ifndef SOCKET_TEST_H
#define SOCKET_TEST_H

// socket_test_run(): loads and runs userland/socket_test.c (see
// grub.cfg's module2 lines) -- a ring-3 program that exercises the new
// socket-fd syscalls (SYS_SOCKET/SYS_SEND/SYS_RECV, see syscall_abi.h).
// There's no NIC driver or protocol stack yet, so this proves the
// fd/syscall SURFACE behaves correctly (a real socket fd gets
// allocated, SYS_SEND/SYS_RECV are reachable and correctly report "no
// transport yet", SYS_WRITE/SYS_READ correctly reject a socket fd, and
// SYS_CLOSE tears it down) rather than that data actually moves
// anywhere. Returns (not interactive or modal, same shape as
// filetest/newsyscalltest).
void socket_test_run(void);

#endif
