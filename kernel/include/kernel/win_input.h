#ifndef KERNEL_WIN_INPUT_H
#define KERNEL_WIN_INPUT_H

// Raw input for a ring-3 compositor. See kernel/proc/win_input.c for the
// gap this closes and why it is silent while a ring-0 WM is up.
//
// Kernel-internal: it reads the input devices, which nothing in apps/ or
// userland/ may do.

// Polls the mouse and keyboard and pushes WIN_EV_RAW_* to the registered
// compositor. A no-op unless there is one AND no ring-0 presentation
// layer is registered -- while the WM is in ring 0 it polls these
// devices itself, and a second consuming read would steal its keys.
//
// Called from scheduler_idle(). Cheap enough to run there: with no
// compositor it is one comparison.
void win_input_poll(void);

#endif
