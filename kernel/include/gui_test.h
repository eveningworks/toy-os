#ifndef GUI_TEST_H
#define GUI_TEST_H

// Loads userland/gui_test.c (GRUB's fifth Multiboot2 module) -- the
// first userland program in this project to draw to the real screen and
// read real input from ring 3, via SYS_GUI_INIT and SYS_GUI_POLL_KEY.
//
// Modal: the process has the real screen to itself while it runs (see
// gui_test.c's own comment for why -- no scheduler yet). Fills the
// screen with a color, cycles it on each keypress, exits cleanly on
// 'q'. Returns when the process exits, same as syscall_test_run().
void gui_test_run(void);

#endif
