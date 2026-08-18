#ifndef GUI3_H
#define GUI3_H

// The desktop. Spawns /bin/wm/system/toywm -- the window manager, which
// is an ordinary ring-3 process -- and waits for it to exit.
//
// Registered in apps/apps.c as "gui" (and "gui3", an alias kept so
// existing notes and scripts keep selecting what they meant). Returns
// when the desktop exits, whether that is the user leaving it, the
// process being killed, or it faulting; the caller redraws the text
// console afterwards.
//
// There is no ring-0 desktop any more: `userland/wm/` was deleted once the
// ring-3 one passed all 23 GUI tools. See docs/wm-ring3-design.md.
void gui3_main(void);

#endif
