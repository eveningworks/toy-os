#ifndef GUI_H
#define GUI_H

// The GUI app's entry point. Registered in apps/apps.c as "gui" -- the
// shell's `gui` command launches it via app_run("gui"). Hands off to the
// window manager (see wm.h); returns when the user presses Esc inside it
// (or immediately if no framebuffer is available). The caller is
// responsible for redrawing the text console afterward (see apps/shell.c).
void gui_main(void);

#endif
