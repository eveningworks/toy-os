#ifndef TASKBAR_CONFIG_H
#define TASKBAR_CONFIG_H

// Registers `desktop.taskbar_height` (px, TASKBAR_H_MIN..TASKBAR_H_MAX).
// Persist-only: the taskbar is drawn by a ring-3 process, which notices
// the change on its own generation poll (userland/wm/wm_taskbar.c).
#define TASKBAR_H_MIN  24
#define TASKBAR_H_MAX  96
#define TASKBAR_H_STEP 2

// The height with nothing written. A PIXEL CONSTANT, not font-derived:
// the registry answers it from ring 0 and the strip is drawn in ring 3,
// and the two tiers' fonts do not share a line height -- a formula gave
// 36 on one side and 40 on the other. Between KDE's 44 and XFCE's 26.
#define TASKBAR_H_DEFAULT 40

void taskbar_setting_register(void);

#endif
