#ifndef TASKBAR_MENU_H
#define TASKBAR_MENU_H

// The two right-click menus the taskbar owns that are not about a
// window (chosen from mockups, 2026-10-05):
//
//   the EMPTY STRIP -- Task Manager, then the taskbar's own options in
//   place (Combine buttons, Button position, Buttons, Floating panel),
//   then Taskbar settings. Windows 11 offers only the two ends; the
//   options in between are KDE's "configure the panel where you are".
//
//   the START BUTTON -- the system tools, then Leave: Windows' Win+X
//   menu, with toy-os's own programs.
//
// Both are context_menu.c menus; their rows are this file's.

void taskbar_menu_open_strip(int mx, int my);
void taskbar_menu_open_start(int mx, int my);

#endif // TASKBAR_MENU_H
