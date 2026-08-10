#ifndef WIN_TEST_H
#define WIN_TEST_H

// win_test_run(): loads and runs userland/win_test.c (see grub.cfg's
// module2 lines) -- a ring-3 program that uses SYS_WIN_CREATE +
// SYS_WIN_PRESENT (see syscall_abi.h) instead of SYS_GUI_INIT
// (gui_test.c). The difference is the whole point: this process never
// gets the real framebuffer mapped into its address space at all, only
// its own private pixel buffer. The kernel is the one that composites
// that buffer, plus a real title bar and close button, onto the real
// screen -- a genuine client/server split, closer to how a real
// windowing protocol works than gui_test.c's "hand over the whole
// screen" approach.
//
// Still modal, same limitation as guitest: see syscall_abi.h's comment
// on SYS_WIN_CREATE for why (no concurrency with the kernel-space
// window manager yet).
void win_test_run(void);

#endif
