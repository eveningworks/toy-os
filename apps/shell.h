#ifndef SHELL_H
#define SHELL_H

// The shell app's entry point. Registered in apps/apps.c as "shell" --
// this is what apps_start() launches after the kernel finishes hardware
// bring-up. Runs forever (it's a read-eval-print loop).
void shell_main(void);

#endif
